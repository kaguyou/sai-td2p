/*
 * SwitchBCM56860HW_vxlan.cpp — VXLAN / EVPN for the Trident2+ hardware switch.
 *
 * Owner: A4. Handles SAI TUNNEL, TUNNEL_MAP(+ENTRY), TUNNEL_TERM_TABLE_ENTRY,
 * BRIDGE (1D, record only), BRIDGE_PORT of type TUNNEL, and remote MACs on
 * tunnel bridge ports (hwFdbOnTunnel, called by the FDB handler).
 *
 * SONiC EVPN (vxlanmgrd / orchagent VxlanOrch + EvpnRemoteVnip2p) programs:
 *   1. one SAI_TUNNEL (VXLAN, P2MP, ENCAP_SRC_IP = local VTEP, UNDERLAY_INTERFACE
 *      = loopback RIF) with ENCAP/DECAP mapper lists (VLAN<->VNI maps),
 *   2. tunnel map entries VLAN_ID_TO_VNI / VNI_TO_VLAN_ID per L2 VNI,
 *   3. per remote VTEP: a P2P SAI_TUNNEL (ENCAP_DST_IP, PEER_MODE P2P, same
 *      mappers), a tunnel term entry (P2P: DST_IP = local, SRC_IP = remote),
 *      a SAI_BRIDGE_PORT of type TUNNEL (TUNNEL_ID = the P2P tunnel),
 *   4. type-2 MACs as FDB entries with BRIDGE_PORT_ID = that bridge port and
 *      ENDPOINT_IP = remote VTEP.
 *
 * Mapping to TD2+ (hwbcm_vxlan.c):
 *   VNI               -> VXLAN VPN (bcm VFI) + flood group        m_vniToVpn
 *   local VLAN member -> access VP in that VPN                    m_vpnAccessVps
 *   remote VTEP       -> tunnel initiator/terminator + VXLAN-only L3 egress
 *                        + one network VP (created in one VPN, added to every
 *                        other VPN's flood group)                 m_vtepByIp
 *   remote MAC        -> bcm_l2_addr (vid = VPN, port = network VP)
 *
 * The underlay next hop of a remote VTEP is resolved from the L3 state kept by
 * A3 (m_neighborEgr exact match, then longest prefix in m_routeEgr) and cloned
 * into a BCM_L3_VXLAN_ONLY egress. If it cannot be resolved yet, the network VP
 * stays pending (desired state kept) and is retried from hwVxlanRetryPending()
 * or when the next VNI appears.
 *
 * Scope of v1: L2 VNI (VLAN<->VNI), P2P remote VTEPs, remote MACs, IPv4
 * underlay. L3 VNI (VNI_TO_VIRTUAL_ROUTER_ID) and 1D bridges: logged as NOT
 * SUPPORTED, success returned so orchagent keeps going.
 */
#include "SwitchBCM56860HW.h"

#include "swss/logger.h"
#include "meta/sai_serialize.h"
#include "meta/NotificationPortStateChange.h"
#include "EventPayloadNotification.h"

#include "hwbcm/hwbcm.h"

#include <cstring>
#include <set>
#include <stdexcept>

using namespace saivs;

static const int VXLAN_UDP_DST_PORT = 4789;
static const int VXLAN_DEFAULT_TTL = 64;

// ---------------------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------------------

bool SwitchBCM56860HW::vxlanIp4Of(
        _In_ const sai_ip_address_t &ip,
        _Out_ uint8_t out[4]) const
{
    SWSS_LOG_ENTER();

    if (ip.addr_family != SAI_IP_ADDR_FAMILY_IPV4)
    {
        return false;
    }

    // sai_ip4_t is in network byte order: first octet first.
    memcpy(out, &ip.addr.ip4, 4);

    return true;
}

int SwitchBCM56860HW::vxlanVpnOfVid(
        _In_ int vid) const
{
    SWSS_LOG_ENTER();

    auto v = m_vidToVni.find(vid);

    if (v == m_vidToVni.end())
    {
        return -1;
    }

    auto p = m_vniToVpn.find(v->second);

    return (p == m_vniToVpn.end()) ? -1 : p->second;
}

sai_status_t SwitchBCM56860HW::vxlanEnsureInit()
{
    SWSS_LOG_ENTER();

    if (m_vxlanInited)
    {
        return SAI_STATUS_SUCCESS;
    }

    int rc = hwbcm_vxlan_init(VXLAN_UDP_DST_PORT);

    if (rc < 0)
    {
        SWSS_LOG_ERROR("hwbcm_vxlan_init failed: %d", rc);

        return SAI_STATUS_FAILURE;
    }

    // The router MAC must hit MY_STATION on any underlay VLAN so the outer
    // IPv4 header is terminated. A3's RIFs may already have done it; the
    // wrapper treats EXISTS as success.
    sai_attribute_t attr;

    if (getStoredAttr(SAI_OBJECT_TYPE_SWITCH, m_switch_id, SAI_SWITCH_ATTR_SRC_MAC_ADDRESS, attr))
    {
        int sid = -1;

        rc = hwbcm_vxlan_station_add(attr.value.mac, 0, &sid);

        if (rc < 0)
        {
            SWSS_LOG_WARN("my-station entry for the switch MAC failed: %d (continuing)", rc);
        }
        else
        {
            m_vxlanStationId = sid;
        }
    }
    else
    {
        SWSS_LOG_WARN("switch SRC_MAC not stored, no my-station entry added for VXLAN termination");
    }

    m_vxlanInited = true;

    SWSS_LOG_NOTICE("VXLAN initialized on TD2+ (UDP dst port %d)", VXLAN_UDP_DST_PORT);

    return SAI_STATUS_SUCCESS;
}

int SwitchBCM56860HW::vxlanResolveUnderlayEgress(
        _In_ const uint8_t ip4[4])
{
    SWSS_LOG_ENTER();

    uint32_t ip = 0;

    memcpy(&ip, ip4, 4); // network byte order, same as sai_ip4_t

    // 1. directly connected neighbor (exact match)
    for (const auto &kv: m_neighborEgr)
    {
        sai_neighbor_entry_t ne;

        memset(&ne, 0, sizeof(ne));

        try
        {
            sai_deserialize_neighbor_entry(kv.first, ne);
        }
        catch (const std::exception &e)
        {
            SWSS_LOG_WARN("cannot deserialize neighbor %s: %s", kv.first.c_str(), e.what());

            continue;
        }

        if (ne.ip_address.addr_family == SAI_IP_ADDR_FAMILY_IPV4 && ne.ip_address.addr.ip4 == ip)
        {
            SWSS_LOG_INFO("remote VTEP %u.%u.%u.%u is a direct neighbor, egress %d",
                    ip4[0], ip4[1], ip4[2], ip4[3], kv.second);

            return kv.second;
        }
    }

    // 2. longest prefix match in the routes A3 programmed
    int best = -1;
    int bestLen = -1;

    for (const auto &kv: m_routeEgr)
    {
        sai_route_entry_t re;

        memset(&re, 0, sizeof(re));

        try
        {
            sai_deserialize_route_entry(kv.first, re);
        }
        catch (const std::exception &e)
        {
            SWSS_LOG_WARN("cannot deserialize route %s: %s", kv.first.c_str(), e.what());

            continue;
        }

        if (re.destination.addr_family != SAI_IP_ADDR_FAMILY_IPV4)
        {
            continue;
        }

        uint32_t mask = re.destination.mask.ip4;

        if ((ip & mask) != (re.destination.addr.ip4 & mask))
        {
            continue;
        }

        int len = __builtin_popcount(mask);

        if (len > bestLen)
        {
            bestLen = len;
            best = kv.second;
        }
    }

    if (best >= 0)
    {
        SWSS_LOG_INFO("remote VTEP %u.%u.%u.%u reached via route (/%d), egress/ecmp %d",
                ip4[0], ip4[1], ip4[2], ip4[3], bestLen, best);
    }
    else
    {
        SWSS_LOG_NOTICE("no neighbor or route toward remote VTEP %u.%u.%u.%u yet",
                ip4[0], ip4[1], ip4[2], ip4[3]);
    }

    return best;
}

