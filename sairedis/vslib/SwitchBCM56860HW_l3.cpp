/*
 * SwitchBCM56860HW_l3.cpp — L3 handlers of the Trident2+ hardware switch:
 * VIRTUAL_ROUTER, ROUTER_INTERFACE, NEIGHBOR_ENTRY, NEXT_HOP,
 * NEXT_HOP_GROUP (+MEMBER), ROUTE_ENTRY.
 *
 * Called by the dispatcher in SwitchBCM56860HW.cpp after the base class has
 * stored the object (create) or before it removes it (remove), so stored
 * attributes can be read with getStoredAttr().
 *
 * Egress object ownership
 * -----------------------
 * A NEXT_HOP (rif, ip) and a NEIGHBOR_ENTRY (rif, ip) share one bcm egress
 * object. Whoever comes first creates it: a neighbor creates it with the real
 * mac/port, a next hop without a neighbor creates a CPU (L2TOCPU) egress so
 * traffic is punted until the neighbor resolves. When the neighbor shows up it
 * rewrites the shared egress in place (BCM_L3_REPLACE), so ECMP groups and
 * routes referencing it stay valid; when it goes away the egress is rewritten
 * back to CPU. The egress is destroyed when neither side references it.
 */
#include "SwitchBCM56860HW.h"
#include "hwbcm/hwbcm.h"

#include "swss/logger.h"
#include "meta/sai_serialize.h"

#include <cstring>
#include <cinttypes>
#include <set>
#include <algorithm>
#include <arpa/inet.h>

using namespace saivs;

namespace
{
    /* Dedicated VLANs for port RIFs are taken from this pool. */
    const int RIF_VID_MIN = 3000;
    const int RIF_VID_MAX = 3999;

    /* One switch instance per process: shared trap/drop egress objects.
     * (L2TOCPU egress ignores intf, so one CPU egress serves every RIF.) */
    int g_cpuEgr = -1;
    int g_dropEgr = -1;

    int sharedCpuEgress()
    {
        SWSS_LOG_ENTER();

        if (g_cpuEgr < 0)
        {
            int rv = hwbcm_l3_egress_cpu_create(-1, &g_cpuEgr);

            if (rv < 0)
            {
                SWSS_LOG_ERROR("hwbcm_l3_egress_cpu_create failed: %d", rv);
                g_cpuEgr = -1;
            }
        }

        return g_cpuEgr;
    }

    int sharedDropEgress()
    {
        SWSS_LOG_ENTER();

        if (g_dropEgr < 0)
        {
            int rv = hwbcm_l3_egress_drop_create(&g_dropEgr);

            if (rv < 0)
            {
                SWSS_LOG_ERROR("hwbcm_l3_egress_drop_create failed: %d", rv);
                g_dropEgr = -1;
            }
        }

        return g_dropEgr;
    }

    /* sai_ip_address_t -> (af, 4/16 network-order bytes) */
    int ipBytes(
            _In_ const sai_ip_address_t &ip,
            _Out_ uint8_t buf[16])
    {
        SWSS_LOG_ENTER();

        if (ip.addr_family == SAI_IP_ADDR_FAMILY_IPV6)
        {
            memcpy(buf, ip.addr.ip6, 16);
            return 6;
        }

        memcpy(buf, &ip.addr.ip4, 4);
        return 4;
    }

    /* sai_ip_prefix_t -> (af, prefix bytes, prefix length) */
    int prefixBytes(
            _In_ const sai_ip_prefix_t &pfx,
            _Out_ uint8_t buf[16],
            _Out_ int &len)
    {
        SWSS_LOG_ENTER();

        uint8_t mask[16];
        int af;
        size_t n;

        if (pfx.addr_family == SAI_IP_ADDR_FAMILY_IPV6)
        {
            memcpy(buf, pfx.addr.ip6, 16);
            memcpy(mask, pfx.mask.ip6, 16);
            af = 6;
            n = 16;
        }
        else
        {
            memcpy(buf, &pfx.addr.ip4, 4);
            memcpy(mask, &pfx.mask.ip4, 4);
            af = 4;
            n = 4;
        }

        len = 0;

        for (size_t i = 0; i < n; i++)
        {
            uint8_t b = mask[i];

            while (b)
            {
                len += (b & 1);
                b = (uint8_t)(b >> 1);
            }
        }

        return af;
    }

    uint64_t vrfIp4Key(
            _In_ int vrf,
            _In_ uint32_t ip4)
    {
        return (static_cast<uint64_t>(static_cast<uint32_t>(vrf)) << 32) | static_cast<uint64_t>(ip4);
    }

    uint32_t ipv4PrefixMaskNet(
            _In_ int len)
    {
        if (len <= 0)
        {
            return 0u;
        }
        if (len >= 32)
        {
            return 0xFFFFFFFFu;
        }
        return htonl(0xFFFFFFFFu << (32 - len));
    }

    /* Resolve a virtual router oid to a bcm vrf. The default VR is created by
     * the base class in initialize_default_objects before any hw dispatch, so
     * it is mapped here on first use (-> vrf 0). Any other VR seen for the
     * first time gets a fresh vrf id (allocate), or fails if !allocate or
     * VRF creation fails (never silently falling back to VRF 0). */
    sai_status_t resolveVrf(
            _In_ std::map<sai_object_id_t, int> &vrToVrf,
            _In_ sai_object_id_t defaultVr,
            _In_ sai_object_id_t vr,
            _In_ bool allocate,
            _In_ const char *ctx,
            _Out_ int &vrfOut)
    {
        SWSS_LOG_ENTER();

        auto it = vrToVrf.find(vr);

        if (it != vrToVrf.end())
        {
            vrfOut = it->second;
            return SAI_STATUS_SUCCESS;
        }

        if (vr == SAI_NULL_OBJECT_ID)
        {
            SWSS_LOG_ERROR("%s: null virtual router", ctx);
            return SAI_STATUS_INVALID_PARAMETER;
        }

        if (vr == defaultVr || defaultVr == SAI_NULL_OBJECT_ID)
        {
            // default VR (or default not yet published by the base: treat the
            // first VR we meet as the default one)
            vrToVrf[vr] = 0;
            vrfOut = 0;
            SWSS_LOG_NOTICE("%s: virtual router %s -> vrf 0 (default)", ctx, sai_serialize_object_id(vr).c_str());
            return SAI_STATUS_SUCCESS;
        }

        if (!allocate)
        {
            SWSS_LOG_ERROR("%s: virtual router %s unknown", ctx, sai_serialize_object_id(vr).c_str());
            return SAI_STATUS_ITEM_NOT_FOUND;
        }

        int vrf = 0;
        int rv = hwbcm_l3_vrf_create(&vrf);

        if (rv < 0)
        {
            SWSS_LOG_ERROR("%s: hwbcm_l3_vrf_create failed: %d", ctx, rv);
            return SwitchBCM56860HW::bcmRvToSai(rv);
        }

        vrToVrf[vr] = vrf;
        vrfOut = vrf;
        SWSS_LOG_NOTICE("%s: virtual router %s -> vrf %d (late)", ctx, sai_serialize_object_id(vr).c_str(), vrf);
        return SAI_STATUS_SUCCESS;
    }

    /* Egress ids of all members of a next hop group, duplicates kept out.
     * Iterates only this group's members (O(k_group) via m_nhgToMembers). */
    std::vector<int> groupEgressList(
            _In_ sai_object_id_t nhg,
            _In_ const std::unordered_map<sai_object_id_t, std::set<sai_object_id_t>> &nhgToMembers,
            _In_ const std::map<sai_object_id_t, sai_object_id_t> &memberToNh,
            _In_ const std::unordered_map<sai_object_id_t, int> &nhToEgr)
    {
        SWSS_LOG_ENTER();

        std::vector<int> egrs;

        auto git = nhgToMembers.find(nhg);
        if (git == nhgToMembers.end())
        {
            return egrs;
        }

        for (auto memberOid: git->second)
        {
            auto nhIt = memberToNh.find(memberOid);

            if (nhIt == memberToNh.end())
            {
                continue;
            }

            auto egrIt = nhToEgr.find(nhIt->second);

            if (egrIt == nhToEgr.end())
            {
                SWSS_LOG_WARN("nhg member %s: next hop %s has no egress, skipped",
                        sai_serialize_object_id(memberOid).c_str(),
                        sai_serialize_object_id(nhIt->second).c_str());
                continue;
            }

            if (std::find(egrs.begin(), egrs.end(), egrIt->second) == egrs.end())
            {
                egrs.push_back(egrIt->second);
            }
        }

        return egrs;
    }
}

// --- Derived (rif, ip) -> NEXT_HOP index ---------------------------------

SwitchBCM56860HW::RifIpKey SwitchBCM56860HW::makeRifIpKey(
        _In_ sai_object_id_t rif,
        _In_ const sai_ip_address_t &ip)
{
    RifIpKey k;
    k.rif = rif;
    if (ip.addr_family == SAI_IP_ADDR_FAMILY_IPV6)
    {
        k.af = 6;
        memcpy(k.addr, ip.addr.ip6, 16);
    }
    else
    {
        k.af = 4;
        memcpy(k.addr, &ip.addr.ip4, 4);
    }
    return k;
}

void SwitchBCM56860HW::nhIndexAdd(
        _In_ sai_object_id_t nh_id,
        _In_ sai_object_id_t rif,
        _In_ const sai_ip_address_t &ip)
{
    RifIpKey k = makeRifIpKey(rif, ip);
    auto &vec = m_nhByRifIp[k];
    if (std::find(vec.begin(), vec.end(), nh_id) == vec.end())
    {
        vec.push_back(nh_id);
    }
    m_nhKey[nh_id] = k;
}

void SwitchBCM56860HW::nhIndexRemove(
        _In_ sai_object_id_t nh_id)
{
    auto kit = m_nhKey.find(nh_id);
    if (kit == m_nhKey.end())
    {
        return;
    }
    auto bit = m_nhByRifIp.find(kit->second);
    if (bit != m_nhByRifIp.end())
    {
        auto &vec = bit->second;
        vec.erase(std::remove(vec.begin(), vec.end(), nh_id), vec.end());
        if (vec.empty())
        {
            m_nhByRifIp.erase(bit);
        }
    }
    m_nhKey.erase(kit);
}

SwitchBCM56860HW::VlanMacKey SwitchBCM56860HW::makeVlanMacKey(
        _In_ sai_object_id_t vlanOid,
        _In_ const uint8_t mac[6])
{
    VlanMacKey k;
    k.vlanOid = vlanOid;
    memcpy(k.mac, mac, 6);
    return k;
}

void SwitchBCM56860HW::egrNhRefInc(_In_ int egr)
{
    m_egrNhRefCount[egr]++;
}

void SwitchBCM56860HW::egrNhRefDec(_In_ int egr)
{
    auto it = m_egrNhRefCount.find(egr);
    if (it != m_egrNhRefCount.end())
    {
        if (it->second <= 1)
        {
            m_egrNhRefCount.erase(it);
        }
        else
        {
            it->second--;
        }
    }
}

void SwitchBCM56860HW::egrNeighborRefInc(_In_ int egr)
{
    m_egrNeighborRefCount[egr]++;
}

void SwitchBCM56860HW::egrNeighborRefDec(_In_ int egr)
{
    auto it = m_egrNeighborRefCount.find(egr);
    if (it != m_egrNeighborRefCount.end())
    {
        if (it->second <= 1)
        {
            m_egrNeighborRefCount.erase(it);
        }
        else
        {
            it->second--;
        }
    }
}

void SwitchBCM56860HW::neighborVlanIndexAdd(
        _In_ const std::string &serialized,
        _In_ sai_object_id_t rif_id,
        _In_ const uint8_t mac[6])
{
    sai_attribute_t rifType, vlanAttr;
    if (!getStoredAttr(SAI_OBJECT_TYPE_ROUTER_INTERFACE, rif_id, SAI_ROUTER_INTERFACE_ATTR_TYPE, rifType) ||
        rifType.value.s32 != SAI_ROUTER_INTERFACE_TYPE_VLAN ||
        !getStoredAttr(SAI_OBJECT_TYPE_ROUTER_INTERFACE, rif_id, SAI_ROUTER_INTERFACE_ATTR_VLAN_ID, vlanAttr))
    {
        return;
    }

    auto vit = m_rifToVid.find(rif_id);
    int vid = (vit != m_rifToVid.end()) ? vit->second : vidOf(vlanAttr.value.oid);
    if (vid < 0)
    {
        return;
    }

    VlanMacKey mk = makeVlanMacKey(vlanAttr.value.oid, mac);

    auto &mvec = m_vlanNeighborsByMac[mk];
    if (std::find(mvec.begin(), mvec.end(), serialized) == mvec.end())
    {
        mvec.push_back(serialized);
    }

    auto &vvec = m_vlanNeighborsByVid[vid];
    if (std::find(vvec.begin(), vvec.end(), serialized) == vvec.end())
    {
        vvec.push_back(serialized);
    }

    m_neighborVlanMac[serialized] = std::make_pair(vid, mk);
}

void SwitchBCM56860HW::neighborVlanIndexUpdateMac(
        _In_ const std::string &serialized,
        _In_ const uint8_t newMac[6])
{
    auto it = m_neighborVlanMac.find(serialized);
    if (it == m_neighborVlanMac.end())
    {
        return;
    }

    VlanMacKey oldKey = it->second.second;
    VlanMacKey newKey = makeVlanMacKey(oldKey.vlanOid, newMac);
    if (oldKey == newKey)
    {
        return;
    }

    auto omit = m_vlanNeighborsByMac.find(oldKey);
    if (omit != m_vlanNeighborsByMac.end())
    {
        auto &vec = omit->second;
        vec.erase(std::remove(vec.begin(), vec.end(), serialized), vec.end());
        if (vec.empty())
        {
            m_vlanNeighborsByMac.erase(omit);
        }
    }

    auto &nvec = m_vlanNeighborsByMac[newKey];
    if (std::find(nvec.begin(), nvec.end(), serialized) == nvec.end())
    {
        nvec.push_back(serialized);
    }

    it->second.second = newKey;
}

void SwitchBCM56860HW::neighborVlanIndexRemove(
        _In_ const std::string &serialized)
{
    auto it = m_neighborVlanMac.find(serialized);
    if (it == m_neighborVlanMac.end())
    {
        return;
    }

    int vid = it->second.first;
    VlanMacKey mk = it->second.second;

    auto mit = m_vlanNeighborsByMac.find(mk);
    if (mit != m_vlanNeighborsByMac.end())
    {
        auto &vec = mit->second;
        vec.erase(std::remove(vec.begin(), vec.end(), serialized), vec.end());
        if (vec.empty())
        {
            m_vlanNeighborsByMac.erase(mit);
        }
    }

    auto vit = m_vlanNeighborsByVid.find(vid);
    if (vit != m_vlanNeighborsByVid.end())
    {
        auto &vec = vit->second;
        vec.erase(std::remove(vec.begin(), vec.end(), serialized), vec.end());
        if (vec.empty())
        {
            m_vlanNeighborsByVid.erase(vit);
        }
    }

    m_neighborVlanMac.erase(it);
}

void SwitchBCM56860HW::v4NeighborIndexAdd(
        _In_ int vrf,
        _In_ uint32_t ip4,
        _In_ const std::string &serialized,
        _In_ int egr)
{
    auto &vec = m_v4NeighborsByVrfIp[vrfIp4Key(vrf, ip4)];
    for (auto &p: vec)
    {
        if (p.first == serialized)
        {
            p.second = egr;
            return;
        }
    }
    vec.emplace_back(serialized, egr);
}

void SwitchBCM56860HW::v4NeighborIndexRemove(
        _In_ int vrf,
        _In_ uint32_t ip4,
        _In_ const std::string &serialized)
{
    auto it = m_v4NeighborsByVrfIp.find(vrfIp4Key(vrf, ip4));
    if (it == m_v4NeighborsByVrfIp.end())
    {
        return;
    }
    auto &vec = it->second;
    vec.erase(std::remove_if(vec.begin(), vec.end(),
            [&](const std::pair<std::string, int> &p) { return p.first == serialized; }),
            vec.end());
    if (vec.empty())
    {
        m_v4NeighborsByVrfIp.erase(it);
    }
}

void SwitchBCM56860HW::v4RouteIndexAdd(
        _In_ int vrf,
        _In_ uint32_t ip4,
        _In_ int len,
        _In_ int egr)
{
    if (len < 0 || len > 32)
    {
        return;
    }
    uint32_t masked = ip4 & ipv4PrefixMaskNet(len);
    m_v4RoutesByVrfLen[len][vrfIp4Key(vrf, masked)] = egr;
    m_v4RouteLenMask |= (1ULL << len);
}

void SwitchBCM56860HW::v4RouteIndexRemove(
        _In_ int vrf,
        _In_ uint32_t ip4,
        _In_ int len)
{
    if (len < 0 || len > 32)
    {
        return;
    }
    uint32_t masked = ip4 & ipv4PrefixMaskNet(len);
    auto &tbl = m_v4RoutesByVrfLen[len];
    tbl.erase(vrfIp4Key(vrf, masked));
    if (tbl.empty())
    {
        m_v4RouteLenMask &= ~(1ULL << len);
    }
}

sai_object_id_t SwitchBCM56860HW::defaultVirtualRouterId()
{
    if (m_defaultVrId == SAI_NULL_OBJECT_ID)
    {
        sai_attribute_t sw;
        if (getStoredAttr(SAI_OBJECT_TYPE_SWITCH, m_switch_id, SAI_SWITCH_ATTR_DEFAULT_VIRTUAL_ROUTER_ID, sw))
        {
            m_defaultVrId = sw.value.oid;
        }
    }
    return m_defaultVrId;
}

// --- VIRTUAL_ROUTER -------------------------------------------------------