// ---------------------------------------------------------------------------
// remote VTEP: network VP life cycle
// ---------------------------------------------------------------------------

void SwitchBCM56860HW::vxlanAddVtepToVpn(
        _Inout_ VxlanRemoteVtep &vtep,
        _In_ int vpn)
{
    SWSS_LOG_ENTER();

    if (!vtep.vpReady || vtep.vpMc == -1 || vtep.floodVpns.count(vpn))
    {
        return;
    }

    int rc = hwbcm_vxlan_vpn_flood_set(vpn, vtep.vpMc, 1);

    if (rc < 0)
    {
        SWSS_LOG_ERROR("adding multicast network vp 0x%x of %s to flood group of vpn 0x%x failed: %d",
                vtep.vpMc, vtep.ip.c_str(), vpn, rc);

        return;
    }

    vtep.floodVpns.insert(vpn);
}

void SwitchBCM56860HW::vxlanInstallVtepMacs(
        _Inout_ VxlanRemoteVtep &vtep,
        _In_ int onlyVid)
{
    SWSS_LOG_ENTER();

    if (!vtep.vpReady)
    {
        return;
    }

    for (const auto &kv: vtep.macs)
    {
        const VxlanMac &m = kv.second;

        if (onlyVid >= 0 && m.vid != onlyVid)
        {
            continue;
        }

        int vpn = vxlanVpnOfVid(m.vid);

        if (vpn < 0)
        {
            continue;
        }

        int rc = hwbcm_vxlan_l2_addr_add(m.mac, vpn, vtep.vp, 1);

        if (rc < 0)
        {
            SWSS_LOG_WARN("remote MAC %s (vlan %d) on %s not installed: %d",
                    sai_serialize_mac(m.mac).c_str(), m.vid, vtep.ip.c_str(), rc);
        }
    }
}

bool SwitchBCM56860HW::vxlanTryBringUpVtep(
        _Inout_ VxlanRemoteVtep &vtep)
{
    SWSS_LOG_ENTER();

    if (vtep.vpReady)
    {
        return true;
    }

    if (!m_hwReady)
    {
        return false;
    }

    if (vtep.tunnelInit < 0 || vtep.tunnelTerm < 0)
    {
        SWSS_LOG_WARN("remote VTEP %s has no bcm tunnel yet", vtep.ip.c_str());

        return false;
    }

    if (m_vniToVpn.empty())
    {
        // bcm_vxlan_port_add needs a valid VPN; wait for the first VNI map.
        SWSS_LOG_NOTICE("remote VTEP %s: network VP deferred until the first VNI is mapped", vtep.ip.c_str());

        return false;
    }

    if (vtep.egr <= 0)
    {
        int l3 = vxlanResolveUnderlayEgress(vtep.ip4);

        if (l3 < 0)
        {
            SWSS_LOG_NOTICE("remote VTEP %s: underlay next hop unknown, network VP deferred", vtep.ip.c_str());

            return false;
        }

        int egr = -1;
        int port = -1;

        int rc = hwbcm_vxlan_egress_from_l3(l3, &egr, &port);

        if (rc < 0)
        {
            SWSS_LOG_WARN("remote VTEP %s: cannot build VXLAN egress from l3 egress %d: %d",
                    vtep.ip.c_str(), l3, rc);

            return false;
        }

        vtep.egr = egr;
        vtep.port = port;
        vtep.srcEgr = l3;
    }

    int vpn = m_vniToVpn.begin()->second;
    int vp = -1;

    int rc = hwbcm_vxlan_network_port_add(vpn, vtep.tunnelInit, vtep.tunnelTerm, vtep.egr, &vp);

    if (rc < 0)
    {
        SWSS_LOG_ERROR("remote VTEP %s: network VP creation failed: %d", vtep.ip.c_str(), rc);

        hwbcm_vxlan_egress_destroy(vtep.egr);

        vtep.egr = -1;
        vtep.port = -1;
        vtep.srcEgr = -1;

        return false;
    }

    vtep.vp = vp;
    vtep.vpReady = true;
    vtep.vpVpn = vpn;
    vtep.floodVpns.clear();

    // BUM copies: a second network VP with an IPMC egress is what goes into the
    // flood groups (TD2+ replication only encapsulates through an L3MC-view
    // next hop; the unicast VP above would flood the inner frame raw).
    {
        int egrMc = -1;
        int vpMc = -1;
        int mrc = hwbcm_vxlan_egress_from_l3_mc(vtep.srcEgr, &egrMc, NULL);
        if (mrc == 0)
        {
            mrc = hwbcm_vxlan_network_port_add_mc(vpn, vtep.tunnelInit, vtep.tunnelTerm, egrMc, &vpMc);
        }
        if (mrc < 0)
        {
            SWSS_LOG_ERROR("remote VTEP %s: multicast network VP creation failed: %d (no BUM toward it)",
                    vtep.ip.c_str(), mrc);
            if (egrMc > 0)
            {
                hwbcm_vxlan_egress_destroy(egrMc);
            }
            egrMc = -1;
            vpMc = -1;
        }
        else
        {
            vtep.floodVpns.insert(vpn);
            SWSS_LOG_NOTICE("remote VTEP %s: multicast network vp 0x%x (ipmc egress %d) in flood group of vpn 0x%x",
                    vtep.ip.c_str(), vpMc, egrMc, vpn);
        }
        vtep.egrMc = egrMc;
        vtep.vpMc = vpMc;
    }

    for (const auto &kv: m_vniToVpn)
    {
        vxlanAddVtepToVpn(vtep, kv.second);
    }

    for (auto bp: vtep.bridgePorts)
    {
        m_bridgePortToVp[bp] = vp;
    }

    vxlanInstallVtepMacs(vtep, -1);

    SWSS_LOG_NOTICE("remote VTEP %s up: network vp 0x%x (vpn 0x%x, egress %d, port %d, %zu MACs)",
            vtep.ip.c_str(), vp, vpn, vtep.egr, vtep.port, vtep.macs.size());

    vxlanNotifyTunnelOper(vtep, true);

    return true;
}