sai_status_t SwitchBCM56860HW::hwCreateVirtualRouter(
        _In_ sai_object_id_t vr_id,
        _In_ uint32_t attr_count,
        _In_ const sai_attribute_t *attr_list)
{
    SWSS_LOG_ENTER();

    if (m_vrToVrf.find(vr_id) != m_vrToVrf.end())
    {
        return SAI_STATUS_SUCCESS;
    }

    // The default VR is created by the base before hw dispatch exists, so a
    // VR reaching this handler is normally a user VRF. Still honour the
    // stored default oid if it matches, and treat the very first VR as the
    // default when the base has not published it yet.

    sai_object_id_t defaultVr = SAI_NULL_OBJECT_ID;

    sai_attribute_t sw;

    if (getStoredAttr(SAI_OBJECT_TYPE_SWITCH, m_switch_id, SAI_SWITCH_ATTR_DEFAULT_VIRTUAL_ROUTER_ID, sw))
    {
        defaultVr = sw.value.oid;
    }

    bool isDefault = (vr_id == defaultVr) || (defaultVr == SAI_NULL_OBJECT_ID && m_vrToVrf.empty());

    int vrf = 0;

    if (!isDefault)
    {
        int rv = hwbcm_l3_vrf_create(&vrf);

        if (rv < 0)
        {
            SWSS_LOG_ERROR("hwbcm_l3_vrf_create failed: %d", rv);
            return bcmRvToSai(rv);
        }
    }

    m_vrToVrf[vr_id] = vrf;

    SWSS_LOG_NOTICE("virtual router %s -> vrf %d%s", sai_serialize_object_id(vr_id).c_str(), vrf, isDefault ? " (default)" : "");

    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchBCM56860HW::hwRemoveVirtualRouter(
        _In_ sai_object_id_t vr_id)
{
    SWSS_LOG_ENTER();

    auto it = m_vrToVrf.find(vr_id);

    if (it == m_vrToVrf.end())
    {
        SWSS_LOG_WARN("virtual router %s unknown to hw", sai_serialize_object_id(vr_id).c_str());
        return SAI_STATUS_SUCCESS;
    }

    if (it->second != 0)
    {
        int rv = hwbcm_l3_vrf_destroy(it->second);

        if (rv < 0)
        {
            SWSS_LOG_ERROR("hwbcm_l3_vrf_destroy(%d) failed: %d", it->second, rv);
            return bcmRvToSai(rv);
        }
    }

    m_vrToVrf.erase(it);

    return SAI_STATUS_SUCCESS;
}

// --- ROUTER_INTERFACE -----------------------------------------------------

sai_status_t SwitchBCM56860HW::hwCreateRouterInterface(
        _In_ sai_object_id_t rif_id,
        _In_ uint32_t attr_count,
        _In_ const sai_attribute_t *attr_list)
{
    SWSS_LOG_ENTER();

    std::string srif = sai_serialize_object_id(rif_id);

    auto *typeAttr = findAttr(attr_count, attr_list, SAI_ROUTER_INTERFACE_ATTR_TYPE);

    if (typeAttr == nullptr)
    {
        SWSS_LOG_ERROR("rif %s: missing SAI_ROUTER_INTERFACE_ATTR_TYPE", srif.c_str());
        return SAI_STATUS_FAILURE;
    }

    sai_router_interface_type_t type = (sai_router_interface_type_t)typeAttr->value.s32;

    switch (type)
    {
        case SAI_ROUTER_INTERFACE_TYPE_PORT:
        case SAI_ROUTER_INTERFACE_TYPE_VLAN:
            break;

        case SAI_ROUTER_INTERFACE_TYPE_LOOPBACK:
            SWSS_LOG_NOTICE("rif %s: loopback, no hw object", srif.c_str());
            return SAI_STATUS_SUCCESS;

        case SAI_ROUTER_INTERFACE_TYPE_SUB_PORT:
        default:
            SWSS_LOG_WARN("rif %s: type %d not supported in hw, ignored", srif.c_str(), (int)type);
            return SAI_STATUS_SUCCESS;
    }

    // vrf

    int vrf = 0;

    auto *vrAttr = findAttr(attr_count, attr_list, SAI_ROUTER_INTERFACE_ATTR_VIRTUAL_ROUTER_ID);

    if (vrAttr != nullptr)
    {
        sai_object_id_t defaultVr = SAI_NULL_OBJECT_ID;

        {
            sai_attribute_t sw;

            if (getStoredAttr(SAI_OBJECT_TYPE_SWITCH, m_switch_id, SAI_SWITCH_ATTR_DEFAULT_VIRTUAL_ROUTER_ID, sw))
            {
                defaultVr = sw.value.oid;
            }
        }

        sai_status_t st = resolveVrf(m_vrToVrf, defaultVr, vrAttr->value.oid, true, srif.c_str(), vrf);

        if (st != SAI_STATUS_SUCCESS)
        {
            return st;
        }
    }

    // mac: RIF attribute, else switch mac

    sai_mac_t mac;
    memset(mac, 0, sizeof(mac));

    auto *macAttr = findAttr(attr_count, attr_list, SAI_ROUTER_INTERFACE_ATTR_SRC_MAC_ADDRESS);

    if (macAttr != nullptr)
    {
        memcpy(mac, macAttr->value.mac, sizeof(mac));
    }
    else
    {
        sai_attribute_t sw;

        if (getStoredAttr(SAI_OBJECT_TYPE_SWITCH, m_switch_id, SAI_SWITCH_ATTR_SRC_MAC_ADDRESS, sw))
        {
            memcpy(mac, sw.value.mac, sizeof(mac));
        }
        else
        {
            SWSS_LOG_ERROR("rif %s: no SRC_MAC on rif and no switch SRC_MAC_ADDRESS", srif.c_str());
            return SAI_STATUS_FAILURE;
        }
    }

    // mtu

    int mtu = 1514;

    auto *mtuAttr = findAttr(attr_count, attr_list, SAI_ROUTER_INTERFACE_ATTR_MTU);

    if (mtuAttr != nullptr)
    {
        mtu = (int)mtuAttr->value.u32;
    }

    // vlan

    int vid = -1;
    int port = -1;
    int trunk = -1;
    std::vector<int> memberPorts;   // ports added to the dedicated vlan (port RIF)
    bool dedicatedVlan = false;

    if (type == SAI_ROUTER_INTERFACE_TYPE_VLAN)
    {
        auto *vlanAttr = findAttr(attr_count, attr_list, SAI_ROUTER_INTERFACE_ATTR_VLAN_ID);

        if (vlanAttr == nullptr)
        {
            SWSS_LOG_ERROR("rif %s: missing SAI_ROUTER_INTERFACE_ATTR_VLAN_ID", srif.c_str());
            return SAI_STATUS_FAILURE;
        }

        vid = vidOf(vlanAttr->value.oid);

        if (vid < 0)
        {
            SWSS_LOG_ERROR("rif %s: vlan %s unknown", srif.c_str(), sai_serialize_object_id(vlanAttr->value.oid).c_str());
            return SAI_STATUS_FAILURE;
        }
    }
    else // PORT
    {
        auto *portAttr = findAttr(attr_count, attr_list, SAI_ROUTER_INTERFACE_ATTR_PORT_ID);

        if (portAttr == nullptr)
        {
            SWSS_LOG_ERROR("rif %s: missing SAI_ROUTER_INTERFACE_ATTR_PORT_ID", srif.c_str());
            return SAI_STATUS_FAILURE;
        }

        sai_object_id_t target = portAttr->value.oid;
        sai_object_type_t tot = objectTypeQuery(target);

        if (tot == SAI_OBJECT_TYPE_PORT)
        {
            port = bcmPortOf(target);

            if (port < 0)
            {
                SWSS_LOG_ERROR("rif %s: port %s has no bcm port", srif.c_str(), sai_serialize_object_id(target).c_str());
                return SAI_STATUS_FAILURE;
            }

            memberPorts.push_back(port);
        }
        else if (tot == SAI_OBJECT_TYPE_LAG)
        {
            trunk = trunkOf(target);

            if (trunk < 0)
            {
                SWSS_LOG_ERROR("rif %s: lag %s has no bcm trunk", srif.c_str(), sai_serialize_object_id(target).c_str());
                return SAI_STATUS_FAILURE;
            }

            auto lm = m_lagMembers.find(target);

            if (lm != m_lagMembers.end())
            {
                std::lock_guard<std::mutex> lock(m_mapMutex);

                for (auto poid: lm->second)
                {
                    auto pit = m_portToBcm.find(poid);

                    if (pit != m_portToBcm.end())
                    {
                        memberPorts.push_back(pit->second);
                    }
                }
            }

            if (memberPorts.empty())
            {
                SWSS_LOG_WARN("rif %s: lag %s has no members yet, vlan membership deferred to lag member add",
                        srif.c_str(), sai_serialize_object_id(target).c_str());
            }
        }
        else
        {
            SWSS_LOG_ERROR("rif %s: PORT_ID %s is neither port nor lag", srif.c_str(), sai_serialize_object_id(target).c_str());
            return SAI_STATUS_FAILURE;
        }

        // pick a free vid from the pool, excluding both existing RIF VLANs
        // and any user-created L2 VLANs in m_objectHash[SAI_OBJECT_TYPE_VLAN]

        std::set<int> used;

        for (auto &kv: m_rifToVid)
        {
            used.insert(kv.second);
        }

        auto vhash = m_objectHash.find(SAI_OBJECT_TYPE_VLAN);

        if (vhash != m_objectHash.end())
        {
            auto *vmeta = sai_metadata_get_attr_metadata(SAI_OBJECT_TYPE_VLAN, SAI_VLAN_ATTR_VLAN_ID);

            if (vmeta != nullptr)
            {
                for (const auto &vkv: vhash->second)
                {
                    auto ait = vkv.second.find(vmeta->attridname);

                    if (ait != vkv.second.end() && ait->second != nullptr && ait->second->getAttr() != nullptr)
                    {
                        used.insert((int)ait->second->getAttr()->value.u16);
                    }
                }
            }
        }

        for (int v = RIF_VID_MIN; v <= RIF_VID_MAX; v++)
        {
            if (used.find(v) == used.end())
            {
                vid = v;
                break;
            }
        }

        if (vid < 0)
        {
            SWSS_LOG_ERROR("rif %s: no free vlan in pool %d-%d", srif.c_str(), RIF_VID_MIN, RIF_VID_MAX);
            return SAI_STATUS_TABLE_FULL;
        }

        int rv = hwbcm_vlan_create(vid);

        if (rv < 0)
        {
            SWSS_LOG_ERROR("rif %s: hwbcm_vlan_create(%d) failed: %d", srif.c_str(), vid, rv);
            return bcmRvToSai(rv);
        }

        dedicatedVlan = true;
        int defVid = vidOf(m_default_vlan_id);
        if (defVid < 0)
        {
            defVid = 1;
        }
        std::vector<int> addedPorts;
        std::vector<int> pvidPorts;

        auto rollbackVlan = [&]()
        {
            for (int rp: pvidPorts)
            {
                int rrv = hwbcm_port_pvid_set(rp, defVid);
                if (rrv < 0)
                {
                    SWSS_LOG_ERROR("rif %s: rollback hwbcm_port_pvid_set(%d, %d) failed: %d", srif.c_str(), rp, defVid, rrv);
                }
            }
            for (int rp: addedPorts)
            {
                int rrv = hwbcm_vlan_port_remove(vid, rp);
                if (rrv < 0)
                {
                    SWSS_LOG_ERROR("rif %s: rollback hwbcm_vlan_port_remove(%d, %d) failed: %d", srif.c_str(), vid, rp, rrv);
                }
            }
            int drv = hwbcm_vlan_destroy(vid);
            if (drv < 0)
            {
                SWSS_LOG_ERROR("rif %s: rollback hwbcm_vlan_destroy(%d) failed: %d", srif.c_str(), vid, drv);
            }
        };

        for (int p: memberPorts)
        {
            rv = hwbcm_vlan_port_add(vid, p, 0);

            if (rv < 0)
            {
                SWSS_LOG_ERROR("rif %s: hwbcm_vlan_port_add(%d, %d) failed: %d", srif.c_str(), vid, p, rv);
                rollbackVlan();
                return bcmRvToSai(rv);
            }
            addedPorts.push_back(p);

            rv = hwbcm_port_pvid_set(p, vid);

            if (rv < 0)
            {
                SWSS_LOG_ERROR("rif %s: hwbcm_port_pvid_set(%d, %d) failed: %d", srif.c_str(), p, vid, rv);
                rollbackVlan();
                return bcmRvToSai(rv);
            }
            pvidPorts.push_back(p);
        }
    }

    int intf = -1;

    int rv = hwbcm_l3_intf_create(mac, vid, vrf, mtu, &intf);

    if (rv < 0)
    {
        SWSS_LOG_ERROR("rif %s: hwbcm_l3_intf_create(vid=%d vrf=%d) failed: %d", srif.c_str(), vid, vrf, rv);

        if (dedicatedVlan)
        {
            int defVid = vidOf(m_default_vlan_id);
            if (defVid < 0)
            {
                defVid = 1;
            }
            for (int p: memberPorts)
            {
                int rrv = hwbcm_port_pvid_set(p, defVid);
                if (rrv < 0)
                {
                    SWSS_LOG_ERROR("rif %s: rollback hwbcm_port_pvid_set(%d, %d) failed: %d", srif.c_str(), p, defVid, rrv);
                }
                rrv = hwbcm_vlan_port_remove(vid, p);
                if (rrv < 0)
                {
                    SWSS_LOG_ERROR("rif %s: rollback hwbcm_vlan_port_remove(%d, %d) failed: %d", srif.c_str(), vid, p, rrv);
                }
            }
            int drv = hwbcm_vlan_destroy(vid);
            if (drv < 0)
            {
                SWSS_LOG_ERROR("rif %s: rollback hwbcm_vlan_destroy(%d) failed: %d", srif.c_str(), vid, drv);
            }
        }

        return bcmRvToSai(rv);
    }

    m_rifToIntf[rif_id] = intf;
    m_rifToVid[rif_id] = vid;

    SWSS_LOG_NOTICE("rif %s: intf %d vid %d vrf %d port %d trunk %d mtu %d",
            srif.c_str(), intf, vid, vrf, port, trunk, mtu);

    if (type == SAI_ROUTER_INTERFACE_TYPE_VLAN)
    {
        vxlanSviSync(vid); // RIOT: the VLAN may already carry a VNI
    }

    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchBCM56860HW::hwSetRouterInterface(
        _In_ sai_object_id_t rif_id,
        _In_ const sai_attribute_t *attr)
{
    SWSS_LOG_ENTER();

    auto it = m_rifToIntf.find(rif_id);

    if (it == m_rifToIntf.end())
    {
        // loopback / unsupported type: nothing in hw
        return SAI_STATUS_SUCCESS;
    }

    switch (attr->id)
    {
        case SAI_ROUTER_INTERFACE_ATTR_MTU:
            {
                int rv = hwbcm_l3_intf_mtu_set(it->second, (int)attr->value.u32);

                if (rv < 0)
                {
                    SWSS_LOG_ERROR("rif %s: hwbcm_l3_intf_mtu_set(%d, %u) failed: %d",
                            sai_serialize_object_id(rif_id).c_str(), it->second, attr->value.u32, rv);
                    return bcmRvToSai(rv);
                }

                return SAI_STATUS_SUCCESS;
            }

        case SAI_ROUTER_INTERFACE_ATTR_ADMIN_V4_STATE:
        case SAI_ROUTER_INTERFACE_ATTR_ADMIN_V6_STATE:
            SWSS_LOG_NOTICE("rif %s: admin %s state %d (routing stays enabled on the vlan)",
                    sai_serialize_object_id(rif_id).c_str(),
                    attr->id == SAI_ROUTER_INTERFACE_ATTR_ADMIN_V4_STATE ? "v4" : "v6",
                    attr->value.booldata ? 1 : 0);
            return SAI_STATUS_SUCCESS;

        case SAI_ROUTER_INTERFACE_ATTR_SRC_MAC_ADDRESS:
            SWSS_LOG_WARN("rif %s: changing SRC_MAC after create not supported in hw",
                    sai_serialize_object_id(rif_id).c_str());
            return SAI_STATUS_SUCCESS;

        default:
            SWSS_LOG_INFO("rif %s: attr %d ignored by hw", sai_serialize_object_id(rif_id).c_str(), attr->id);
            return SAI_STATUS_SUCCESS;
    }
}

sai_status_t SwitchBCM56860HW::hwRemoveRouterInterface(
        _In_ sai_object_id_t rif_id)
{
    SWSS_LOG_ENTER();

    std::string srif = sai_serialize_object_id(rif_id);

    auto it = m_rifToIntf.find(rif_id);

    if (it == m_rifToIntf.end())
    {
        return SAI_STATUS_SUCCESS;
    }

    {
        sai_attribute_t rt;
        auto v = m_rifToVid.find(rif_id);

        if (v != m_rifToVid.end() &&
                getStoredAttr(SAI_OBJECT_TYPE_ROUTER_INTERFACE, rif_id, SAI_ROUTER_INTERFACE_ATTR_TYPE, rt) &&
                rt.value.s32 == SAI_ROUTER_INTERFACE_TYPE_VLAN)
        {
            vxlanSviSync(v->second, rif_id); // RIOT: drop the VPN's L3 gateway first
        }
    }

    int rv = hwbcm_l3_intf_destroy(it->second);

    if (rv < 0)
    {
        SWSS_LOG_ERROR("rif %s: hwbcm_l3_intf_destroy(%d) failed: %d", srif.c_str(), it->second, rv);
        return bcmRvToSai(rv);
    }

    m_rifToIntf.erase(it);

    auto vit = m_rifToVid.find(rif_id);

    if (vit != m_rifToVid.end())
    {
        int vid = vit->second;

        sai_attribute_t rifType;
        bool isPortRif = getStoredAttr(SAI_OBJECT_TYPE_ROUTER_INTERFACE, rif_id, SAI_ROUTER_INTERFACE_ATTR_TYPE, rifType) &&
                         (rifType.value.s32 == SAI_ROUTER_INTERFACE_TYPE_PORT);

        if (isPortRif && vid >= RIF_VID_MIN && vid <= RIF_VID_MAX)
        {
            // port RIF: move the port(s) back to the default vlan, drop the dedicated vlan

            int defVid = vidOf(m_default_vlan_id);

            if (defVid < 0)
            {
                defVid = 1;
            }

            sai_attribute_t portAttr;

            if (getStoredAttr(SAI_OBJECT_TYPE_ROUTER_INTERFACE, rif_id, SAI_ROUTER_INTERFACE_ATTR_PORT_ID, portAttr))
            {
                std::vector<int> ports;
                sai_object_id_t target = portAttr.value.oid;

                if (objectTypeQuery(target) == SAI_OBJECT_TYPE_LAG)
                {
                    auto lm = m_lagMembers.find(target);

                    if (lm != m_lagMembers.end())
                    {
                        std::lock_guard<std::mutex> lock(m_mapMutex);

                        for (auto poid: lm->second)
                        {
                            auto pit = m_portToBcm.find(poid);

                            if (pit != m_portToBcm.end())
                            {
                                ports.push_back(pit->second);
                            }
                        }
                    }
                }
                else
                {
                    int p = bcmPortOf(target);

                    if (p >= 0)
                    {
                        ports.push_back(p);
                    }
                }

                for (int p: ports)
                {
                    int prv = hwbcm_port_pvid_set(p, defVid);
                    if (prv < 0)
                    {
                        SWSS_LOG_ERROR("rif %s: hwbcm_port_pvid_set(%d, %d) failed: %d", srif.c_str(), p, defVid, prv);
                    }
                    int rrv = hwbcm_vlan_port_remove(vid, p);
                    if (rrv < 0)
                    {
                        SWSS_LOG_ERROR("rif %s: hwbcm_vlan_port_remove(%d, %d) failed: %d", srif.c_str(), vid, p, rrv);
                    }
                }
            }

            rv = hwbcm_vlan_destroy(vid);

            if (rv < 0)
            {
                SWSS_LOG_WARN("rif %s: hwbcm_vlan_destroy(%d) failed: %d", srif.c_str(), vid, rv);
            }
        }

        m_rifToVid.erase(vit);
    }

    SWSS_LOG_NOTICE("rif %s removed", srif.c_str());

    return SAI_STATUS_SUCCESS;
}

// --- NEIGHBOR_ENTRY -------------------------------------------------------

sai_status_t SwitchBCM56860HW::hwCreateNeighbor(
        _In_ const std::string &serialized,
        _In_ uint32_t attr_count,
        _In_ const sai_attribute_t *attr_list)
{
    SWSS_LOG_ENTER();

    sai_neighbor_entry_t ne;
    sai_deserialize_neighbor_entry(serialized, ne);

    auto *macAttr = findAttr(attr_count, attr_list, SAI_NEIGHBOR_ENTRY_ATTR_DST_MAC_ADDRESS);

    if (macAttr == nullptr)
    {
        SWSS_LOG_ERROR("neighbor %s: missing DST_MAC_ADDRESS", serialized.c_str());
        return SAI_STATUS_FAILURE;
    }

    bool noHostRoute = false;

    auto *nhrAttr = findAttr(attr_count, attr_list, SAI_NEIGHBOR_ENTRY_ATTR_NO_HOST_ROUTE);

    if (nhrAttr != nullptr)
    {
        noHostRoute = nhrAttr->value.booldata;
    }

    sai_packet_action_t action = SAI_PACKET_ACTION_FORWARD;

    auto *paAttr = findAttr(attr_count, attr_list, SAI_NEIGHBOR_ENTRY_ATTR_PACKET_ACTION);

    if (paAttr != nullptr)
    {
        action = (sai_packet_action_t)paAttr->value.s32;
    }

    auto rit = m_rifToIntf.find(ne.rif_id);

    if (rit == m_rifToIntf.end())
    {
        SWSS_LOG_WARN("neighbor %s: rif %s has no hw intf (loopback?), ignored",
                serialized.c_str(), sai_serialize_object_id(ne.rif_id).c_str());
        return SAI_STATUS_SUCCESS;
    }

    int intf = rit->second;

    // vrf of the RIF

    int vrf = 0;

    sai_attribute_t vrAttr;

    if (getStoredAttr(SAI_OBJECT_TYPE_ROUTER_INTERFACE, ne.rif_id, SAI_ROUTER_INTERFACE_ATTR_VIRTUAL_ROUTER_ID, vrAttr))
    {
        sai_status_t st = resolveVrf(m_vrToVrf, defaultVirtualRouterId(), vrAttr.value.oid, true, serialized.c_str(), vrf);

        if (st != SAI_STATUS_SUCCESS)
        {
            return st;
        }
    }

    // where does the neighbor live: port / trunk / VXLAN VP, or unknown (-> CPU)
    int port = -1;
    int trunk = -1;
    int vpGport = -1;
    int ovlIntf = -1;
    bool toCpu = false;

    if (!neighborTarget(ne.rif_id, macAttr->value.mac, port, trunk, vpGport, ovlIntf))
    {
        SWSS_LOG_NOTICE("neighbor %s: mac not in fdb yet, punting to cpu", serialized.c_str());
        toCpu = true;
    }

    // egress object: reuse the one a NEXT_HOP (same rif+ip) already created,
    // rewriting it in place; otherwise create a new one.

    int egr = -1;
    std::vector<sai_object_id_t> matchingNh;

    {
        auto nhIt = m_nhByRifIp.find(makeRifIpKey(ne.rif_id, ne.ip_address));

        if (nhIt != m_nhByRifIp.end())
        {
            for (auto nh: nhIt->second)
            {
                if (m_nhToEgr.find(nh) != m_nhToEgr.end())
                {
                    matchingNh.push_back(nh);
                }
            }
        }
    }

    std::vector<int> replacedEgrs;

    auto revertReplaced = [&]()
    {
        for (int reEgr: replacedEgrs)
        {
            (void)hwbcm_l3_egress_cpu_replace(reEgr, intf);
        }
    };

    if (action == SAI_PACKET_ACTION_DROP)
    {
        if (!matchingNh.empty())
        {
            for (auto nh: matchingNh)
            {
                int e = m_nhToEgr[nh];
                int rv = hwbcm_l3_egress_drop_replace(e);

                if (rv < 0)
                {
                    SWSS_LOG_ERROR("neighbor %s: drop egress %d replace failed: %d", serialized.c_str(), e, rv);
                    revertReplaced();
                    return bcmRvToSai(rv);
                }

                replacedEgrs.push_back(e);

                if (egr < 0)
                {
                    egr = e;
                }
            }
        }
        else
        {
            int rv = hwbcm_l3_egress_drop_create(&egr);

            if (rv < 0)
            {
                SWSS_LOG_ERROR("neighbor %s: hwbcm_l3_egress_drop_create failed: %d", serialized.c_str(), rv);
                return bcmRvToSai(rv);
            }
        }
    }
    else if (!matchingNh.empty())
    {
        for (auto nh: matchingNh)
        {
            int e = m_nhToEgr[nh];
            int rv;

            if (toCpu)
            {
                rv = hwbcm_l3_egress_cpu_replace(e, intf);
            }
            else if (vpGport != -1)
            {
                rv = hwbcm_vxlan_overlay_egress_replace(e, ovlIntf, macAttr->value.mac, vpGport);
            }
            else
            {
                rv = hwbcm_l3_egress_replace(e, intf, macAttr->value.mac, port, trunk);
            }

            if (rv < 0)
            {
                SWSS_LOG_ERROR("neighbor %s: egress %d replace failed: %d", serialized.c_str(), e, rv);
                revertReplaced();
                return bcmRvToSai(rv);
            }

            replacedEgrs.push_back(e);

            SWSS_LOG_INFO("neighbor %s: next hop %s egress %d resolved (port %d trunk %d cpu %d)",
                    serialized.c_str(), sai_serialize_object_id(nh).c_str(), e, port, trunk, toCpu ? 1 : 0);

            if (egr < 0)
            {
                egr = e;
            }
        }
    }
    else
    {
        int rv;

        if (toCpu)
        {
            rv = hwbcm_l3_egress_cpu_create(intf, &egr);
        }
        else if (vpGport != -1)
        {
            rv = hwbcm_vxlan_overlay_egress_create(ovlIntf, macAttr->value.mac, vpGport, &egr);
        }
        else
        {
            rv = hwbcm_l3_egress_create(intf, macAttr->value.mac, port, trunk, &egr);
        }

        if (rv < 0)
        {
            SWSS_LOG_ERROR("neighbor %s: egress create failed: %d", serialized.c_str(), rv);
            return bcmRvToSai(rv);
        }
    }

    // host entry

    if (!noHostRoute)
    {
        uint8_t addr[16];
        int af = ipBytes(ne.ip_address, addr);

        int rv = hwbcm_l3_host_add(vrf, af, addr, egr);

        if (rv < 0)
        {
            SWSS_LOG_ERROR("neighbor %s: hwbcm_l3_host_add failed: %d", serialized.c_str(), rv);

            if (!replacedEgrs.empty())
            {
                revertReplaced();
            }
            else if (matchingNh.empty() && m_egrNhRefCount.find(egr) == m_egrNhRefCount.end())
            {
                hwbcm_l3_egress_destroy(egr);
            }

            return bcmRvToSai(rv);
        }
    }

    auto existing = m_neighborEgr.find(serialized);
    if (existing != m_neighborEgr.end())
    {
        egrNeighborRefDec(existing->second);
    }
    m_neighborEgr[serialized] = egr;
    egrNeighborRefInc(egr);
    neighborVlanIndexAdd(serialized, ne.rif_id, macAttr->value.mac);
    if (ne.ip_address.addr_family == SAI_IP_ADDR_FAMILY_IPV4)
    {
        v4NeighborIndexAdd(vrf, ne.ip_address.addr.ip4, serialized, egr);
    }

    SWSS_LOG_INFO("neighbor %s -> egress %d (intf %d vrf %d port %d trunk %d vp 0x%x/ovl %d cpu %d host %d)",
            serialized.c_str(), egr, intf, vrf, port, trunk, vpGport, ovlIntf, toCpu ? 1 : 0, noHostRoute ? 0 : 1);

    if (m_hwReady)
    {
        hwVxlanRetryPendingForNeighbor(ne.ip_address, egr); // A4: a remote VTEP may have been waiting for this underlay
    }

    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchBCM56860HW::hwSetNeighbor(
        _In_ const std::string &serialized,
        _In_ const sai_attribute_t *attr)
{
    SWSS_LOG_ENTER();

    if (attr == nullptr)
    {
        return SAI_STATUS_INVALID_PARAMETER;
    }

    auto it = m_neighborEgr.find(serialized);

    if (it == m_neighborEgr.end())
    {
        // loopback / unsupported RIF: nothing in hw
        return SAI_STATUS_SUCCESS;
    }

    if (attr->id != SAI_NEIGHBOR_ENTRY_ATTR_DST_MAC_ADDRESS &&
        attr->id != SAI_NEIGHBOR_ENTRY_ATTR_PACKET_ACTION &&
        attr->id != SAI_NEIGHBOR_ENTRY_ATTR_NO_HOST_ROUTE)
    {
        SWSS_LOG_INFO("neighbor %s: attr %d ignored by hw", serialized.c_str(), attr->id);
        return SAI_STATUS_SUCCESS;
    }

    sai_neighbor_entry_t ne;
    sai_deserialize_neighbor_entry(serialized, ne);

    auto rit = m_rifToIntf.find(ne.rif_id);

    if (rit == m_rifToIntf.end())
    {
        return SAI_STATUS_SUCCESS;
    }

    int intf = rit->second;

    if (attr->id == SAI_NEIGHBOR_ENTRY_ATTR_NO_HOST_ROUTE)
    {
        int vrf = 0;
        sai_attribute_t vrAttr;

        if (getStoredAttr(SAI_OBJECT_TYPE_ROUTER_INTERFACE, ne.rif_id, SAI_ROUTER_INTERFACE_ATTR_VIRTUAL_ROUTER_ID, vrAttr))
        {
            sai_object_id_t defaultVr = SAI_NULL_OBJECT_ID;
            sai_attribute_t sw;
            if (getStoredAttr(SAI_OBJECT_TYPE_SWITCH, m_switch_id, SAI_SWITCH_ATTR_DEFAULT_VIRTUAL_ROUTER_ID, sw))
            {
                defaultVr = sw.value.oid;
            }
            sai_status_t st = resolveVrf(m_vrToVrf, defaultVr, vrAttr.value.oid, false, serialized.c_str(), vrf);

            if (st != SAI_STATUS_SUCCESS)
            {
                return st;
            }
        }

        uint8_t addr[16];
        int af = ipBytes(ne.ip_address, addr);

        int rv = attr->value.booldata
            ? hwbcm_l3_host_delete(vrf, af, addr)
            : hwbcm_l3_host_add(vrf, af, addr, it->second);

        if (rv < 0 && !(attr->value.booldata && rv == HWBCM_E_NOT_FOUND))
        {
            SWSS_LOG_ERROR("neighbor %s: host route update (no_host_route=%d) failed: %d",
                    serialized.c_str(), attr->value.booldata ? 1 : 0, rv);
            return bcmRvToSai(rv);
        }

        return SAI_STATUS_SUCCESS;
    }

    // DST_MAC_ADDRESS or PACKET_ACTION: update neighbor egress and any matching NEXT_HOP egresses.
    sai_attribute_t macAttr, paAttr;
    sai_mac_t mac = { 0 };
    bool haveMac = false;

    if (attr->id == SAI_NEIGHBOR_ENTRY_ATTR_DST_MAC_ADDRESS)
    {
        memcpy(mac, attr->value.mac, sizeof(mac));
        haveMac = true;
    }
    else if (getStoredAttr(SAI_OBJECT_TYPE_NEIGHBOR_ENTRY, serialized, SAI_NEIGHBOR_ENTRY_ATTR_DST_MAC_ADDRESS, macAttr))
    {
        memcpy(mac, macAttr.value.mac, sizeof(mac));
        haveMac = true;
    }

    sai_packet_action_t action = SAI_PACKET_ACTION_FORWARD;

    if (attr->id == SAI_NEIGHBOR_ENTRY_ATTR_PACKET_ACTION)
    {
        action = (sai_packet_action_t)attr->value.s32;
    }
    else if (getStoredAttr(SAI_OBJECT_TYPE_NEIGHBOR_ENTRY, serialized, SAI_NEIGHBOR_ENTRY_ATTR_PACKET_ACTION, paAttr))
    {
        action = (sai_packet_action_t)paAttr.value.s32;
    }

    std::set<int> egrs;
    egrs.insert(it->second);

    {
        auto nhIt = m_nhByRifIp.find(makeRifIpKey(ne.rif_id, ne.ip_address));

        if (nhIt != m_nhByRifIp.end())
        {
            for (auto nh: nhIt->second)
            {
                auto eit = m_nhToEgr.find(nh);

                if (eit != m_nhToEgr.end())
                {
                    egrs.insert(eit->second);
                }
            }
        }
    }

    int port = -1, trunk = -1, vpGport = -1, ovlIntf = -1;
    bool toCpu = false;

    if (action == SAI_PACKET_ACTION_TRAP)
    {
        toCpu = true;
    }
    else if (action != SAI_PACKET_ACTION_DROP && action != SAI_PACKET_ACTION_DENY)
    {
        if (!haveMac || !neighborTarget(ne.rif_id, mac, port, trunk, vpGport, ovlIntf))
        {
            toCpu = true;
        }
    }

    for (int e: egrs)
    {
        int rv;

        if (action == SAI_PACKET_ACTION_DROP || action == SAI_PACKET_ACTION_DENY)
        {
            rv = hwbcm_l3_egress_drop_replace(e);
        }
        else if (toCpu)
        {
            rv = hwbcm_l3_egress_cpu_replace(e, intf);
        }
        else if (vpGport != -1)
        {
            rv = hwbcm_vxlan_overlay_egress_replace(e, ovlIntf, mac, vpGport);
        }
        else
        {
            rv = hwbcm_l3_egress_replace(e, intf, mac, port, trunk);
        }

        if (rv < 0)
        {
            SWSS_LOG_ERROR("neighbor %s: set attr %d egress %d replace failed: %d",
                    serialized.c_str(), attr->id, e, rv);
            return bcmRvToSai(rv);
        }
    }

    if (attr->id == SAI_NEIGHBOR_ENTRY_ATTR_DST_MAC_ADDRESS)
    {
        neighborVlanIndexUpdateMac(serialized, mac);
    }

    SWSS_LOG_INFO("neighbor %s updated (attr %d, %zu egress(es), action %d port %d trunk %d vp 0x%x cpu %d)",
            serialized.c_str(), attr->id, egrs.size(), (int)action, port, trunk, vpGport, toCpu ? 1 : 0);

    if (m_hwReady)
    {
        hwVxlanRetryPendingForNeighbor(ne.ip_address, it->second);
    }

    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchBCM56860HW::hwRemoveNeighbor(
        _In_ const std::string &serialized)
{
    SWSS_LOG_ENTER();

    auto it = m_neighborEgr.find(serialized);

    if (it == m_neighborEgr.end())
    {
        SWSS_LOG_WARN("neighbor %s unknown to hw", serialized.c_str());
        return SAI_STATUS_SUCCESS;
    }

    int egr = it->second;

    sai_neighbor_entry_t ne;
    sai_deserialize_neighbor_entry(serialized, ne);

    bool noHostRoute = false;

    sai_attribute_t nhrAttr;

    if (getStoredAttr(SAI_OBJECT_TYPE_NEIGHBOR_ENTRY, serialized, SAI_NEIGHBOR_ENTRY_ATTR_NO_HOST_ROUTE, nhrAttr))
    {
        noHostRoute = nhrAttr.value.booldata;
    }

    int vrf = 0;

    sai_attribute_t vrAttr;

    if (getStoredAttr(SAI_OBJECT_TYPE_ROUTER_INTERFACE, ne.rif_id, SAI_ROUTER_INTERFACE_ATTR_VIRTUAL_ROUTER_ID, vrAttr))
    {
        sai_status_t st = resolveVrf(m_vrToVrf, defaultVirtualRouterId(), vrAttr.value.oid, false, serialized.c_str(), vrf);

        if (st != SAI_STATUS_SUCCESS)
        {
            return st;
        }
    }

    if (!noHostRoute)
    {
        uint8_t addr[16];
        int af = ipBytes(ne.ip_address, addr);

        int rv = hwbcm_l3_host_delete(vrf, af, addr);

        if (rv < 0)
        {
            SWSS_LOG_WARN("neighbor %s: hwbcm_l3_host_delete failed: %d (continuing)", serialized.c_str(), rv);
        }
    }

    // Collect every NEXT_HOP egress matching (rif, ip) so multiple next hops
    // sharing this neighbor are all reverted to CPU punt on remove.
    std::set<int> nhEgrs;

    {
        auto nhIt = m_nhByRifIp.find(makeRifIpKey(ne.rif_id, ne.ip_address));

        if (nhIt != m_nhByRifIp.end())
        {
            for (auto nh: nhIt->second)
            {
                auto eit = m_nhToEgr.find(nh);

                if (eit != m_nhToEgr.end())
                {
                    nhEgrs.insert(eit->second);
                }
            }
        }
    }

    if (m_egrNhRefCount.find(egr) != m_egrNhRefCount.end())
    {
        nhEgrs.insert(egr);
    }

    auto nRefIt = m_egrNeighborRefCount.find(egr);
    bool otherNeighborUsesEgr = (nRefIt != m_egrNeighborRefCount.end() && nRefIt->second > 1);

    auto eraseNeighborIndexes = [&]()
    {
        m_neighborEgr.erase(it);
        egrNeighborRefDec(egr);
        neighborVlanIndexRemove(serialized);
        if (ne.ip_address.addr_family == SAI_IP_ADDR_FAMILY_IPV4)
        {
            v4NeighborIndexRemove(vrf, ne.ip_address.addr.ip4, serialized);
        }
    };

    if (!nhEgrs.empty())
    {
        // next hop(s) still reference it: back to punt-to-cpu
        int intf = -1;

        auto rit = m_rifToIntf.find(ne.rif_id);

        if (rit != m_rifToIntf.end())
        {
            intf = rit->second;
        }

        for (int e: nhEgrs)
        {
            int rv = hwbcm_l3_egress_cpu_replace(e, intf);

            if (rv < 0)
            {
                SWSS_LOG_ERROR("neighbor %s: egress %d cpu replace failed: %d", serialized.c_str(), e, rv);
                return bcmRvToSai(rv);
            }
        }

        if (nhEgrs.find(egr) == nhEgrs.end() && !otherNeighborUsesEgr)
        {
            int rv = hwbcm_l3_egress_destroy(egr);

            if (rv < 0 && rv != HWBCM_E_NOT_FOUND)
            {
                SWSS_LOG_ERROR("neighbor %s: hwbcm_l3_egress_destroy(%d) failed: %d", serialized.c_str(), egr, rv);
                return bcmRvToSai(rv);
            }
        }

        eraseNeighborIndexes();
        SWSS_LOG_INFO("neighbor %s removed, %zu next-hop egress(es) now punt to cpu", serialized.c_str(), nhEgrs.size());
    }
    else if (!otherNeighborUsesEgr)
    {
        int rv = hwbcm_l3_egress_destroy(egr);

        if (rv < 0 && rv != HWBCM_E_NOT_FOUND)
        {
            SWSS_LOG_ERROR("neighbor %s: hwbcm_l3_egress_destroy(%d) failed: %d", serialized.c_str(), egr, rv);

            if (!noHostRoute)
            {
                uint8_t addr[16];
                int af = ipBytes(ne.ip_address, addr);
                (void)hwbcm_l3_host_add(vrf, af, addr, egr);
            }

            return bcmRvToSai(rv);
        }

        eraseNeighborIndexes();
        SWSS_LOG_INFO("neighbor %s removed, egress %d destroyed", serialized.c_str(), egr);
    }
    else
    {
        eraseNeighborIndexes();
    }

    return SAI_STATUS_SUCCESS;
}

// --- NEXT_HOP -------------------------------------------------------------

sai_status_t SwitchBCM56860HW::hwCreateNextHop(
        _In_ sai_object_id_t nh_id,
        _In_ uint32_t attr_count,
        _In_ const sai_attribute_t *attr_list)
{
    SWSS_LOG_ENTER();

    std::string snh = sai_serialize_object_id(nh_id);

    sai_next_hop_type_t type = SAI_NEXT_HOP_TYPE_IP;

    auto *typeAttr = findAttr(attr_count, attr_list, SAI_NEXT_HOP_ATTR_TYPE);

    if (typeAttr != nullptr)
    {
        type = (sai_next_hop_type_t)typeAttr->value.s32;
    }

    switch (type)
    {
        case SAI_NEXT_HOP_TYPE_IP:
            break;

        default:
            SWSS_LOG_ERROR("next hop %s: type %d not supported in hardware", snh.c_str(), (int)type);
            return SAI_STATUS_NOT_SUPPORTED;
    }

    auto *ipAttr = findAttr(attr_count, attr_list, SAI_NEXT_HOP_ATTR_IP);
    auto *rifAttr = findAttr(attr_count, attr_list, SAI_NEXT_HOP_ATTR_ROUTER_INTERFACE_ID);

    if (ipAttr == nullptr || rifAttr == nullptr)
    {
        SWSS_LOG_ERROR("next hop %s: missing IP or ROUTER_INTERFACE_ID", snh.c_str());
        return SAI_STATUS_FAILURE;
    }

    // existing neighbor on the same rif+ip ?

    sai_neighbor_entry_t ne;
    memset(&ne, 0, sizeof(ne));
    ne.switch_id = m_switch_id;
    ne.rif_id = rifAttr->value.oid;
    ne.ip_address = ipAttr->value.ipaddr;

    std::string key = sai_serialize_neighbor_entry(ne);

    auto nit = m_neighborEgr.find(key);

    if (nit != m_neighborEgr.end())
    {
        m_nhToEgr[nh_id] = nit->second;
        egrNhRefInc(nit->second);
        nhIndexAdd(nh_id, rifAttr->value.oid, ipAttr->value.ipaddr);

        SWSS_LOG_INFO("next hop %s -> egress %d (shared with neighbor %s)", snh.c_str(), nit->second, key.c_str());

        if (m_hwReady)
        {
            hwVxlanRetryPendingForNextHop(nit->second);
        }

        return SAI_STATUS_SUCCESS;
    }

    // no neighbor yet: punt to cpu until it resolves

    int intf = -1;

    auto rit = m_rifToIntf.find(rifAttr->value.oid);

    if (rit != m_rifToIntf.end())
    {
        intf = rit->second;
    }
    else
    {
        SWSS_LOG_WARN("next hop %s: rif %s has no hw intf", snh.c_str(), sai_serialize_object_id(rifAttr->value.oid).c_str());
    }

    int egr = -1;

    int rv = hwbcm_l3_egress_cpu_create(intf, &egr);

    if (rv < 0)
    {
        SWSS_LOG_ERROR("next hop %s: hwbcm_l3_egress_cpu_create failed: %d", snh.c_str(), rv);
        return bcmRvToSai(rv);
    }

    m_nhToEgr[nh_id] = egr;
    egrNhRefInc(egr);
    nhIndexAdd(nh_id, rifAttr->value.oid, ipAttr->value.ipaddr);

    SWSS_LOG_INFO("next hop %s -> egress %d (cpu, neighbor %s unresolved)", snh.c_str(), egr, key.c_str());

    if (m_hwReady)
    {
        hwVxlanRetryPendingForNextHop(egr);
    }

    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchBCM56860HW::hwRemoveNextHop(
        _In_ sai_object_id_t nh_id)
{
    SWSS_LOG_ENTER();

    auto it = m_nhToEgr.find(nh_id);

    if (it == m_nhToEgr.end())
    {
        return SAI_STATUS_SUCCESS;
    }

    int egr = it->second;

    auto nCnt = m_egrNeighborRefCount.find(egr);
    auto nhCnt = m_egrNhRefCount.find(egr);
    bool stillUsed = (nCnt != m_egrNeighborRefCount.end() && nCnt->second > 0) ||
                     (nhCnt != m_egrNhRefCount.end() && nhCnt->second > 1);

    if (stillUsed)
    {
        m_nhToEgr.erase(it);
        egrNhRefDec(egr);
        nhIndexRemove(nh_id);
        SWSS_LOG_INFO("next hop 0x%" PRIx64 " removed, egress %d still in use", nh_id, egr);
        return SAI_STATUS_SUCCESS;
    }

    int rv = hwbcm_l3_egress_destroy(egr);

    if (rv < 0 && rv != HWBCM_E_NOT_FOUND)
    {
        SWSS_LOG_ERROR("next hop %s: hwbcm_l3_egress_destroy(%d) failed: %d", sai_serialize_object_id(nh_id).c_str(), egr, rv);
        return bcmRvToSai(rv);
    }

    m_nhToEgr.erase(it);
    egrNhRefDec(egr);
    nhIndexRemove(nh_id);

    return SAI_STATUS_SUCCESS;
}

// --- NEXT_HOP_GROUP -------------------------------------------------------

sai_status_t SwitchBCM56860HW::hwCreateNextHopGroup(
        _In_ sai_object_id_t nhg_id,
        _In_ uint32_t attr_count,
        _In_ const sai_attribute_t *attr_list)
{
    SWSS_LOG_ENTER();

    auto *typeAttr = findAttr(attr_count, attr_list, SAI_NEXT_HOP_GROUP_ATTR_TYPE);

    if (typeAttr != nullptr)
    {
        switch ((sai_next_hop_group_type_t)typeAttr->value.s32)
        {
            case SAI_NEXT_HOP_GROUP_TYPE_DYNAMIC_UNORDERED_ECMP:
            case SAI_NEXT_HOP_GROUP_TYPE_DYNAMIC_ORDERED_ECMP:
                break;

            default:
                SWSS_LOG_WARN("next hop group %s: type %d not supported in hw, ignored",
                        sai_serialize_object_id(nhg_id).c_str(), typeAttr->value.s32);
                return SAI_STATUS_SUCCESS;
        }
    }

    // the ecmp object is created when the first member arrives

    SWSS_LOG_NOTICE("next hop group %s created (ecmp deferred to first member)", sai_serialize_object_id(nhg_id).c_str());

    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchBCM56860HW::hwRemoveNextHopGroup(
        _In_ sai_object_id_t nhg_id)
{
    SWSS_LOG_ENTER();

    auto it = m_nhgToEcmp.find(nhg_id);

    if (it != m_nhgToEcmp.end())
    {
        int rv = hwbcm_l3_ecmp_destroy(it->second);

        if (rv < 0 && rv != HWBCM_E_NOT_FOUND)
        {
            SWSS_LOG_ERROR("next hop group %s: hwbcm_l3_ecmp_destroy(%d) failed: %d",
                    sai_serialize_object_id(nhg_id).c_str(), it->second, rv);
            return bcmRvToSai(rv);
        }

        m_ecmpToNhg.erase(it->second);
        m_nhgToEcmp.erase(it);
    }

    // drop stale members (should be gone already)

    auto gmit = m_nhgToMembers.find(nhg_id);
    if (gmit != m_nhgToMembers.end())
    {
        for (auto memberOid: gmit->second)
        {
            m_nhgMemberToNh.erase(memberOid);
            m_nhgMemberToNhg.erase(memberOid);
        }
        m_nhgToMembers.erase(gmit);
    }

    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchBCM56860HW::hwCreateNextHopGroupMember(
        _In_ sai_object_id_t member_id,
        _In_ uint32_t attr_count,
        _In_ const sai_attribute_t *attr_list)
{
    SWSS_LOG_ENTER();

    std::string sm = sai_serialize_object_id(member_id);

    auto *nhgAttr = findAttr(attr_count, attr_list, SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID);
    auto *nhAttr = findAttr(attr_count, attr_list, SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID);

    if (nhgAttr == nullptr || nhAttr == nullptr)
    {
        SWSS_LOG_ERROR("nhg member %s: missing NEXT_HOP_GROUP_ID or NEXT_HOP_ID", sm.c_str());
        return SAI_STATUS_FAILURE;
    }

    sai_object_id_t nhg = nhgAttr->value.oid;
    sai_object_id_t nh = nhAttr->value.oid;

    if (m_nhToEgr.find(nh) == m_nhToEgr.end())
    {
        SWSS_LOG_ERROR("nhg member %s: next hop %s has no egress", sm.c_str(), sai_serialize_object_id(nh).c_str());
        return SAI_STATUS_FAILURE;
    }

    m_nhgMemberToNh[member_id] = nh;
    m_nhgMemberToNhg[member_id] = nhg;
    m_nhgToMembers[nhg].insert(member_id);

    std::vector<int> egrs = groupEgressList(nhg, m_nhgToMembers, m_nhgMemberToNh, m_nhToEgr);

    auto eit = m_nhgToEcmp.find(nhg);

    int rv;

    if (eit == m_nhgToEcmp.end())
    {
        int ecmp = -1;

        rv = hwbcm_l3_ecmp_create(egrs.data(), (int)egrs.size(), &ecmp);

        if (rv < 0)
        {
            SWSS_LOG_ERROR("nhg member %s: hwbcm_l3_ecmp_create failed: %d", sm.c_str(), rv);
            m_nhgMemberToNh.erase(member_id);
            m_nhgMemberToNhg.erase(member_id);
            m_nhgToMembers[nhg].erase(member_id);
            if (m_nhgToMembers[nhg].empty()) m_nhgToMembers.erase(nhg);
            return bcmRvToSai(rv);
        }

        m_nhgToEcmp[nhg] = ecmp;
        m_ecmpToNhg[ecmp] = nhg;

        SWSS_LOG_NOTICE("next hop group %s -> ecmp %d (%zu members)", sai_serialize_object_id(nhg).c_str(), ecmp, egrs.size());
    }
    else
    {
        rv = hwbcm_l3_ecmp_set(eit->second, egrs.data(), (int)egrs.size());

        if (rv < 0)
        {
            SWSS_LOG_ERROR("nhg member %s: hwbcm_l3_ecmp_set(%d) failed: %d", sm.c_str(), eit->second, rv);
            m_nhgMemberToNh.erase(member_id);
            m_nhgMemberToNhg.erase(member_id);
            m_nhgToMembers[nhg].erase(member_id);
            if (m_nhgToMembers[nhg].empty()) m_nhgToMembers.erase(nhg);
            return bcmRvToSai(rv);
        }

        SWSS_LOG_NOTICE("next hop group %s ecmp %d now %zu members", sai_serialize_object_id(nhg).c_str(), eit->second, egrs.size());
    }

    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchBCM56860HW::hwRemoveNextHopGroupMember(
        _In_ sai_object_id_t member_id)
{
    SWSS_LOG_ENTER();

    auto git = m_nhgMemberToNhg.find(member_id);

    if (git == m_nhgMemberToNhg.end())
    {
        return SAI_STATUS_SUCCESS;
    }

    sai_object_id_t nhg = git->second;
    bool hadNh = false;
    sai_object_id_t savedNh = SAI_NULL_OBJECT_ID;

    auto nit = m_nhgMemberToNh.find(member_id);

    if (nit != m_nhgMemberToNh.end())
    {
        hadNh = true;
        savedNh = nit->second;
    }

    m_nhgMemberToNhg.erase(git);
    m_nhgMemberToNh.erase(member_id);
    auto gmit = m_nhgToMembers.find(nhg);
    if (gmit != m_nhgToMembers.end())
    {
        gmit->second.erase(member_id);
        if (gmit->second.empty())
        {
            m_nhgToMembers.erase(gmit);
        }
    }

    auto eit = m_nhgToEcmp.find(nhg);

    if (eit == m_nhgToEcmp.end())
    {
        return SAI_STATUS_SUCCESS;
    }

    std::vector<int> egrs = groupEgressList(nhg, m_nhgToMembers, m_nhgMemberToNh, m_nhToEgr);
    bool empty = egrs.empty();

    if (empty)
    {
        int dropEgr = sharedDropEgress();

        if (dropEgr < 0)
        {
            m_nhgMemberToNhg[member_id] = nhg;
            m_nhgToMembers[nhg].insert(member_id);
            if (hadNh)
            {
                m_nhgMemberToNh[member_id] = savedNh;
            }

            SWSS_LOG_ERROR("nhg member %s: failed to allocate drop egress for empty ecmp %d",
                    sai_serialize_object_id(member_id).c_str(), eit->second);
            return SAI_STATUS_FAILURE;
        }

        egrs.push_back(dropEgr);
    }

    int rv = hwbcm_l3_ecmp_set(eit->second, egrs.data(), (int)egrs.size());

    if (rv < 0)
    {
        m_nhgMemberToNhg[member_id] = nhg;
        m_nhgToMembers[nhg].insert(member_id);
        if (hadNh)
        {
            m_nhgMemberToNh[member_id] = savedNh;
        }

        SWSS_LOG_ERROR("nhg member %s: hwbcm_l3_ecmp_set(%d) failed: %d",
                sai_serialize_object_id(member_id).c_str(), eit->second, rv);
        return bcmRvToSai(rv);
    }

    SWSS_LOG_NOTICE("next hop group %s ecmp %d now %zu members%s",
            sai_serialize_object_id(nhg).c_str(), eit->second,
            empty ? (size_t)0 : egrs.size(),
            empty ? " (drop)" : "");

    return SAI_STATUS_SUCCESS;
}

// --- ROUTE_ENTRY ----------------------------------------------------------

sai_status_t SwitchBCM56860HW::hwCreateRoute(
        _In_ const std::string &serialized,
        _In_ uint32_t attr_count,
        _In_ const sai_attribute_t *attr_list)
{
    SWSS_LOG_ENTER();

    sai_route_entry_t re;
    sai_deserialize_route_entry(serialized, re);

    // vrf

    int vrf = 0;
    sai_status_t st = resolveVrf(m_vrToVrf, defaultVirtualRouterId(), re.vr_id, true, serialized.c_str(), vrf);

    if (st != SAI_STATUS_SUCCESS)
    {
        return st;
    }

    // action + next hop

    sai_packet_action_t action = SAI_PACKET_ACTION_FORWARD;

    auto *paAttr = findAttr(attr_count, attr_list, SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION);

    if (paAttr != nullptr)
    {
        action = (sai_packet_action_t)paAttr->value.s32;
    }

    sai_object_id_t nhOid = SAI_NULL_OBJECT_ID;

    auto *nhAttr = findAttr(attr_count, attr_list, SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID);

    if (nhAttr != nullptr)
    {
        nhOid = nhAttr->value.oid;
    }

    int egr = -1;

    switch (action)
    {
        case SAI_PACKET_ACTION_DROP:
        case SAI_PACKET_ACTION_DENY:
            egr = sharedDropEgress();
            break;

        case SAI_PACKET_ACTION_TRAP:
        case SAI_PACKET_ACTION_LOG:
        case SAI_PACKET_ACTION_COPY:
            egr = sharedCpuEgress();
            break;

        case SAI_PACKET_ACTION_FORWARD:
        case SAI_PACKET_ACTION_TRANSIT:
        case SAI_PACKET_ACTION_COPY_CANCEL:
        default:
            if (nhOid == SAI_NULL_OBJECT_ID)
            {
                SWSS_LOG_NOTICE("route %s: forward with null next hop, dropping", serialized.c_str());
                egr = sharedDropEgress();
                break;
            }

            if (nhOid == m_cpu_port_id)
            {
                egr = sharedCpuEgress();
                break;
            }

            switch (objectTypeQuery(nhOid))
            {
                case SAI_OBJECT_TYPE_NEXT_HOP:
                    {
                        auto it = m_nhToEgr.find(nhOid);

                        if (it == m_nhToEgr.end())
                        {
                            SWSS_LOG_ERROR("route %s: next hop %s has no egress", serialized.c_str(), sai_serialize_object_id(nhOid).c_str());
                            return SAI_STATUS_FAILURE;
                        }

                        egr = it->second;
                        break;
                    }

                case SAI_OBJECT_TYPE_NEXT_HOP_GROUP:
                    {
                        auto it = m_nhgToEcmp.find(nhOid);

                        if (it == m_nhgToEcmp.end())
                        {
                            // group without members yet: create the ecmp now (drop egress when empty)
                            std::vector<int> egrs = groupEgressList(nhOid, m_nhgToMembers, m_nhgMemberToNh, m_nhToEgr);

                            if (egrs.empty())
                            {
                                int dropEgr = sharedDropEgress();

                                if (dropEgr < 0)
                                {
                                    SWSS_LOG_ERROR("route %s: next hop group %s empty and drop egress unavailable",
                                            serialized.c_str(), sai_serialize_object_id(nhOid).c_str());
                                    return SAI_STATUS_FAILURE;
                                }

                                egrs.push_back(dropEgr);
                            }

                            int ecmp = -1;

                            int rv = hwbcm_l3_ecmp_create(egrs.data(), (int)egrs.size(), &ecmp);

                            if (rv < 0)
                            {
                                SWSS_LOG_ERROR("route %s: next hop group %s has no ecmp and create failed: %d",
                                        serialized.c_str(), sai_serialize_object_id(nhOid).c_str(), rv);
                                return bcmRvToSai(rv);
                            }

                            m_nhgToEcmp[nhOid] = ecmp;
                            m_ecmpToNhg[ecmp] = nhOid;
                            egr = ecmp;
                        }
                        else
                        {
                            egr = it->second;
                        }

                        break;
                    }

                case SAI_OBJECT_TYPE_ROUTER_INTERFACE:
                    // directly connected subnet: punt so the kernel resolves neighbors
                    egr = sharedCpuEgress();
                    break;

                case SAI_OBJECT_TYPE_PORT:
                    SWSS_LOG_WARN("route %s: next hop is port %s (not cpu), punting", serialized.c_str(), sai_serialize_object_id(nhOid).c_str());
                    egr = sharedCpuEgress();
                    break;

                default:
                    SWSS_LOG_ERROR("route %s: unsupported next hop object %s", serialized.c_str(), sai_serialize_object_id(nhOid).c_str());
                    return SAI_STATUS_FAILURE;
            }

            break;
    }

    if (egr < 0)
    {
        SWSS_LOG_ERROR("route %s: could not resolve an egress object", serialized.c_str());
        return SAI_STATUS_FAILURE;
    }

    uint8_t prefix[16];
    int len = 0;
    int af = prefixBytes(re.destination, prefix, len);

    int rv = hwbcm_l3_route_add(vrf, af, prefix, len, egr);

    if (rv < 0)
    {
        SWSS_LOG_ERROR("route %s: hwbcm_l3_route_add(vrf=%d) failed: %d", serialized.c_str(), vrf, rv);
        return bcmRvToSai(rv);
    }

    m_routeEgr[serialized] = egr;
    if (af == 4)
    {
        v4RouteIndexAdd(vrf, re.destination.addr.ip4, len, egr);
    }

    SWSS_LOG_INFO("route %s -> %d (vrf %d action %d)", serialized.c_str(), egr, vrf, (int)action);

    if (m_hwReady)
    {
        hwVxlanRetryPendingForRoute(re.destination);
    }

    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchBCM56860HW::hwSetRoute(
        _In_ const std::string &serialized,
        _In_ const sai_attribute_t *attr)
{
    SWSS_LOG_ENTER();

    if (attr->id != SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID && attr->id != SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION)
    {
        SWSS_LOG_INFO("route %s: attr %d ignored by hw", serialized.c_str(), attr->id);
        return SAI_STATUS_SUCCESS;
    }

    // rebuild the (action, next hop) pair from the new attr + stored other one,
    // then re-add: hwbcm_l3_route_add replaces an existing prefix in place.

    sai_attribute_t attrs[2];
    uint32_t n = 0;

    attrs[n++] = *attr;

    sai_attr_id_t other = (attr->id == SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID)
        ? SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION
        : SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID;

    if (getStoredAttr(SAI_OBJECT_TYPE_ROUTE_ENTRY, serialized, other, attrs[n]))
    {
        n++;
    }

    return hwCreateRoute(serialized, n, attrs);
}

sai_status_t SwitchBCM56860HW::hwRemoveRoute(
        _In_ const std::string &serialized)
{
    SWSS_LOG_ENTER();

    auto it = m_routeEgr.find(serialized);

    if (it == m_routeEgr.end())
    {
        SWSS_LOG_WARN("route %s unknown to hw", serialized.c_str());
        return SAI_STATUS_SUCCESS;
    }

    sai_route_entry_t re;
    sai_deserialize_route_entry(serialized, re);

    int vrf = 0;
    sai_status_t st = resolveVrf(m_vrToVrf, defaultVirtualRouterId(), re.vr_id, false, serialized.c_str(), vrf);

    if (st != SAI_STATUS_SUCCESS)
    {
        return st;
    }

    uint8_t prefix[16];
    int len = 0;
    int af = prefixBytes(re.destination, prefix, len);

    int rv = hwbcm_l3_route_delete(vrf, af, prefix, len);

    if (rv < 0 && rv != HWBCM_E_NOT_FOUND)
    {
        SWSS_LOG_ERROR("route %s: hwbcm_l3_route_delete(vrf=%d) failed: %d", serialized.c_str(), vrf, rv);
        return bcmRvToSai(rv);
    }

    m_routeEgr.erase(it);
    if (af == 4)
    {
        v4RouteIndexRemove(vrf, re.destination.addr.ip4, len);
    }

    return SAI_STATUS_SUCCESS;
}

void SwitchBCM56860HW::hwRefreshNeighborsOnLearn(
        _In_ sai_object_id_t vlanOid,
        _In_ const uint8_t mac[6],
        _In_ int port,
        _In_ int trunk)
{
    SWSS_LOG_ENTER();

    /*
     * hwCreateNeighbor() punts a neighbor on a VLAN RIF to the CPU when its MAC
     * is not in the FDB yet. That is the normal case right after a syncd
     * restart: orchagent re-creates the neighbors from the kernel cache before
     * the ASIC has re-learned anything, and it never touches the neighbor
     * again (neighorch does not observe FDB changes). The Broadcom SAI fixes
     * this up internally; do the same here: on a learn of (vlan, mac) rewrite
     * the egress of every neighbor with that MAC on that VLAN's RIF in place
     * (host entry, next hops, routes and ECMP members keep the egress id), and
     * let the remote VTEPs re-check their underlay (a tunnel cloned from the
     * punt egress is stale). Also covers a MAC moving to another port.
     */

    auto idxIt = m_vlanNeighborsByMac.find(makeVlanMacKey(vlanOid, mac));

    if (idxIt == m_vlanNeighborsByMac.end())
    {
        return;
    }

    bool changed = false;

    for (const auto &neKey: idxIt->second)
    {
        auto nIt = m_neighborEgr.find(neKey);

        if (nIt == m_neighborEgr.end())
        {
            continue;
        }

        sai_neighbor_entry_t ne;
        sai_deserialize_neighbor_entry(neKey, ne);

        sai_attribute_t paAttr;

        if (getStoredAttr(SAI_OBJECT_TYPE_NEIGHBOR_ENTRY, neKey, SAI_NEIGHBOR_ENTRY_ATTR_PACKET_ACTION, paAttr) &&
                paAttr.value.s32 == SAI_PACKET_ACTION_DROP)
        {
            continue;
        }

        auto rit = m_rifToIntf.find(ne.rif_id);

        if (rit == m_rifToIntf.end())
        {
            continue;
        }

        std::set<int> egrs;
        egrs.insert(nIt->second);

        {
            auto nhIt = m_nhByRifIp.find(makeRifIpKey(ne.rif_id, ne.ip_address));

            if (nhIt != m_nhByRifIp.end())
            {
                for (auto nh: nhIt->second)
                {
                    auto eit = m_nhToEgr.find(nh);

                    if (eit != m_nhToEgr.end())
                    {
                        egrs.insert(eit->second);
                    }
                }
            }
        }

        for (int e: egrs)
        {
            int rv = hwbcm_l3_egress_replace(e, rit->second, mac, port, trunk);

            if (rv < 0)
            {
                SWSS_LOG_ERROR("neighbor %s: egress %d re-resolve after fdb learn failed: %d",
                        neKey.c_str(), e, rv);
                continue;
            }

            SWSS_LOG_NOTICE("neighbor %s: egress %d re-resolved after fdb learn (port %d trunk %d)",
                    neKey.c_str(), e, port, trunk);

            changed = true;
        }
    }

    if (changed && m_hwReady)
    {
        hwVxlanRetryPending();
    }
}

void SwitchBCM56860HW::hwPuntNeighborsOnAge(
        _In_ sai_object_id_t vlanOid,
        _In_ const uint8_t mac[6])
{
    SWSS_LOG_ENTER();

    /*
     * Mirror of hwRefreshNeighborsOnLearn(): when a dynamic (vlan, mac) leaves
     * the L2 table (ASIC age-out, or a flush on link down / VLAN member
     * removal) every neighbor on that VLAN's RIF still pointing at the old
     * port would keep forwarding into a port the host may no longer be behind.
     * The Broadcom SAI falls back to punting in that case so the kernel re-ARPs
     * and the reply re-learns the MAC (which then brings hwRefreshNeighborsOnLearn
     * back in). Rewrite the egress in place (ids stay valid for host entries,
     * next hops, routes and ECMP members) and let the remote VTEPs notice
     * their underlay clone is stale.
     */

    if (!m_hwReady)
    {
        return;
    }

    auto idxIt = m_vlanNeighborsByMac.find(makeVlanMacKey(vlanOid, mac));

    if (idxIt == m_vlanNeighborsByMac.end())
    {
        return;
    }

    bool changed = false;

    for (const auto &neKey: idxIt->second)
    {
        auto nIt = m_neighborEgr.find(neKey);

        if (nIt == m_neighborEgr.end())
        {
            continue;
        }

        sai_neighbor_entry_t ne;
        sai_deserialize_neighbor_entry(neKey, ne);

        sai_attribute_t paAttr;

        if (getStoredAttr(SAI_OBJECT_TYPE_NEIGHBOR_ENTRY, neKey, SAI_NEIGHBOR_ENTRY_ATTR_PACKET_ACTION, paAttr) &&
                paAttr.value.s32 == SAI_PACKET_ACTION_DROP)
        {
            continue; // a drop egress stays a drop egress
        }

        int intf = -1;

        auto rit = m_rifToIntf.find(ne.rif_id);

        if (rit != m_rifToIntf.end())
        {
            intf = rit->second;
        }

        std::set<int> egrs;
        egrs.insert(nIt->second);

        {
            auto nhIt = m_nhByRifIp.find(makeRifIpKey(ne.rif_id, ne.ip_address));

            if (nhIt != m_nhByRifIp.end())
            {
                for (auto nh: nhIt->second)
                {
                    auto eit = m_nhToEgr.find(nh);

                    if (eit != m_nhToEgr.end())
                    {
                        egrs.insert(eit->second);
                    }
                }
            }
        }

        for (int e: egrs)
        {
            int rv = hwbcm_l3_egress_cpu_replace(e, intf);

            if (rv < 0)
            {
                SWSS_LOG_ERROR("neighbor %s: egress %d punt after fdb age/flush failed: %d",
                        neKey.c_str(), e, rv);
                continue;
            }

            SWSS_LOG_NOTICE("neighbor %s: mac %s left the fdb, egress %d punts to cpu until re-learned",
                    neKey.c_str(), sai_serialize_mac(mac).c_str(), e);

            changed = true;
        }
    }

    if (changed)
    {
        hwVxlanRetryPending(); // a tunnel cloned from this egress is stale now -> pending
    }
}

// ---------------------------------------------------------------------------
// neighbor target resolution (shared by create and re-resolve)
// ---------------------------------------------------------------------------

bool SwitchBCM56860HW::neighborTarget(
        _In_ sai_object_id_t rif_id,
        _In_ const sai_mac_t mac,
        _Out_ int &port,
        _Out_ int &trunk,
        _Out_ int &vpGport,
        _Out_ int &ovlIntf)
{
    SWSS_LOG_ENTER();

    port = -1;
    trunk = -1;
    vpGport = -1;
    ovlIntf = -1;

    sai_attribute_t rifType;

    if (!getStoredAttr(SAI_OBJECT_TYPE_ROUTER_INTERFACE, rif_id, SAI_ROUTER_INTERFACE_ATTR_TYPE, rifType))
    {
        SWSS_LOG_ERROR("rif %s has no TYPE attribute", sai_serialize_object_id(rif_id).c_str());
        return false;
    }

    if (rifType.value.s32 == SAI_ROUTER_INTERFACE_TYPE_PORT)
    {
        sai_attribute_t portAttr;

        if (getStoredAttr(SAI_OBJECT_TYPE_ROUTER_INTERFACE, rif_id, SAI_ROUTER_INTERFACE_ATTR_PORT_ID, portAttr))
        {
            sai_object_id_t target = portAttr.value.oid;

            if (objectTypeQuery(target) == SAI_OBJECT_TYPE_LAG)
            {
                trunk = trunkOf(target);
            }
            else
            {
                port = bcmPortOf(target);
            }
        }

        return port >= 0 || trunk >= 0;
    }

    if (rifType.value.s32 != SAI_ROUTER_INTERFACE_TYPE_VLAN)
    {
        return false;
    }

    sai_attribute_t vlanAttr;

    if (!getStoredAttr(SAI_OBJECT_TYPE_ROUTER_INTERFACE, rif_id, SAI_ROUTER_INTERFACE_ATTR_VLAN_ID, vlanAttr))
    {
        return false;
    }

    sai_object_id_t vlanOid = vlanAttr.value.oid;

    auto fdbIt = m_objectHash.find(SAI_OBJECT_TYPE_FDB_ENTRY);

    if (fdbIt == m_objectHash.end())
    {
        return false;
    }

    sai_fdb_entry_t fe;
    memset(&fe, 0, sizeof(fe));
    fe.switch_id = m_switch_id;
    fe.bv_id = vlanOid;
    memcpy(fe.mac_address, mac, sizeof(fe.mac_address));

    auto kv = fdbIt->second.find(sai_serialize_fdb_entry(fe));

    if (kv == fdbIt->second.end())
    {
        return false;
    }

    auto bp = kv->second.find("SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID");

    if (bp == kv->second.end() || bp->second == nullptr || bp->second->getAttr() == nullptr)
    {
        return false;
    }

    sai_object_id_t bridgePort = bp->second->getAttr()->value.oid;

    // RIOT: MAC behind a remote VTEP -> routed into the VPN over the
    // VTEP's network VP (needs the VPN's SVI binding and a live VP).
    auto rip = m_bridgePortRemoteIp.find(bridgePort);

    if (rip != m_bridgePortRemoteIp.end())
    {
        int vpn = vxlanVpnOfVid(vidOf(vlanOid));
        int ovl = (vpn >= 0) ? hwbcm_vxlan_vpn_l3_intf(vpn) : -1;
        auto vt = m_vtepByIp.find(rip->second);

        if (ovl >= 0 && vt != m_vtepByIp.end() && vt->second.vpReady)
        {
            vpGport = vt->second.vp;
            ovlIntf = ovl;
            return true;
        }

        return false;
    }

    sai_object_id_t target = bridgePortTarget(bridgePort);

    if (objectTypeQuery(target) == SAI_OBJECT_TYPE_LAG)
    {
        trunk = trunkOf(target);
    }
    else
    {
        port = bcmPortOf(target);
    }

    return port >= 0 || trunk >= 0;
}

void SwitchBCM56860HW::hwReresolveNeighborsOnVlan(
        _In_ int vid,
        _In_ bool punt)
{
    SWSS_LOG_ENTER();

    auto vIt = m_vlanNeighborsByVid.find(vid);

    if (vIt == m_vlanNeighborsByVid.end())
    {
        return;
    }

    bool changed = false;

    for (const auto &neKey: vIt->second)
    {
        auto nIt = m_neighborEgr.find(neKey);

        if (nIt == m_neighborEgr.end())
        {
            continue;
        }

        sai_neighbor_entry_t ne;
        sai_deserialize_neighbor_entry(neKey, ne);

        auto rit = m_rifToIntf.find(ne.rif_id);

        if (rit == m_rifToIntf.end())
        {
            continue;
        }

        sai_attribute_t macAttr, paAttr;

        if (!getStoredAttr(SAI_OBJECT_TYPE_NEIGHBOR_ENTRY, neKey, SAI_NEIGHBOR_ENTRY_ATTR_DST_MAC_ADDRESS, macAttr))
        {
            continue;
        }

        if (getStoredAttr(SAI_OBJECT_TYPE_NEIGHBOR_ENTRY, neKey, SAI_NEIGHBOR_ENTRY_ATTR_PACKET_ACTION, paAttr) &&
                paAttr.value.s32 == SAI_PACKET_ACTION_DROP)
        {
            continue;
        }

        int port, trunk, vpGport, ovlIntf;
        int rv;

        if (punt || !neighborTarget(ne.rif_id, macAttr.value.mac, port, trunk, vpGport, ovlIntf))
        {
            port = trunk = vpGport = ovlIntf = -1;
            rv = hwbcm_l3_egress_cpu_replace(nIt->second, rit->second);
        }
        else if (vpGport != -1)
        {
            rv = hwbcm_vxlan_overlay_egress_replace(nIt->second, ovlIntf, macAttr.value.mac, vpGport);
        }
        else
        {
            rv = hwbcm_l3_egress_replace(nIt->second, rit->second, macAttr.value.mac, port, trunk);
        }

        if (rv < 0)
        {
            SWSS_LOG_ERROR("neighbor %s: egress %d re-resolve failed: %d", neKey.c_str(), nIt->second, rv);
            continue;
        }

        SWSS_LOG_INFO("neighbor %s: egress %d re-resolved (port %d trunk %d vp 0x%x ovl %d)",
                neKey.c_str(), nIt->second, port, trunk, vpGport, ovlIntf);
        changed = true;
    }

    // Remote VTEPs only depend on underlay neighbors; a VNI-mapped VLAN is
    // overlay, and re-checking the VTEPs from here fed back into this function
    // (VTEP rebuild -> remote MAC install -> re-resolve).
    if (changed && m_hwReady && vxlanVpnOfVid(vid) < 0)
    {
        hwVxlanRetryPending();
    }
}

// ---------------------------------------------------------------------------
// RIOT: SVI (L3 gateway) on a VNI-mapped VLAN
// ---------------------------------------------------------------------------

void SwitchBCM56860HW::vxlanSviSync(
        _In_ int vid,
        _In_ sai_object_id_t removingRif)
{
    SWSS_LOG_ENTER();

    if (!m_hwReady || !hwbcm_riot_enabled())
    {
        return;
    }

    int vpn = vxlanVpnOfVid(vid);

    if (vpn < 0)
    {
        return; // no VNI: hwbcm_vxlan_vpn_destroy already dropped any binding
    }

    // the VLAN RIF of vid (not a port RIF's internal VLAN)
    sai_object_id_t rif = SAI_NULL_OBJECT_ID;

    for (auto &kv: m_rifToVid)
    {
        sai_attribute_t rt;

        if (kv.second != vid || kv.first == removingRif)
        {
            continue;
        }

        if (getStoredAttr(SAI_OBJECT_TYPE_ROUTER_INTERFACE, kv.first, SAI_ROUTER_INTERFACE_ATTR_TYPE, rt) &&
                rt.value.s32 == SAI_ROUTER_INTERFACE_TYPE_VLAN)
        {
            rif = kv.first;
            break;
        }
    }

    if (rif == SAI_NULL_OBJECT_ID)
    {
        if (hwbcm_vxlan_vpn_l3_intf(vpn) >= 0)
        {
            hwReresolveNeighborsOnVlan(vid, true); // release overlay egresses first
            int rc = hwbcm_vxlan_vpn_l3_unbind(vpn);
            SWSS_LOG_NOTICE("vlan %d (vpn 0x%x): SVI removed, L3 gateway unbound: %d", vid, vpn, rc);
        }
        return;
    }

    if (hwbcm_vxlan_vpn_l3_intf(vpn) >= 0)
    {
        return; // already bound
    }

    sai_mac_t mac;
    memset(mac, 0, sizeof(mac));
    sai_attribute_t a;

    if (getStoredAttr(SAI_OBJECT_TYPE_ROUTER_INTERFACE, rif, SAI_ROUTER_INTERFACE_ATTR_SRC_MAC_ADDRESS, a))
    {
        memcpy(mac, a.value.mac, sizeof(mac));
    }
    else if (getStoredAttr(SAI_OBJECT_TYPE_SWITCH, m_switch_id, SAI_SWITCH_ATTR_SRC_MAC_ADDRESS, a))
    {
        memcpy(mac, a.value.mac, sizeof(mac));
    }

    int mtu = 9100;

    if (getStoredAttr(SAI_OBJECT_TYPE_ROUTER_INTERFACE, rif, SAI_ROUTER_INTERFACE_ATTR_MTU, a))
    {
        mtu = (int)a.value.u32;
    }

    int vrf = 0;

    if (getStoredAttr(SAI_OBJECT_TYPE_ROUTER_INTERFACE, rif, SAI_ROUTER_INTERFACE_ATTR_VIRTUAL_ROUTER_ID, a))
    {
        sai_object_id_t defaultVr = SAI_NULL_OBJECT_ID;
        sai_attribute_t sw;

        if (getStoredAttr(SAI_OBJECT_TYPE_SWITCH, m_switch_id, SAI_SWITCH_ATTR_DEFAULT_VIRTUAL_ROUTER_ID, sw))
        {
            defaultVr = sw.value.oid;
        }

        sai_status_t st = resolveVrf(m_vrToVrf, defaultVr, a.value.oid, true, "svi", vrf);

        if (st != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("vlan %d (vpn 0x%x): resolveVrf failed: %d", vid, vpn, st);
            return;
        }
    }

    int ovl = -1;
    int rc = hwbcm_vxlan_vpn_l3_bind(vpn, mac, vrf, mtu, &ovl);

    if (rc < 0)
    {
        SWSS_LOG_ERROR("vlan %d (vpn 0x%x): L3 gateway bind failed: %d", vid, vpn, rc);
        return;
    }

    SWSS_LOG_NOTICE("vlan %d (vpn 0x%x): SVI bound, overlay intf %d vrf %d mtu %d", vid, vpn, ovl, vrf, mtu);

    hwReresolveNeighborsOnVlan(vid); // remote hosts can now be routed in hardware
}

void SwitchBCM56860HW::hwL3GetSharedEgress(
        _Out_ int &cpuEgr,
        _Out_ int &dropEgr) const
{
    cpuEgr = g_cpuEgr;
    dropEgr = g_dropEgr;
}

void SwitchBCM56860HW::hwL3RestoreSharedEgress(
        _In_ int cpuEgr,
        _In_ int dropEgr)
{
    g_cpuEgr = cpuEgr;
    g_dropEgr = dropEgr;
}