void SwitchBCM56860HW::vxlanNotifyTunnelOper(
        _In_ const VxlanRemoteVtep &vtep,
        _In_ bool up)
{
    SWSS_LOG_ENTER();

    /*
     * orchagent learns the oper status of an EVPN P2P tunnel ("show vxlan
     * remotevtep", STATE_DB VXLAN_TUNNEL_TABLE operstatus) from a port state
     * change notification whose port_id is the SAI_OBJECT_TYPE_TUNNEL oid
     * (PortsOrch::updateDbPortOperStatus -> VxlanTunnelOrch::updateDbTunnelOperStatus),
     * the way the Broadcom SAI reports it. Without this it stays oper_down forever.
     * SwitchStateBase::send_port_oper_status_notification() only accepts PORT oids,
     * so the event is queued here the same way it does it.
     */
    if (vtep.tunnels.empty())
    {
        return;
    }

    sai_attribute_t attr;

    attr.id = SAI_SWITCH_ATTR_PORT_STATE_CHANGE_NOTIFY;

    if (get(SAI_OBJECT_TYPE_SWITCH, m_switch_id, 1, &attr) != SAI_STATUS_SUCCESS || attr.value.ptr == NULL)
    {
        SWSS_LOG_WARN("remote VTEP %s: no port state change callback, tunnel oper status not reported",
                vtep.ip.c_str());
        return;
    }

    sai_switch_notifications_t sn = { };

    sn.on_port_state_change = (sai_port_state_change_notification_fn)attr.value.ptr;

    for (auto tunnel_id: vtep.tunnels)
    {
        sai_port_oper_status_notification_t data = { };

        data.port_id = tunnel_id;
        data.port_state = up ? SAI_PORT_OPER_STATUS_UP : SAI_PORT_OPER_STATUS_DOWN;

        auto str = sai_serialize_port_oper_status_ntf(1, &data);
        auto ntf = std::make_shared<sairedis::NotificationPortStateChange>(str);
        auto payload = std::make_shared<EventPayloadNotification>(ntf, sn);

        if (m_switchConfig->m_eventQueue)
        {
            m_switchConfig->m_eventQueue->enqueue(std::make_shared<Event>(EVENT_TYPE_NOTIFICATION, payload));
        }

        SWSS_LOG_NOTICE("remote VTEP %s: tunnel %s oper %s reported",
                vtep.ip.c_str(), sai_serialize_object_id(tunnel_id).c_str(), up ? "up" : "down");
    }
}

void SwitchBCM56860HW::vxlanTearDownVtepVp(
        _Inout_ VxlanRemoteVtep &vtep)
{
    SWSS_LOG_ENTER();

    if (vtep.vpReady)
    {
        for (const auto &kv: vtep.macs)
        {
            int vpn = vxlanVpnOfVid(kv.second.vid);

            if (vpn >= 0)
            {
                hwbcm_vxlan_l2_addr_delete(kv.second.mac, vpn);
            }
        }

        if (vtep.vpMc != -1)
        {
            for (int vpn: vtep.floodVpns)
            {
                if (vpn != vtep.vpVpn)
                {
                    hwbcm_vxlan_vpn_flood_set(vpn, vtep.vpMc, 0);
                }
            }
            int mrc = hwbcm_vxlan_port_delete(vtep.vpVpn, vtep.vpMc);
            if (mrc < 0)
            {
                SWSS_LOG_WARN("remote VTEP %s: multicast network vp 0x%x delete: %d", vtep.ip.c_str(), vtep.vpMc, mrc);
            }
            vtep.vpMc = -1;
        }
        if (vtep.egrMc > 0)
        {
            hwbcm_vxlan_egress_destroy(vtep.egrMc);
            vtep.egrMc = -1;
        }

        int rc = hwbcm_vxlan_port_delete(vtep.vpVpn, vtep.vp);

        if (rc < 0)
        {
            SWSS_LOG_WARN("remote VTEP %s: network vp 0x%x delete: %d", vtep.ip.c_str(), vtep.vp, rc);
        }

        for (auto bp: vtep.bridgePorts)
        {
            m_bridgePortToVp.erase(bp);
        }

        SWSS_LOG_NOTICE("remote VTEP %s: network vp 0x%x removed", vtep.ip.c_str(), vtep.vp);

        vxlanNotifyTunnelOper(vtep, false);

        vtep.vp = -1;
        vtep.vpReady = false;
        vtep.vpVpn = -1;
        vtep.floodVpns.clear();
    }

    if (vtep.egr > 0)
    {
        hwbcm_vxlan_egress_destroy(vtep.egr);

        vtep.egr = -1;
        vtep.port = -1;
        vtep.srcEgr = -1;
    }
}

sai_status_t SwitchBCM56860HW::hwVxlanRetryPending()
{
    SWSS_LOG_ENTER();

    if (!m_hwReady)
    {
        return SAI_STATUS_SUCCESS;
    }

    for (auto &kv: m_vtepByIp)
    {
        VxlanRemoteVtep &vtep = kv.second;

        if (vtep.bridgePorts.empty())
        {
            continue;
        }

        if (!vtep.vpReady)
        {
            vxlanTryBringUpVtep(vtep);

            continue;
        }

        // Already up: the underlay may have changed (A3 rewrites a shared
        // egress in place when a neighbor disappears/returns, or a better
        // route appeared). Rebuild the network VP when the clone is stale.
        int l3 = vxlanResolveUnderlayEgress(vtep.ip4);
        int stale = 0;

        if (l3 < 0)
        {
            continue; // nothing better known; keep what we have
        }

        // Compare contents, not ids: A3 may hand out a different egress/ecmp id
        // for the same next hop (route flap), which must not churn the VP.
        if (hwbcm_vxlan_egress_is_stale(l3, vtep.egr, &stale) < 0)
        {
            continue;
        }

        if (!stale)
        {
            vtep.srcEgr = l3;

            continue;
        }

        SWSS_LOG_NOTICE("remote VTEP %s: underlay egress %d is stale (resolved %d), rebuilding network VP",
                vtep.ip.c_str(), vtep.srcEgr, l3);

        vxlanTearDownVtepVp(vtep);
        vxlanTryBringUpVtep(vtep);
    }

    return SAI_STATUS_SUCCESS;
}

bool SwitchBCM56860HW::hwVxlanTranslateL2Event(
        _Inout_ int &vid,
        _Inout_ int &port,
        _Out_ int &trunk)
{
    SWSS_LOG_ENTER();

    trunk = -1;

    int isNetwork = 0;
    int phys = -1;
    int tid = -1;
    int vpn = -1;

    if (hwbcm_vxlan_vp_info(port, &isNetwork, &phys, &tid, &vpn) < 0)
    {
        // Not one of our VPs: a plain VLAN event passes, a foreign VFI is dropped.
        return vid >= 0 && vid <= 4095;
    }

    if (isNetwork)
    {
        return false; // remote MAC (EVPN type-2), never reported as a local learn
    }

    // VPN id -> VLAN. The event's vid is the VPN; trust the VP's VPN if they differ.
    int lookup = (vid > 4095) ? vid : vpn;

    for (const auto &kv: m_vniToVpn)
    {
        if (kv.second != lookup)
        {
            continue;
        }

        for (const auto &vv: m_vidToVni)
        {
            if (vv.second == kv.first)
            {
                vid = vv.first;
                port = phys;
                trunk = tid;

                return true;
            }
        }
    }

    SWSS_LOG_WARN("l2 event on access vp 0x%x: vpn 0x%x has no VLAN mapping, dropped", port, lookup);

    return false;
}

// ---------------------------------------------------------------------------
// access VPs (local VLAN members of a mapped VLAN)
// ---------------------------------------------------------------------------

sai_status_t SwitchBCM56860HW::vxlanAddAccessPort(
        _In_ sai_object_id_t member_oid,
        _In_ int vid,
        _In_ int vpn)
{
    SWSS_LOG_ENTER();

    for (const auto &a: m_vpnAccessVps[vpn])
    {
        if (a.member == member_oid)
        {
            return SAI_STATUS_SUCCESS;
        }
    }

    sai_attribute_t attr;

    if (!getStoredAttr(SAI_OBJECT_TYPE_VLAN_MEMBER, member_oid, SAI_VLAN_MEMBER_ATTR_BRIDGE_PORT_ID, attr))
    {
        SWSS_LOG_WARN("vlan member %s has no bridge port", sai_serialize_object_id(member_oid).c_str());

        return SAI_STATUS_SUCCESS;
    }

    sai_object_id_t target = bridgePortTarget(attr.value.oid);

    int tagged = 0;

    if (getStoredAttr(SAI_OBJECT_TYPE_VLAN_MEMBER, member_oid, SAI_VLAN_MEMBER_ATTR_VLAN_TAGGING_MODE, attr))
    {
        tagged = (attr.value.s32 == SAI_VLAN_TAGGING_MODE_TAGGED) ? 1 : 0;
    }

    int port = bcmPortOf(target);
    int trunk = -1;

    if (port < 0)
    {
        trunk = trunkOf(target);

        if (trunk < 0)
        {
            SWSS_LOG_WARN("vlan member %s: target %s is neither a known port nor a LAG, skipped",
                    sai_serialize_object_id(member_oid).c_str(),
                    sai_serialize_object_id(target).c_str());

            return SAI_STATUS_SUCCESS;
        }
    }

    int vp = -1;

    int rc = hwbcm_vxlan_access_port_add2(vpn, port, trunk, vid, tagged, &vp);

    if (rc < 0)
    {
        SWSS_LOG_ERROR("access VP for vlan %d port %d trunk %d in vpn 0x%x failed: %d", vid, port, trunk, vpn, rc);

        return SAI_STATUS_FAILURE;
    }

    VxlanAccessVp a;

    a.member = member_oid;
    a.vp = vp;

    m_vpnAccessVps[vpn].push_back(a);

    SWSS_LOG_NOTICE("access vp 0x%x: vlan %d port %d trunk %d %s -> vpn 0x%x",
            vp, vid, port, trunk, tagged ? "tagged" : "untagged", vpn);

    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchBCM56860HW::vxlanAddAccessPorts(
        _In_ int vid,
        _In_ int vpn)
{
    SWSS_LOG_ENTER();

    auto members = m_objectHash.find(SAI_OBJECT_TYPE_VLAN_MEMBER);

    if (members == m_objectHash.end())
    {
        return SAI_STATUS_SUCCESS;
    }

    auto md = sai_metadata_get_attr_metadata(SAI_OBJECT_TYPE_VLAN_MEMBER, SAI_VLAN_MEMBER_ATTR_VLAN_ID);

    sai_status_t worst = SAI_STATUS_SUCCESS;

    for (const auto &vm: members->second)
    {
        auto it = vm.second.find(md->attridname);

        if (it == vm.second.end())
        {
            continue;
        }

        if (vidOf(it->second->getAttr()->value.oid) != vid)
        {
            continue;
        }

        sai_object_id_t member_oid = SAI_NULL_OBJECT_ID;

        sai_deserialize_object_id(vm.first, member_oid);

        sai_status_t st = vxlanAddAccessPort(member_oid, vid, vpn);

        if (st != SAI_STATUS_SUCCESS)
        {
            worst = st;
        }
    }

    return worst;
}

void SwitchBCM56860HW::vxlanRemoveAccessPorts(
        _In_ int vpn)
{
    SWSS_LOG_ENTER();

    auto it = m_vpnAccessVps.find(vpn);

    if (it == m_vpnAccessVps.end())
    {
        return;
    }

    for (const auto &a: it->second)
    {
        int rc = hwbcm_vxlan_port_delete(vpn, a.vp);

        if (rc < 0)
        {
            SWSS_LOG_WARN("access vp 0x%x delete (vpn 0x%x): %d", a.vp, vpn, rc);
        }
    }

    m_vpnAccessVps.erase(it);
}

sai_status_t SwitchBCM56860HW::hwVxlanVlanMemberChanged(
        _In_ sai_object_id_t vlan_member_oid,
        _In_ bool add)
{
    SWSS_LOG_ENTER();

    if (!m_hwReady)
    {
        return SAI_STATUS_SUCCESS;
    }

    if (add)
    {
        sai_attribute_t attr;

        if (!getStoredAttr(SAI_OBJECT_TYPE_VLAN_MEMBER, vlan_member_oid, SAI_VLAN_MEMBER_ATTR_VLAN_ID, attr))
        {
            return SAI_STATUS_SUCCESS;
        }

        int vid = vidOf(attr.value.oid);
        int vpn = vxlanVpnOfVid(vid);

        if (vid < 0 || vpn < 0)
        {
            return SAI_STATUS_SUCCESS; // VLAN has no VNI: nothing to do
        }

        return vxlanAddAccessPort(vlan_member_oid, vid, vpn);
    }

    for (auto &kv: m_vpnAccessVps)
    {
        auto &list = kv.second;

        for (auto it = list.begin(); it != list.end(); ++it)
        {
            if (it->member != vlan_member_oid)
            {
                continue;
            }

            int rc = hwbcm_vxlan_port_delete(kv.first, it->vp);

            if (rc < 0)
            {
                SWSS_LOG_WARN("access vp 0x%x delete (vpn 0x%x): %d", it->vp, kv.first, rc);
            }

            SWSS_LOG_NOTICE("access vp 0x%x removed from vpn 0x%x (vlan member gone)", it->vp, kv.first);

            list.erase(it);

            return SAI_STATUS_SUCCESS;
        }
    }

    return SAI_STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// SAI_OBJECT_TYPE_TUNNEL
// ---------------------------------------------------------------------------

sai_status_t SwitchBCM56860HW::hwCreateTunnel(
        _In_ sai_object_id_t tunnel_id,
        _In_ uint32_t attr_count,
        _In_ const sai_attribute_t *attr_list)
{
    SWSS_LOG_ENTER();

    auto *type = findAttr(attr_count, attr_list, SAI_TUNNEL_ATTR_TYPE);

    if (type == nullptr || type->value.s32 != SAI_TUNNEL_TYPE_VXLAN)
    {
        SWSS_LOG_WARN("tunnel %s: type %d not VXLAN, NOT SUPPORTED in hardware (software only)",
                sai_serialize_object_id(tunnel_id).c_str(), type ? type->value.s32 : -1);

        return SAI_STATUS_SUCCESS;
    }

    if (!m_hwReady)
    {
        SWSS_LOG_WARN("hardware not ready, tunnel %s kept in software only", sai_serialize_object_id(tunnel_id).c_str());

        return SAI_STATUS_SUCCESS;
    }

    sai_status_t st = vxlanEnsureInit();

    if (st != SAI_STATUS_SUCCESS)
    {
        return st;
    }

    uint8_t src4[4] = {0, 0, 0, 0};
    bool haveSrc = false;

    auto *src = findAttr(attr_count, attr_list, SAI_TUNNEL_ATTR_ENCAP_SRC_IP);

    if (src != nullptr)
    {
        if (!vxlanIp4Of(src->value.ipaddr, src4))
        {
            SWSS_LOG_WARN("tunnel %s: IPv6 underlay NOT SUPPORTED, software only",
                    sai_serialize_object_id(tunnel_id).c_str());

            return SAI_STATUS_SUCCESS;
        }

        haveSrc = true;
    }
    else if (m_vtepSrcValid)
    {
        memcpy(src4, m_vtepSrcIp4, 4);
        haveSrc = true;
    }

    int ttl = VXLAN_DEFAULT_TTL;

    auto *ttlMode = findAttr(attr_count, attr_list, SAI_TUNNEL_ATTR_ENCAP_TTL_MODE);
    auto *ttlVal = findAttr(attr_count, attr_list, SAI_TUNNEL_ATTR_ENCAP_TTL_VAL);

    if (ttlMode != nullptr && ttlMode->value.s32 == SAI_TUNNEL_TTL_MODE_PIPE_MODEL && ttlVal != nullptr && ttlVal->value.u8 > 0)
    {
        ttl = ttlVal->value.u8;
    }

    int dscp = -1; // copy inner (uniform)

    auto *dscpMode = findAttr(attr_count, attr_list, SAI_TUNNEL_ATTR_ENCAP_DSCP_MODE);
    auto *dscpVal = findAttr(attr_count, attr_list, SAI_TUNNEL_ATTR_ENCAP_DSCP_VAL);

    if (dscpMode != nullptr && dscpMode->value.s32 == SAI_TUNNEL_DSCP_MODE_PIPE_MODEL && dscpVal != nullptr)
    {
        dscp = dscpVal->value.u8;
    }

    auto *peer = findAttr(attr_count, attr_list, SAI_TUNNEL_ATTR_PEER_MODE);
    auto *dst = findAttr(attr_count, attr_list, SAI_TUNNEL_ATTR_ENCAP_DST_IP);

    bool p2p = (peer != nullptr && peer->value.s32 == SAI_TUNNEL_PEER_MODE_P2P && dst != nullptr);

    if (!p2p)
    {
        // The local VTEP (P2MP) tunnel: nothing in hardware by itself.
        m_vtepTunnel = tunnel_id;

        if (haveSrc)
        {
            memcpy(m_vtepSrcIp4, src4, 4);
            m_vtepSrcValid = true;
        }

        m_vtepTtl = ttl;
        m_vtepDscp = dscp;

        SWSS_LOG_NOTICE("local VTEP tunnel %s: src %u.%u.%u.%u ttl %d dscp %d",
                sai_serialize_object_id(tunnel_id).c_str(), src4[0], src4[1], src4[2], src4[3], ttl, dscp);

        return SAI_STATUS_SUCCESS;
    }

    uint8_t dst4[4] = {0, 0, 0, 0};

    if (!vxlanIp4Of(dst->value.ipaddr, dst4))
    {
        SWSS_LOG_WARN("tunnel %s: IPv6 remote VTEP NOT SUPPORTED, software only",
                sai_serialize_object_id(tunnel_id).c_str());

        return SAI_STATUS_SUCCESS;
    }

    if (!haveSrc)
    {
        SWSS_LOG_ERROR("tunnel %s: P2P tunnel without ENCAP_SRC_IP and no local VTEP known",
                sai_serialize_object_id(tunnel_id).c_str());

        return SAI_STATUS_FAILURE;
    }

    std::string key = sai_serialize_ip_address(dst->value.ipaddr);

    VxlanRemoteVtep &vtep = m_vtepByIp[key];

    if (vtep.ip.empty())
    {
        vtep.ip = key;
        memcpy(vtep.ip4, dst4, 4);
    }

    if (vtep.tunnelInit < 0)
    {
        int init = -1;
        int term = -1;

        int rc = hwbcm_vxlan_tunnel_create2(src4, dst4, ttl, dscp, &init, &term);

        if (rc < 0)
        {
            SWSS_LOG_ERROR("tunnel %s to %s: bcm tunnel creation failed: %d",
                    sai_serialize_object_id(tunnel_id).c_str(), key.c_str(), rc);

            if (vtep.tunnels.empty() && vtep.bridgePorts.empty())
            {
                m_vtepByIp.erase(key);
            }

            return SAI_STATUS_FAILURE;
        }

        vtep.tunnelInit = init;
        vtep.tunnelTerm = term;
    }

    vtep.tunnels.insert(tunnel_id);

    m_tunnelRemoteIp[tunnel_id] = key;
    m_tunnelInit[tunnel_id] = vtep.tunnelInit;
    m_tunnelTerm[tunnel_id] = vtep.tunnelTerm;

    SWSS_LOG_NOTICE("P2P tunnel %s: %u.%u.%u.%u -> %s init 0x%x term 0x%x",
            sai_serialize_object_id(tunnel_id).c_str(), src4[0], src4[1], src4[2], src4[3],
            key.c_str(), vtep.tunnelInit, vtep.tunnelTerm);

    // A bridge port may already be waiting for this tunnel (unusual order).
    if (!vtep.bridgePorts.empty())
    {
        vxlanTryBringUpVtep(vtep);
    }

    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchBCM56860HW::hwRemoveTunnel(
        _In_ sai_object_id_t tunnel_id)
{
    SWSS_LOG_ENTER();

    m_tunnelInit.erase(tunnel_id);
    m_tunnelTerm.erase(tunnel_id);

    auto it = m_tunnelRemoteIp.find(tunnel_id);

    if (it == m_tunnelRemoteIp.end())
    {
        if (tunnel_id == m_vtepTunnel)
        {
            SWSS_LOG_NOTICE("local VTEP tunnel %s removed", sai_serialize_object_id(tunnel_id).c_str());

            m_vtepTunnel = SAI_NULL_OBJECT_ID;
        }

        return SAI_STATUS_SUCCESS;
    }

    std::string key = it->second;

    m_tunnelRemoteIp.erase(it);

    auto vt = m_vtepByIp.find(key);

    if (vt == m_vtepByIp.end())
    {
        return SAI_STATUS_SUCCESS;
    }

    VxlanRemoteVtep &vtep = vt->second;

    vtep.tunnels.erase(tunnel_id);

    if (!vtep.tunnels.empty())
    {
        return SAI_STATUS_SUCCESS;
    }

    if (!vtep.bridgePorts.empty())
    {
        SWSS_LOG_WARN("P2P tunnel %s removed while %zu bridge port(s) still reference remote VTEP %s",
                sai_serialize_object_id(tunnel_id).c_str(), vtep.bridgePorts.size(), key.c_str());
    }

    vxlanTearDownVtepVp(vtep);

    int rc = hwbcm_vxlan_tunnel_destroy(vtep.tunnelInit, vtep.tunnelTerm);

    if (rc < 0)
    {
        SWSS_LOG_WARN("bcm tunnel destroy for %s: %d", key.c_str(), rc);
    }

    vtep.tunnelInit = -1;
    vtep.tunnelTerm = -1;

    SWSS_LOG_NOTICE("P2P tunnel %s to %s removed", sai_serialize_object_id(tunnel_id).c_str(), key.c_str());

    if (vtep.bridgePorts.empty())
    {
        m_vtepByIp.erase(vt);
    }

    return SAI_STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// SAI_OBJECT_TYPE_TUNNEL_MAP / TUNNEL_MAP_ENTRY
// ---------------------------------------------------------------------------

sai_status_t SwitchBCM56860HW::hwCreateTunnelMap(
        _In_ sai_object_id_t map_id,
        _In_ uint32_t attr_count,
        _In_ const sai_attribute_t *attr_list)
{
    SWSS_LOG_ENTER();

    auto *type = findAttr(attr_count, attr_list, SAI_TUNNEL_MAP_ATTR_TYPE);

    m_tunnelMapType[map_id] = (type != nullptr) ? type->value.s32 : -1;

    SWSS_LOG_INFO("tunnel map %s type %d recorded", sai_serialize_object_id(map_id).c_str(), m_tunnelMapType[map_id]);

    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchBCM56860HW::hwRemoveTunnelMap(
        _In_ sai_object_id_t map_id)
{
    SWSS_LOG_ENTER();

    m_tunnelMapType.erase(map_id);

    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchBCM56860HW::hwCreateTunnelMapEntry(
        _In_ sai_object_id_t entry_id,
        _In_ uint32_t attr_count,
        _In_ const sai_attribute_t *attr_list)
{
    SWSS_LOG_ENTER();

    int32_t type = -1;

    auto *t = findAttr(attr_count, attr_list, SAI_TUNNEL_MAP_ENTRY_ATTR_TUNNEL_MAP_TYPE);

    if (t != nullptr)
    {
        type = t->value.s32;
    }
    else
    {
        auto *m = findAttr(attr_count, attr_list, SAI_TUNNEL_MAP_ENTRY_ATTR_TUNNEL_MAP);

        if (m != nullptr && m_tunnelMapType.count(m->value.oid))
        {
            type = m_tunnelMapType.at(m->value.oid);
        }
    }

    int vid = -1;
    uint32_t vni = 0;

    switch (type)
    {
        case SAI_TUNNEL_MAP_TYPE_VNI_TO_VLAN_ID:
            {
                auto *k = findAttr(attr_count, attr_list, SAI_TUNNEL_MAP_ENTRY_ATTR_VNI_ID_KEY);
                auto *v = findAttr(attr_count, attr_list, SAI_TUNNEL_MAP_ENTRY_ATTR_VLAN_ID_VALUE);

                if (k == nullptr || v == nullptr)
                {
                    SWSS_LOG_ERROR("map entry %s: VNI_TO_VLAN_ID without key/value", sai_serialize_object_id(entry_id).c_str());

                    return SAI_STATUS_FAILURE;
                }

                vni = k->value.u32;
                vid = v->value.u16;
            }
            break;

        case SAI_TUNNEL_MAP_TYPE_VLAN_ID_TO_VNI:
            {
                auto *k = findAttr(attr_count, attr_list, SAI_TUNNEL_MAP_ENTRY_ATTR_VLAN_ID_KEY);
                auto *v = findAttr(attr_count, attr_list, SAI_TUNNEL_MAP_ENTRY_ATTR_VNI_ID_VALUE);

                if (k == nullptr || v == nullptr)
                {
                    SWSS_LOG_ERROR("map entry %s: VLAN_ID_TO_VNI without key/value", sai_serialize_object_id(entry_id).c_str());

                    return SAI_STATUS_FAILURE;
                }

                vid = k->value.u16;
                vni = v->value.u32;
            }
            break;

        case SAI_TUNNEL_MAP_TYPE_VNI_TO_VIRTUAL_ROUTER_ID:
        case SAI_TUNNEL_MAP_TYPE_VIRTUAL_ROUTER_ID_TO_VNI:
            SWSS_LOG_WARN("map entry %s: L3 VNI (VNI<->VIRTUAL_ROUTER) NOT SUPPORTED on TD2+ v1 (needs RIOT), software only",
                    sai_serialize_object_id(entry_id).c_str());

            return SAI_STATUS_SUCCESS;

        case SAI_TUNNEL_MAP_TYPE_VNI_TO_BRIDGE_IF:
        case SAI_TUNNEL_MAP_TYPE_BRIDGE_IF_TO_VNI:
            SWSS_LOG_WARN("map entry %s: 1D bridge VNI NOT SUPPORTED (SONiC uses VLAN based VNIs), software only",
                    sai_serialize_object_id(entry_id).c_str());

            return SAI_STATUS_SUCCESS;

        default:
            SWSS_LOG_WARN("map entry %s: map type %d NOT SUPPORTED, software only",
                    sai_serialize_object_id(entry_id).c_str(), type);

            return SAI_STATUS_SUCCESS;
    }

    if (!m_hwReady)
    {
        return SAI_STATUS_SUCCESS;
    }

    if (vid <= 0 || vid > 4095 || vni > 0xFFFFFF)
    {
        SWSS_LOG_ERROR("map entry %s: invalid vlan %d / vni %u", sai_serialize_object_id(entry_id).c_str(), vid, vni);

        return SAI_STATUS_INVALID_PARAMETER;
    }

    sai_status_t st = vxlanEnsureInit();

    if (st != SAI_STATUS_SUCCESS)
    {
        return st;
    }

    auto old = m_vidToVni.find(vid);

    if (old != m_vidToVni.end() && old->second != vni)
    {
        SWSS_LOG_ERROR("vlan %d already mapped to vni %u, cannot map to %u", vid, old->second, vni);

        return SAI_STATUS_FAILURE;
    }

    if (m_vniToVpn.count(vni))
    {
        // Second direction of the same VLAN<->VNI pair.
        m_vniRefs[vni]++;
        m_mapEntryVidVni[entry_id] = std::make_pair(vid, vni);

        SWSS_LOG_INFO("map entry %s: vlan %d <-> vni %u (vpn 0x%x) ref %d",
                sai_serialize_object_id(entry_id).c_str(), vid, vni, m_vniToVpn.at(vni), m_vniRefs[vni]);

        return SAI_STATUS_SUCCESS;
    }

    int vpn = -1;

    int rc = hwbcm_vxlan_vpn_create(vni, vid, &vpn);

    if (rc < 0)
    {
        SWSS_LOG_ERROR("VPN for vni %u (vlan %d) failed: %d", vni, vid, rc);

        return SAI_STATUS_FAILURE;
    }

    m_vniToVpn[vni] = vpn;
    m_vidToVni[vid] = vni;
    m_vniRefs[vni] = 1;
    m_mapEntryVidVni[entry_id] = std::make_pair(vid, vni);

    SWSS_LOG_NOTICE("vni %u <-> vlan %d: vpn 0x%x created", vni, vid, vpn);

    // Local side: every current member of the VLAN becomes an access VP.
    st = vxlanAddAccessPorts(vid, vpn);

    if (st != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_WARN("some access VPs of vlan %d could not be created (vpn 0x%x stays)", vid, vpn);
    }

    // Remote side: known VTEPs join this VPN's flood group; pending ones can
    // now be brought up (a VPN exists).
    for (auto &kv: m_vtepByIp)
    {
        VxlanRemoteVtep &vtep = kv.second;

        if (!vtep.vpReady)
        {
            if (!vtep.bridgePorts.empty())
            {
                vxlanTryBringUpVtep(vtep); // installs all its MACs itself
            }
        }
        else
        {
            vxlanAddVtepToVpn(vtep, vpn);
            vxlanInstallVtepMacs(vtep, vid);
        }
    }

    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchBCM56860HW::hwRemoveTunnelMapEntry(
        _In_ sai_object_id_t entry_id)
{
    SWSS_LOG_ENTER();

    auto it = m_mapEntryVidVni.find(entry_id);

    if (it == m_mapEntryVidVni.end())
    {
        return SAI_STATUS_SUCCESS;
    }

    int vid = it->second.first;
    uint32_t vni = it->second.second;

    m_mapEntryVidVni.erase(it);

    auto ref = m_vniRefs.find(vni);

    if (ref != m_vniRefs.end())
    {
        if (--ref->second > 0)
        {
            SWSS_LOG_INFO("map entry %s removed, vni %u still referenced (%d)",
                    sai_serialize_object_id(entry_id).c_str(), vni, ref->second);

            return SAI_STATUS_SUCCESS;
        }

        m_vniRefs.erase(ref);
    }

    auto vp = m_vniToVpn.find(vni);

    if (vp == m_vniToVpn.end())
    {
        m_vidToVni.erase(vid);

        return SAI_STATUS_SUCCESS;
    }

    int vpn = vp->second;

    // bcm_vxlan_vpn_destroy() deletes every VP created in the VPN: network VPs
    // that were homed here are torn down and re-created in another VPN below.
    std::vector<std::string> rehome;

    for (auto &kv: m_vtepByIp)
    {
        VxlanRemoteVtep &vtep = kv.second;

        if (!vtep.vpReady)
        {
            continue;
        }

        if (vtep.vpVpn == vpn)
        {
            vxlanTearDownVtepVp(vtep);
            rehome.push_back(kv.first);
        }
        else if (vtep.floodVpns.count(vpn))
        {
            if (vtep.vpMc != -1)
            {
                hwbcm_vxlan_vpn_flood_set(vpn, vtep.vpMc, 0);
            }
            vtep.floodVpns.erase(vpn);
        }
    }

    vxlanRemoveAccessPorts(vpn);

    int rc = hwbcm_vxlan_vpn_destroy(vpn);

    if (rc < 0)
    {
        SWSS_LOG_ERROR("vpn 0x%x (vni %u) destroy failed: %d", vpn, vni, rc);
    }

    m_vniToVpn.erase(vp);
    m_vidToVni.erase(vid);

    SWSS_LOG_NOTICE("vni %u <-> vlan %d: vpn 0x%x destroyed", vni, vid, vpn);

    for (const auto &key: rehome)
    {
        auto vt = m_vtepByIp.find(key);

        if (vt != m_vtepByIp.end() && !vt->second.bridgePorts.empty())
        {
            vxlanTryBringUpVtep(vt->second);
        }
    }

    return rc < 0 ? SAI_STATUS_FAILURE : SAI_STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// SAI_OBJECT_TYPE_TUNNEL_TERM_TABLE_ENTRY
// ---------------------------------------------------------------------------

sai_status_t SwitchBCM56860HW::hwCreateTunnelTermEntry(
        _In_ sai_object_id_t term_id,
        _In_ uint32_t attr_count,
        _In_ const sai_attribute_t *attr_list)
{
    SWSS_LOG_ENTER();

    auto *action = findAttr(attr_count, attr_list, SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_ACTION_TUNNEL_ID);
    auto *type = findAttr(attr_count, attr_list, SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_TYPE);
    auto *ttype = findAttr(attr_count, attr_list, SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_TUNNEL_TYPE);
    auto *dst = findAttr(attr_count, attr_list, SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_DST_IP);
    auto *src = findAttr(attr_count, attr_list, SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_SRC_IP);

    if (ttype != nullptr && ttype->value.s32 != SAI_TUNNEL_TYPE_VXLAN)
    {
        SWSS_LOG_WARN("term entry %s: tunnel type %d NOT SUPPORTED, software only",
                sai_serialize_object_id(term_id).c_str(), ttype->value.s32);

        return SAI_STATUS_SUCCESS;
    }

    sai_object_id_t tunnel = (action != nullptr) ? action->value.oid : SAI_NULL_OBJECT_ID;

    m_termEntryTunnel[term_id] = tunnel;

    // On TD2 the terminator (local DIP) was created together with the P2P
    // tunnel initiator; this entry only confirms it. Check consistency.
    if (type != nullptr && type->value.s32 == SAI_TUNNEL_TERM_TABLE_ENTRY_TYPE_P2P && src != nullptr)
    {
        std::string key = sai_serialize_ip_address(src->value.ipaddr);

        auto rt = m_tunnelRemoteIp.find(tunnel);

        if (rt == m_tunnelRemoteIp.end())
        {
            SWSS_LOG_WARN("term entry %s: action tunnel %s is not a known P2P VXLAN tunnel (remote %s)",
                    sai_serialize_object_id(term_id).c_str(), sai_serialize_object_id(tunnel).c_str(), key.c_str());
        }
        else if (rt->second != key)
        {
            SWSS_LOG_WARN("term entry %s: SRC_IP %s differs from tunnel remote %s",
                    sai_serialize_object_id(term_id).c_str(), key.c_str(), rt->second.c_str());
        }
    }

    if (dst != nullptr)
    {
        uint8_t d4[4] = {0, 0, 0, 0};

        if (vxlanIp4Of(dst->value.ipaddr, d4) && m_vtepSrcValid && memcmp(d4, m_vtepSrcIp4, 4) != 0)
        {
            SWSS_LOG_WARN("term entry %s: DST_IP %s differs from local VTEP %u.%u.%u.%u",
                    sai_serialize_object_id(term_id).c_str(), sai_serialize_ip_address(dst->value.ipaddr).c_str(),
                    m_vtepSrcIp4[0], m_vtepSrcIp4[1], m_vtepSrcIp4[2], m_vtepSrcIp4[3]);
        }
    }

    SWSS_LOG_INFO("term entry %s recorded for tunnel %s",
            sai_serialize_object_id(term_id).c_str(), sai_serialize_object_id(tunnel).c_str());

    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchBCM56860HW::hwRemoveTunnelTermEntry(
        _In_ sai_object_id_t term_id)
{
    SWSS_LOG_ENTER();

    m_termEntryTunnel.erase(term_id);

    return SAI_STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// SAI_OBJECT_TYPE_BRIDGE (1D) / BRIDGE_PORT (tunnel)
// ---------------------------------------------------------------------------

sai_status_t SwitchBCM56860HW::hwCreateBridge(
        _In_ sai_object_id_t bridge_id,
        _In_ uint32_t attr_count,
        _In_ const sai_attribute_t *attr_list)
{
    SWSS_LOG_ENTER();

    auto *type = findAttr(attr_count, attr_list, SAI_BRIDGE_ATTR_TYPE);

    if (type != nullptr && type->value.s32 == SAI_BRIDGE_TYPE_1D)
    {
        SWSS_LOG_WARN("1D bridge %s: recorded only, NOT SUPPORTED in hardware (SONiC EVPN uses VLAN based VNIs)",
                sai_serialize_object_id(bridge_id).c_str());

        m_bridgeToVpn[bridge_id] = -1;
    }

    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchBCM56860HW::hwRemoveBridge(
        _In_ sai_object_id_t bridge_id)
{
    SWSS_LOG_ENTER();

    m_bridgeToVpn.erase(bridge_id);

    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchBCM56860HW::hwCreateBridgePort(
        _In_ sai_object_id_t bp_id,
        _In_ uint32_t attr_count,
        _In_ const sai_attribute_t *attr_list)
{
    SWSS_LOG_ENTER();

    auto *type = findAttr(attr_count, attr_list, SAI_BRIDGE_PORT_ATTR_TYPE);

    if (type == nullptr || type->value.s32 != SAI_BRIDGE_PORT_TYPE_TUNNEL)
    {
        // PORT / SUB_PORT / 1Q_ROUTER ... are handled by the core (A2).
        return SAI_STATUS_SUCCESS;
    }

    auto *tun = findAttr(attr_count, attr_list, SAI_BRIDGE_PORT_ATTR_TUNNEL_ID);

    if (tun == nullptr)
    {
        SWSS_LOG_ERROR("tunnel bridge port %s without SAI_BRIDGE_PORT_ATTR_TUNNEL_ID", sai_serialize_object_id(bp_id).c_str());

        return SAI_STATUS_INVALID_PARAMETER;
    }

    if (!m_hwReady)
    {
        return SAI_STATUS_SUCCESS;
    }

    auto rt = m_tunnelRemoteIp.find(tun->value.oid);

    if (rt == m_tunnelRemoteIp.end())
    {
        SWSS_LOG_WARN("tunnel bridge port %s: tunnel %s is not a P2P VXLAN tunnel in hardware, software only",
                sai_serialize_object_id(bp_id).c_str(), sai_serialize_object_id(tun->value.oid).c_str());

        return SAI_STATUS_SUCCESS;
    }

    const std::string key = rt->second;

    VxlanRemoteVtep &vtep = m_vtepByIp[key];

    vtep.bridgePorts.insert(bp_id);
    m_bridgePortRemoteIp[bp_id] = key;

    if (vtep.vpReady)
    {
        m_bridgePortToVp[bp_id] = vtep.vp;
    }
    else if (!vxlanTryBringUpVtep(vtep))
    {
        SWSS_LOG_NOTICE("tunnel bridge port %s for %s: network VP pending (underlay next hop or VNI missing)",
                sai_serialize_object_id(bp_id).c_str(), key.c_str());
    }

    SWSS_LOG_NOTICE("tunnel bridge port %s -> remote VTEP %s (vp 0x%x)",
            sai_serialize_object_id(bp_id).c_str(), key.c_str(), vtep.vp);

    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchBCM56860HW::hwRemoveBridgePort(
        _In_ sai_object_id_t bp_id)
{
    SWSS_LOG_ENTER();

    auto it = m_bridgePortRemoteIp.find(bp_id);

    if (it == m_bridgePortRemoteIp.end())
    {
        // not a tunnel bridge port: nothing for A4
        return SAI_STATUS_SUCCESS;
    }

    const std::string key = it->second;

    m_bridgePortRemoteIp.erase(it);
    m_bridgePortToVp.erase(bp_id);

    auto vt = m_vtepByIp.find(key);

    if (vt == m_vtepByIp.end())
    {
        return SAI_STATUS_SUCCESS;
    }

    VxlanRemoteVtep &vtep = vt->second;

    vtep.bridgePorts.erase(bp_id);

    if (vtep.bridgePorts.empty())
    {
        vxlanTearDownVtepVp(vtep);
        vtep.macs.clear();

        if (vtep.tunnels.empty())
        {
            m_vtepByIp.erase(vt);
        }
    }

    SWSS_LOG_NOTICE("tunnel bridge port %s removed (remote VTEP %s)", sai_serialize_object_id(bp_id).c_str(), key.c_str());

    return SAI_STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// remote MACs (FDB on a tunnel bridge port)
// ---------------------------------------------------------------------------

sai_status_t SwitchBCM56860HW::hwFdbOnTunnel(
        _In_ const sai_fdb_entry_t &fdb,
        _In_ sai_object_id_t bridge_port,
        _In_ bool add)
{
    SWSS_LOG_ENTER();

    auto it = m_bridgePortRemoteIp.find(bridge_port);

    if (it == m_bridgePortRemoteIp.end())
    {
        SWSS_LOG_WARN("fdb %s: bridge port %s is not a tunnel bridge port",
                sai_serialize_fdb_entry(fdb).c_str(), sai_serialize_object_id(bridge_port).c_str());

        return SAI_STATUS_ITEM_NOT_FOUND;
    }

    auto vt = m_vtepByIp.find(it->second);

    if (vt == m_vtepByIp.end())
    {
        return SAI_STATUS_ITEM_NOT_FOUND;
    }

    VxlanRemoteVtep &vtep = vt->second;

    int vid = vidOf(fdb.bv_id);

    if (vid < 0)
    {
        SWSS_LOG_WARN("fdb %s: bv_id is not a VLAN (1D bridge NOT SUPPORTED), software only",
                sai_serialize_fdb_entry(fdb).c_str());

        return SAI_STATUS_SUCCESS;
    }

    const std::string key = sai_serialize_fdb_entry(fdb);

    int vpn = vxlanVpnOfVid(vid);

    if (add)
    {
        VxlanMac m;

        memcpy(m.mac, fdb.mac_address, sizeof(sai_mac_t));
        m.vid = vid;

        vtep.macs[key] = m;

        if (!vtep.vpReady || vpn < 0)
        {
            SWSS_LOG_NOTICE("fdb %s on %s deferred (vp ready %d, vpn %d)", key.c_str(), vtep.ip.c_str(), vtep.vpReady ? 1 : 0, vpn);

            return SAI_STATUS_SUCCESS;
        }

        int rc = hwbcm_vxlan_l2_addr_add(fdb.mac_address, vpn, vtep.vp, 1);

        if (rc < 0)
        {
            SWSS_LOG_ERROR("fdb %s on %s: l2 add failed: %d", key.c_str(), vtep.ip.c_str(), rc);

            return SAI_STATUS_FAILURE;
        }

        SWSS_LOG_INFO("fdb %s -> remote VTEP %s (vpn 0x%x, vp 0x%x)", key.c_str(), vtep.ip.c_str(), vpn, vtep.vp);

        return SAI_STATUS_SUCCESS;
    }

    vtep.macs.erase(key);

    if (vpn >= 0 && vtep.vpReady)
    {
        int rc = hwbcm_vxlan_l2_addr_delete(fdb.mac_address, vpn);

        if (rc < 0)
        {
            SWSS_LOG_WARN("fdb %s on %s: l2 delete: %d", key.c_str(), vtep.ip.c_str(), rc);
        }
    }

    return SAI_STATUS_SUCCESS;
}
