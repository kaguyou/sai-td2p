/*
 * SwitchBCM56860HW_mirror.cpp — port mirroring (SPAN / RSPAN / ERSPAN) on the
 * Trident2+ hardware switch:
 *   - SAI_OBJECT_TYPE_MIRROR_SESSION (LOCAL = SPAN, REMOTE = RSPAN,
 *     ENHANCED_REMOTE = ERSPAN with IPv4 L3 GRE encapsulation)
 *   - Port mirroring (SAI_PORT_ATTR_INGRESS_MIRROR_SESSION and
 *     SAI_PORT_ATTR_EGRESS_MIRROR_SESSION)
 *   - ACL integration hook: calls hwAclMirrorSessionUpdated(session_oid, gport)
 *     when a session's bcm mirror destination is created/updated, and with -1
 *     before it is destroyed (see notes/integration_acl.md 1.7).
 *
 * Called by the dispatcher in SwitchBCM56860HW.cpp after SwitchBCM56850 has
 * updated the object hash (create/set) or before it removes the object
 * (remove), so every operation reads the full attribute set from the store.
 *
 * For ERSPAN, SONiC's MirrorOrch resolves the route/neighbor/FDB to the
 * destination IP and passes MONITOR_PORT, SRC/DST MAC, and (when egressing a
 * VLAN) VLAN_HEADER_VALID + VLAN_ID/TPID/PRI/CFI directly on the mirror
 * session, updating them via set_mirror_session_attribute() when the path
 * changes. Because OpenBCM ESW only rewrites the software descriptor on
 * BCM_MIRROR_DEST_REPLACE and programs MTP/EGR_MIRROR_ENCAP tables on attach,
 * live session updates detach port bindings and ACL entries, recreate (or
 * replace) the destination, and re-attach them.
 */

#include "SwitchBCM56860HW.h"
#include "hwbcm/hwbcm.h"
#include "hwbcm/hwbcm_mirror.h"

#include "swss/logger.h"
#include "meta/sai_serialize.h"

#include <arpa/inet.h>

#include <cstring>
#include <set>
#include <utility>
#include <vector>

using namespace saivs;

namespace
{
    sai_status_t mirrorHwStatus(int rv)
    {
        SWSS_LOG_ENTER();

        switch (rv)
        {
            case 0:                         return SAI_STATUS_SUCCESS;
            case HWBCM_MIRROR_E_PARAM:      return SAI_STATUS_INVALID_PARAMETER;
            case HWBCM_MIRROR_E_NOT_FOUND:  return SAI_STATUS_ITEM_NOT_FOUND;
            case HWBCM_MIRROR_E_EXISTS:     return SAI_STATUS_ITEM_ALREADY_EXISTS;
            case HWBCM_MIRROR_E_BUSY:       return SAI_STATUS_OBJECT_IN_USE;
            case HWBCM_MIRROR_E_RESOURCE:   return SAI_STATUS_INSUFFICIENT_RESOURCES;
            case HWBCM_MIRROR_E_UNAVAIL:    return SAI_STATUS_NOT_SUPPORTED;
            default:                        return SAI_STATUS_FAILURE;
        }
    }

    const char *mirrorTypeStr(int type)
    {
        switch (type)
        {
            case HWBCM_MIRROR_TYPE_LOCAL:  return "SPAN";
            case HWBCM_MIRROR_TYPE_RSPAN:  return "RSPAN";
            case HWBCM_MIRROR_TYPE_ERSPAN: return "ERSPAN";
            default:                       return "?";
        }
    }

    std::string ipv4HostStr(uint32_t ipHost)
    {
        struct in_addr in;
        char buf[INET_ADDRSTRLEN] = { 0 };

        in.s_addr = htonl(ipHost);

        if (inet_ntop(AF_INET, &in, buf, sizeof(buf)) == NULL)
        {
            return "?";
        }

        return std::string(buf);
    }

    bool isMirrorHwAttr(sai_attr_id_t id)
    {
        switch (id)
        {
            case SAI_MIRROR_SESSION_ATTR_MONITOR_PORT:
            case SAI_MIRROR_SESSION_ATTR_MONITOR_PORTLIST:
            case SAI_MIRROR_SESSION_ATTR_TC:
            case SAI_MIRROR_SESSION_ATTR_VLAN_HEADER_VALID:
            case SAI_MIRROR_SESSION_ATTR_VLAN_TPID:
            case SAI_MIRROR_SESSION_ATTR_VLAN_ID:
            case SAI_MIRROR_SESSION_ATTR_VLAN_PRI:
            case SAI_MIRROR_SESSION_ATTR_VLAN_CFI:
            case SAI_MIRROR_SESSION_ATTR_IPHDR_VERSION:
            case SAI_MIRROR_SESSION_ATTR_TOS:
            case SAI_MIRROR_SESSION_ATTR_TTL:
            case SAI_MIRROR_SESSION_ATTR_SRC_IP_ADDRESS:
            case SAI_MIRROR_SESSION_ATTR_DST_IP_ADDRESS:
            case SAI_MIRROR_SESSION_ATTR_SRC_MAC_ADDRESS:
            case SAI_MIRROR_SESSION_ATTR_DST_MAC_ADDRESS:
            case SAI_MIRROR_SESSION_ATTR_GRE_PROTOCOL_TYPE:
                return true;

            default:
                return false;
        }
    }
}

bool SwitchBCM56860HW::MirrorCfgSnapshot::sameAs(
        _In_ const MirrorCfgSnapshot &o) const
{
    return type == o.type &&
           port == o.port &&
           trunkId == o.trunkId &&
           tc == o.tc &&
           vlanValid == o.vlanValid &&
           vlanTpid == o.vlanTpid &&
           vlanId == o.vlanId &&
           vlanPri == o.vlanPri &&
           vlanCfi == o.vlanCfi &&
           srcIp == o.srcIp &&
           dstIp == o.dstIp &&
           std::memcmp(srcMac, o.srcMac, sizeof(srcMac)) == 0 &&
           std::memcmp(dstMac, o.dstMac, sizeof(dstMac)) == 0 &&
           tos == o.tos &&
           ttl == o.ttl &&
           greProto == o.greProto &&
           payloadUntagged == o.payloadUntagged;
}

static void snapshotToC(
        _In_ const SwitchBCM56860HW::MirrorCfgSnapshot &s,
        _Out_ hwbcm_mirror_dest_cfg_t &c)
{
    hwbcm_mirror_dest_cfg_init(&c);
    c.type = s.type;
    c.port = s.port;
    c.trunk_id = s.trunkId;
    c.tc = s.tc;
    c.vlan_valid = s.vlanValid;
    c.vlan_tpid = s.vlanTpid;
    c.vlan_id = s.vlanId;
    c.vlan_pri = s.vlanPri;
    c.vlan_cfi = s.vlanCfi;
    c.src_ip = s.srcIp;
    c.dst_ip = s.dstIp;
    std::memcpy(c.src_mac, s.srcMac, sizeof(c.src_mac));
    std::memcpy(c.dst_mac, s.dstMac, sizeof(c.dst_mac));
    c.tos = s.tos;
    c.ttl = s.ttl;
    c.gre_protocol = s.greProto;
    c.payload_untagged = s.payloadUntagged;
}

// ---------------------------------------------------------------------------
// attribute parsing
// ---------------------------------------------------------------------------

sai_status_t SwitchBCM56860HW::mirrorReadConfig(
        _In_ sai_object_id_t session_id,
        _Out_ MirrorCfgSnapshot &out)
{
    SWSS_LOG_ENTER();

    const std::string sid = sai_serialize_object_id(session_id);

    out = MirrorCfgSnapshot();

    sai_attribute_t a;

    // 1. Session type (mandatory)
    if (!getStoredAttr(SAI_OBJECT_TYPE_MIRROR_SESSION, session_id, SAI_MIRROR_SESSION_ATTR_TYPE, a))
    {
        SWSS_LOG_ERROR("mirror session %s: missing SAI_MIRROR_SESSION_ATTR_TYPE", sid.c_str());
        return SAI_STATUS_MANDATORY_ATTRIBUTE_MISSING;
    }

    switch (a.value.s32)
    {
        case SAI_MIRROR_SESSION_TYPE_LOCAL:
            out.type = HWBCM_MIRROR_TYPE_LOCAL;
            break;

        case SAI_MIRROR_SESSION_TYPE_REMOTE:
            out.type = HWBCM_MIRROR_TYPE_RSPAN;
            break;

        case SAI_MIRROR_SESSION_TYPE_ENHANCED_REMOTE:
            out.type = HWBCM_MIRROR_TYPE_ERSPAN;
            break;

        case SAI_MIRROR_SESSION_TYPE_SFLOW:
            SWSS_LOG_ERROR("mirror session %s: SFLOW mirror type not supported on TD2+", sid.c_str());
            return SAI_STATUS_NOT_SUPPORTED;

        default:
            SWSS_LOG_ERROR("mirror session %s: unknown type %d", sid.c_str(), a.value.s32);
            return SAI_STATUS_INVALID_PARAMETER;
    }

    // 2. Monitor port (or single-entry monitor portlist)
    bool usePortList = false;

    if (getStoredAttr(SAI_OBJECT_TYPE_MIRROR_SESSION, session_id, SAI_MIRROR_SESSION_ATTR_MONITOR_PORTLIST_VALID, a))
    {
        usePortList = a.value.booldata;
    }

    sai_object_id_t monitorOid = SAI_NULL_OBJECT_ID;

    if (usePortList)
    {
        if (!getStoredAttr(SAI_OBJECT_TYPE_MIRROR_SESSION, session_id, SAI_MIRROR_SESSION_ATTR_MONITOR_PORTLIST, a) ||
            a.value.objlist.count == 0 || a.value.objlist.list == NULL)
        {
            SWSS_LOG_ERROR("mirror session %s: MONITOR_PORTLIST_VALID is true but MONITOR_PORTLIST is empty",
                    sid.c_str());
            return SAI_STATUS_INVALID_PARAMETER;
        }

        if (a.value.objlist.count > 1)
        {
            SWSS_LOG_ERROR("mirror session %s: multi-port MONITOR_PORTLIST (count %u) not supported on TD2+",
                    sid.c_str(), a.value.objlist.count);
            return SAI_STATUS_NOT_SUPPORTED;
        }

        monitorOid = a.value.objlist.list[0];
    }
    else
    {
        if (!getStoredAttr(SAI_OBJECT_TYPE_MIRROR_SESSION, session_id, SAI_MIRROR_SESSION_ATTR_MONITOR_PORT, a))
        {
            SWSS_LOG_ERROR("mirror session %s: missing SAI_MIRROR_SESSION_ATTR_MONITOR_PORT", sid.c_str());
            return SAI_STATUS_MANDATORY_ATTRIBUTE_MISSING;
        }

        monitorOid = a.value.oid;
    }

    int bcmPort = bcmPortOf(monitorOid);
    int trunkId = (bcmPort < 0) ? trunkOf(monitorOid) : -1;

    if (bcmPort >= 0)
    {
        out.port = bcmPort;
        out.trunkId = -1;
    }
    else if (trunkId >= 0)
    {
        out.port = -1;
        out.trunkId = trunkId;
    }
    else
    {
        SWSS_LOG_ERROR("mirror session %s: monitor port %s has no hardware port/trunk",
                sid.c_str(), sai_serialize_object_id(monitorOid).c_str());
        return SAI_STATUS_INVALID_PARAMETER;
    }

    // 3. Optional TC override
    if (getStoredAttr(SAI_OBJECT_TYPE_MIRROR_SESSION, session_id, SAI_MIRROR_SESSION_ATTR_TC, a))
    {
        if (a.value.u8 > 15)
        {
            SWSS_LOG_WARN("mirror session %s: TC %u exceeds TD2+ 4-bit int_pri, clamping to 15",
                    sid.c_str(), (unsigned)a.value.u8);
            out.tc = 15;
        }
        else
        {
            out.tc = (int)a.value.u8;
        }
    }

    // 4. Optional attributes not supported in TD2+ hardware (warn and ignore)
    if (getStoredAttr(SAI_OBJECT_TYPE_MIRROR_SESSION, session_id, SAI_MIRROR_SESSION_ATTR_TRUNCATE_SIZE, a) &&
        a.value.u16 != 0)
    {
        SWSS_LOG_WARN("mirror session %s: TRUNCATE_SIZE %u not supported on TD2+ (ignored)",
                sid.c_str(), (unsigned)a.value.u16);
    }

    if (getStoredAttr(SAI_OBJECT_TYPE_MIRROR_SESSION, session_id, SAI_MIRROR_SESSION_ATTR_SAMPLE_RATE, a) &&
        a.value.u32 != 0 && a.value.u32 != 1)
    {
        SWSS_LOG_WARN("mirror session %s: SAMPLE_RATE %u not supported on TD2+ (mirroring all packets)",
                sid.c_str(), a.value.u32);
    }

    if (getStoredAttr(SAI_OBJECT_TYPE_MIRROR_SESSION, session_id, SAI_MIRROR_SESSION_ATTR_CONGESTION_MODE, a) &&
        a.value.s32 != SAI_MIRROR_SESSION_CONGESTION_MODE_INDEPENDENT)
    {
        SWSS_LOG_WARN("mirror session %s: CONGESTION_MODE %d not supported on TD2+ (ignored)",
                sid.c_str(), a.value.s32);
    }

    if (getStoredAttr(SAI_OBJECT_TYPE_MIRROR_SESSION, session_id, SAI_MIRROR_SESSION_ATTR_POLICER, a) &&
        a.value.oid != SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_WARN("mirror session %s: mirror POLICER %s not supported on TD2+ (ignored)",
                sid.c_str(), sai_serialize_object_id(a.value.oid).c_str());
    }

    if (getStoredAttr(SAI_OBJECT_TYPE_MIRROR_SESSION, session_id, SAI_MIRROR_SESSION_ATTR_COUNTER_ID, a) &&
        a.value.oid != SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_WARN("mirror session %s: mirror COUNTER_ID %s not supported on TD2+ (ignored)",
                sid.c_str(), sai_serialize_object_id(a.value.oid).c_str());
    }

    // 5. RSPAN (L2 VLAN tunnel)
    if (out.type == HWBCM_MIRROR_TYPE_RSPAN)
    {
        if (!getStoredAttr(SAI_OBJECT_TYPE_MIRROR_SESSION, session_id, SAI_MIRROR_SESSION_ATTR_VLAN_ID, a) ||
            a.value.u16 < 1 || a.value.u16 > 4094)
        {
            SWSS_LOG_ERROR("mirror session %s: RSPAN requires VLAN_ID 1..4094", sid.c_str());
            return SAI_STATUS_INVALID_PARAMETER;
        }

        out.vlanValid = 1;
        out.vlanId = a.value.u16;

        if (getStoredAttr(SAI_OBJECT_TYPE_MIRROR_SESSION, session_id, SAI_MIRROR_SESSION_ATTR_VLAN_TPID, a) &&
            a.value.u16 != 0)
        {
            out.vlanTpid = a.value.u16;
        }

        if (getStoredAttr(SAI_OBJECT_TYPE_MIRROR_SESSION, session_id, SAI_MIRROR_SESSION_ATTR_VLAN_PRI, a))
        {
            out.vlanPri = static_cast<uint8_t>(a.value.u8 & 0x7u);
        }

        if (getStoredAttr(SAI_OBJECT_TYPE_MIRROR_SESSION, session_id, SAI_MIRROR_SESSION_ATTR_VLAN_CFI, a))
        {
            out.vlanCfi = static_cast<uint8_t>(a.value.u8 & 0x1u);
        }

        return SAI_STATUS_SUCCESS;
    }

    // 6. ERSPAN (IPv4 L3 GRE tunnel)
    if (out.type == HWBCM_MIRROR_TYPE_ERSPAN)
    {
        if (getStoredAttr(SAI_OBJECT_TYPE_MIRROR_SESSION, session_id,
                    SAI_MIRROR_SESSION_ATTR_ERSPAN_ENCAPSULATION_TYPE, a))
        {
            if (a.value.s32 != SAI_ERSPAN_ENCAPSULATION_TYPE_MIRROR_L3_GRE_TUNNEL)
            {
                SWSS_LOG_ERROR("mirror session %s: ERSPAN encap type %d not supported on TD2+ "
                        "(only MIRROR_L3_GRE_TUNNEL)", sid.c_str(), a.value.s32);
                return SAI_STATUS_NOT_SUPPORTED;
            }
        }

        if (getStoredAttr(SAI_OBJECT_TYPE_MIRROR_SESSION, session_id, SAI_MIRROR_SESSION_ATTR_IPHDR_VERSION, a) &&
            a.value.u8 != 4)
        {
            SWSS_LOG_ERROR("mirror session %s: ERSPAN IP version %u not supported on TD2+ (IPv4 only)",
                    sid.c_str(), (unsigned)a.value.u8);
            return SAI_STATUS_NOT_SUPPORTED;
        }

        if (!getStoredAttr(SAI_OBJECT_TYPE_MIRROR_SESSION, session_id, SAI_MIRROR_SESSION_ATTR_SRC_IP_ADDRESS, a))
        {
            SWSS_LOG_ERROR("mirror session %s: ERSPAN missing SRC_IP_ADDRESS", sid.c_str());
            return SAI_STATUS_MANDATORY_ATTRIBUTE_MISSING;
        }
        if (a.value.ipaddr.addr_family != SAI_IP_ADDR_FAMILY_IPV4)
        {
            SWSS_LOG_ERROR("mirror session %s: ERSPAN IPv6 SRC_IP_ADDRESS not supported on TD2+", sid.c_str());
            return SAI_STATUS_NOT_SUPPORTED;
        }
        out.srcIp = ntohl(a.value.ipaddr.addr.ip4);

        if (!getStoredAttr(SAI_OBJECT_TYPE_MIRROR_SESSION, session_id, SAI_MIRROR_SESSION_ATTR_DST_IP_ADDRESS, a))
        {
            SWSS_LOG_ERROR("mirror session %s: ERSPAN missing DST_IP_ADDRESS", sid.c_str());
            return SAI_STATUS_MANDATORY_ATTRIBUTE_MISSING;
        }
        if (a.value.ipaddr.addr_family != SAI_IP_ADDR_FAMILY_IPV4)
        {
            SWSS_LOG_ERROR("mirror session %s: ERSPAN IPv6 DST_IP_ADDRESS not supported on TD2+", sid.c_str());
            return SAI_STATUS_NOT_SUPPORTED;
        }
        out.dstIp = ntohl(a.value.ipaddr.addr.ip4);

        if (!getStoredAttr(SAI_OBJECT_TYPE_MIRROR_SESSION, session_id, SAI_MIRROR_SESSION_ATTR_SRC_MAC_ADDRESS, a))
        {
            SWSS_LOG_ERROR("mirror session %s: ERSPAN missing SRC_MAC_ADDRESS", sid.c_str());
            return SAI_STATUS_MANDATORY_ATTRIBUTE_MISSING;
        }
        std::memcpy(out.srcMac, a.value.mac, sizeof(out.srcMac));

        if (!getStoredAttr(SAI_OBJECT_TYPE_MIRROR_SESSION, session_id, SAI_MIRROR_SESSION_ATTR_DST_MAC_ADDRESS, a))
        {
            SWSS_LOG_ERROR("mirror session %s: ERSPAN missing DST_MAC_ADDRESS", sid.c_str());
            return SAI_STATUS_MANDATORY_ATTRIBUTE_MISSING;
        }
        std::memcpy(out.dstMac, a.value.mac, sizeof(out.dstMac));

        if (getStoredAttr(SAI_OBJECT_TYPE_MIRROR_SESSION, session_id, SAI_MIRROR_SESSION_ATTR_TOS, a))
        {
            out.tos = a.value.u8;
        }

        if (getStoredAttr(SAI_OBJECT_TYPE_MIRROR_SESSION, session_id, SAI_MIRROR_SESSION_ATTR_TTL, a))
        {
            out.ttl = a.value.u8;
        }

        if (getStoredAttr(SAI_OBJECT_TYPE_MIRROR_SESSION, session_id, SAI_MIRROR_SESSION_ATTR_GRE_PROTOCOL_TYPE, a) &&
            a.value.u16 != 0)
        {
            out.greProto = a.value.u16;
        }

        // Outer VLAN tag: SONiC's MirrorOrch::updateSessionType sets
        // VLAN_HEADER_VALID=true, VLAN_TPID, VLAN_ID, VLAN_PRI, VLAN_CFI in
        // separate set_mirror_session_attribute() calls. When VLAN_HEADER_VALID
        // is set before VLAN_ID arrives, vlan_id is still 0; keep outer untagged
        // until a valid VLAN_ID (1..4094) is present rather than failing step 1.
        bool vlanHdrValid = false;

        if (getStoredAttr(SAI_OBJECT_TYPE_MIRROR_SESSION, session_id, SAI_MIRROR_SESSION_ATTR_VLAN_HEADER_VALID, a))
        {
            vlanHdrValid = a.value.booldata;
        }

        if (vlanHdrValid &&
            getStoredAttr(SAI_OBJECT_TYPE_MIRROR_SESSION, session_id, SAI_MIRROR_SESSION_ATTR_VLAN_ID, a) &&
            a.value.u16 >= 1 && a.value.u16 <= 4094)
        {
            out.vlanValid = 1;
            out.vlanId = a.value.u16;

            if (getStoredAttr(SAI_OBJECT_TYPE_MIRROR_SESSION, session_id, SAI_MIRROR_SESSION_ATTR_VLAN_TPID, a) &&
                a.value.u16 != 0)
            {
                out.vlanTpid = a.value.u16;
            }

            if (getStoredAttr(SAI_OBJECT_TYPE_MIRROR_SESSION, session_id, SAI_MIRROR_SESSION_ATTR_VLAN_PRI, a))
            {
                out.vlanPri = static_cast<uint8_t>(a.value.u8 & 0x7u);
            }

            if (getStoredAttr(SAI_OBJECT_TYPE_MIRROR_SESSION, session_id, SAI_MIRROR_SESSION_ATTR_VLAN_CFI, a))
            {
                out.vlanCfi = static_cast<uint8_t>(a.value.u8 & 0x1u);
            }
        }
        else
        {
            out.vlanValid = 0;
            out.vlanId = 0;
            out.vlanTpid = 0;
            out.vlanPri = 0;
            out.vlanCfi = 0;
        }

        if (getStoredAttr(SAI_OBJECT_TYPE_MIRROR_SESSION, session_id, SAI_MIRROR_SESSION_ATTR_ERSPAN_SESSION_ID, a) &&
            a.value.u16 != 0)
        {
            SWSS_LOG_WARN("mirror session %s: ERSPAN_SESSION_ID %u not supported on TD2+ (ignored)",
                    sid.c_str(), (unsigned)a.value.u16);
        }

        if (getStoredAttr(SAI_OBJECT_TYPE_MIRROR_SESSION, session_id,
                    SAI_MIRROR_SESSION_ATTR_GRE_HEADER_FIRST_16BIT, a) &&
            a.value.u16 != 0)
        {
            SWSS_LOG_WARN("mirror session %s: GRE_HEADER_FIRST_16BIT 0x%04x not supported on TD2+ (ignored)",
                    sid.c_str(), (unsigned)a.value.u16);
        }
    }

    return SAI_STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// SAI_OBJECT_TYPE_MIRROR_SESSION handlers
// ---------------------------------------------------------------------------

sai_status_t SwitchBCM56860HW::hwCreateMirrorSession(
        _In_ sai_object_id_t session_id,
        _In_ uint32_t attr_count,
        _In_ const sai_attribute_t *attr_list)
{
    SWSS_LOG_ENTER();

    (void)attr_count;
    (void)attr_list;

    const std::string sid = sai_serialize_object_id(session_id);

    MirrorCfgSnapshot snap;

    CHECK_STATUS(mirrorReadConfig(session_id, snap));

    hwbcm_mirror_dest_cfg_t c;

    snapshotToC(snap, c);

    int destGport = -1;
    int rv = hwbcm_mirror_dest_create(&c, &destGport);

    if (rv < 0)
    {
        SWSS_LOG_ERROR("mirror session %s: hwbcm_mirror_dest_create(%s) failed: %d",
                sid.c_str(), mirrorTypeStr(snap.type), rv);
        return mirrorHwStatus(rv);
    }

    MirrorSessionHw hw;
    hw.destGport = destGport;
    hw.cfg = snap;
    m_mirrorSessions[session_id] = hw;

    // Notify ACL (notes/integration_acl.md 1.7) so any rules waiting on this
    // session receive its hardware mirror destination gport.
    hwAclMirrorSessionUpdated(session_id, destGport);

    if (snap.type == HWBCM_MIRROR_TYPE_ERSPAN)
    {
        SWSS_LOG_NOTICE("mirror session %s created: ERSPAN dest 0x%x -> %s %d "
                "src=%s dst=%s tos=0x%02x ttl=%u gre=0x%04x vlan=%u",
                sid.c_str(), (unsigned)destGport,
                snap.trunkId >= 0 ? "trunk" : "port",
                snap.trunkId >= 0 ? snap.trunkId : snap.port,
                ipv4HostStr(snap.srcIp).c_str(),
                ipv4HostStr(snap.dstIp).c_str(),
                (unsigned)snap.tos, (unsigned)snap.ttl,
                (unsigned)snap.greProto,
                snap.vlanValid ? (unsigned)snap.vlanId : 0u);
    }
    else
    {
        SWSS_LOG_NOTICE("mirror session %s created: %s dest 0x%x -> %s %d vlan=%u",
                sid.c_str(), mirrorTypeStr(snap.type), (unsigned)destGport,
                snap.trunkId >= 0 ? "trunk" : "port",
                snap.trunkId >= 0 ? snap.trunkId : snap.port,
                snap.vlanValid ? (unsigned)snap.vlanId : 0u);
    }

    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchBCM56860HW::hwSetMirrorSession(
        _In_ sai_object_id_t session_id,
        _In_ const sai_attribute_t *attr)
{
    SWSS_LOG_ENTER();

    if (attr == NULL)
    {
        return SAI_STATUS_INVALID_PARAMETER;
    }

    const std::string sid = sai_serialize_object_id(session_id);

    auto it = m_mirrorSessions.find(session_id);

    if (it == m_mirrorSessions.end())
    {
        SWSS_LOG_ERROR("mirror session %s: not found in hardware state", sid.c_str());
        return SAI_STATUS_ITEM_NOT_FOUND;
    }

    if (!isMirrorHwAttr(attr->id))
    {
        SWSS_LOG_WARN("mirror session %s: attr %d not programmed to hardware (bookkeeping only)",
                sid.c_str(), attr->id);
        return SAI_STATUS_SUCCESS;
    }

    MirrorCfgSnapshot newSnap;

    CHECK_STATUS(mirrorReadConfig(session_id, newSnap));

    // No-op when the resulting hardware configuration did not change (e.g.
    // setting VLAN_PRI=0 / VLAN_CFI=0 or setting VLAN_HEADER_VALID before
    // VLAN_ID arrives).
    if (newSnap.sameAs(it->second.cfg))
    {
        return SAI_STATUS_SUCCESS;
    }

    const int oldDest = it->second.destGport;
    const MirrorCfgSnapshot oldSnap = it->second.cfg;

    // Collect every port currently mirroring to this session so we can detach
    // before updating the destination and re-attach afterwards.
    std::vector<std::pair<int, uint32_t>> boundPorts;

    for (const auto &kv: m_portIngMirrors)
    {
        if (kv.second.count(session_id) != 0)
        {
            int bcm = bcmPortOf(kv.first);

            if (bcm >= 0)
            {
                boundPorts.emplace_back(bcm, HWBCM_MIRROR_DIR_INGRESS);
            }
        }
    }

    for (const auto &kv: m_portEgrMirrors)
    {
        if (kv.second.count(session_id) != 0)
        {
            int bcm = bcmPortOf(kv.first);

            if (bcm >= 0)
            {
                boundPorts.emplace_back(bcm, HWBCM_MIRROR_DIR_EGRESS);
            }
        }
    }

    // 1. Detach all port bindings and ACL entries so the SDK unreserves the
    //    hardware MTP slot and frees the old EGR_MIRROR_ENCAP profile.
    for (const auto &bp: boundPorts)
    {
        (void)hwbcm_mirror_port_delete(bp.first, bp.second, oldDest);
    }

    hwAclMirrorSessionUpdated(session_id, -1);

    // 2. Replace the destination. Prefer destroy + create so no stale
    //    BCM_MIRROR_DEST_REPLACE flag remains on the descriptor; fall back to
    //    in-place replace if the descriptor still has an external reference.
    hwbcm_mirror_dest_cfg_t newC;
    snapshotToC(newSnap, newC);

    int newDest = -1;
    int rv = hwbcm_mirror_dest_destroy(oldDest);

    if (rv == 0 || rv == HWBCM_MIRROR_E_NOT_FOUND)
    {
        rv = hwbcm_mirror_dest_create(&newC, &newDest);

        if (rv < 0)
        {
            SWSS_LOG_ERROR("mirror session %s: recreate after set attr %d failed: %d (restoring old)",
                    sid.c_str(), attr->id, rv);

            hwbcm_mirror_dest_cfg_t oldC;
            snapshotToC(oldSnap, oldC);

            int restoredDest = -1;

            if (hwbcm_mirror_dest_create(&oldC, &restoredDest) == 0)
            {
                it->second.destGport = restoredDest;

                for (const auto &bp: boundPorts)
                {
                    (void)hwbcm_mirror_port_add(bp.first, bp.second, restoredDest);
                }

                hwAclMirrorSessionUpdated(session_id, restoredDest);
            }
            else
            {
                SWSS_LOG_ERROR("mirror session %s: rollback recreate also failed", sid.c_str());
                it->second.destGport = -1;
            }

            return mirrorHwStatus(rv);
        }
    }
    else
    {
        SWSS_LOG_WARN("mirror session %s: destroy(0x%x) returned %d during update, falling back to replace",
                sid.c_str(), (unsigned)oldDest, rv);

        rv = hwbcm_mirror_dest_replace(oldDest, &newC);

        if (rv < 0)
        {
            SWSS_LOG_ERROR("mirror session %s: hwbcm_mirror_dest_replace(0x%x) failed: %d",
                    sid.c_str(), (unsigned)oldDest, rv);

            for (const auto &bp: boundPorts)
            {
                (void)hwbcm_mirror_port_add(bp.first, bp.second, oldDest);
            }

            hwAclMirrorSessionUpdated(session_id, oldDest);
            return mirrorHwStatus(rv);
        }

        newDest = oldDest;
    }

    it->second.destGport = newDest;
    it->second.cfg = newSnap;

    // 3. Re-attach ports and reprogram ACL entries with the updated destination.
    sai_status_t attachStatus = SAI_STATUS_SUCCESS;

    for (const auto &bp: boundPorts)
    {
        int arv = hwbcm_mirror_port_add(bp.first, bp.second, newDest);

        if (arv < 0)
        {
            SWSS_LOG_ERROR("mirror session %s: re-attach port %d dir 0x%x to dest 0x%x failed: %d",
                    sid.c_str(), bp.first, (unsigned)bp.second, (unsigned)newDest, arv);

            if (attachStatus == SAI_STATUS_SUCCESS)
            {
                attachStatus = mirrorHwStatus(arv);
            }
        }
    }

    hwAclMirrorSessionUpdated(session_id, newDest);

    SWSS_LOG_NOTICE("mirror session %s updated (attr %d): %s dest 0x%x -> %s %d vlan=%u",
            sid.c_str(), attr->id, mirrorTypeStr(newSnap.type), (unsigned)newDest,
            newSnap.trunkId >= 0 ? "trunk" : "port",
            newSnap.trunkId >= 0 ? newSnap.trunkId : newSnap.port,
            newSnap.vlanValid ? (unsigned)newSnap.vlanId : 0u);

    return attachStatus;
}

sai_status_t SwitchBCM56860HW::hwRemoveMirrorSession(
        _In_ sai_object_id_t session_id)
{
    SWSS_LOG_ENTER();

    const std::string sid = sai_serialize_object_id(session_id);

    auto it = m_mirrorSessions.find(session_id);

    if (it == m_mirrorSessions.end())
    {
        return SAI_STATUS_SUCCESS;
    }

    const int destGport = it->second.destGport;

    // Defensively detach any ports still pointing at this session.
    for (auto pit = m_portIngMirrors.begin(); pit != m_portIngMirrors.end(); )
    {
        if (pit->second.erase(session_id) != 0)
        {
            int bcm = bcmPortOf(pit->first);

            if (bcm >= 0 && destGport != -1)
            {
                (void)hwbcm_mirror_port_delete(bcm, HWBCM_MIRROR_DIR_INGRESS, destGport);
            }
        }

        if (pit->second.empty())
        {
            pit = m_portIngMirrors.erase(pit);
        }
        else
        {
            ++pit;
        }
    }

    for (auto pit = m_portEgrMirrors.begin(); pit != m_portEgrMirrors.end(); )
    {
        if (pit->second.erase(session_id) != 0)
        {
            int bcm = bcmPortOf(pit->first);

            if (bcm >= 0 && destGport != -1)
            {
                (void)hwbcm_mirror_port_delete(bcm, HWBCM_MIRROR_DIR_EGRESS, destGport);
            }
        }

        if (pit->second.empty())
        {
            pit = m_portEgrMirrors.erase(pit);
        }
        else
        {
            ++pit;
        }
    }

    // Notify ACL BEFORE destroying the bcm mirror destination (integration_acl.md 1.7)
    // so FP entries release their MTP references first.
    hwAclMirrorSessionUpdated(session_id, -1);

    if (destGport != -1)
    {
        int rv = hwbcm_mirror_dest_destroy(destGport);

        if (rv < 0 && rv != HWBCM_MIRROR_E_NOT_FOUND)
        {
            SWSS_LOG_ERROR("mirror session %s: hwbcm_mirror_dest_destroy(0x%x) failed: %d",
                    sid.c_str(), (unsigned)destGport, rv);
            return mirrorHwStatus(rv);
        }
    }

    m_mirrorSessions.erase(it);

    SWSS_LOG_NOTICE("mirror session %s removed (dest 0x%x)", sid.c_str(), (unsigned)destGport);

    return SAI_STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// Port mirroring (SAI_PORT_ATTR_INGRESS/EGRESS_MIRROR_SESSION)
// ---------------------------------------------------------------------------

bool SwitchBCM56860HW::mirrorIsPortAttr(
        _In_ sai_attr_id_t id)
{
    return id == SAI_PORT_ATTR_INGRESS_MIRROR_SESSION ||
           id == SAI_PORT_ATTR_EGRESS_MIRROR_SESSION;
}

sai_status_t SwitchBCM56860HW::mirrorHwSetPort(
        _In_ sai_object_id_t port_id,
        _In_ const sai_attribute_t *attr)
{
    SWSS_LOG_ENTER();

    if (attr == NULL || !mirrorIsPortAttr(attr->id))
    {
        return SAI_STATUS_INVALID_PARAMETER;
    }

    int bcm = bcmPortOf(port_id);

    if (bcm < 0)
    {
        // CPU port or unmapped port: bookkeeping only
        return SAI_STATUS_SUCCESS;
    }

    const bool ingress = (attr->id == SAI_PORT_ATTR_INGRESS_MIRROR_SESSION);
    const uint32_t dir = ingress ? HWBCM_MIRROR_DIR_INGRESS : HWBCM_MIRROR_DIR_EGRESS;
    auto &bindMap = ingress ? m_portIngMirrors : m_portEgrMirrors;

    std::set<sai_object_id_t> desired;

    if (attr->value.objlist.count > 0)
    {
        if (attr->value.objlist.list == NULL)
        {
            return SAI_STATUS_INVALID_PARAMETER;
        }

        for (uint32_t i = 0; i < attr->value.objlist.count; i++)
        {
            sai_object_id_t sess = attr->value.objlist.list[i];

            if (sess == SAI_NULL_OBJECT_ID)
            {
                continue;
            }

            auto sit = m_mirrorSessions.find(sess);

            if (sit == m_mirrorSessions.end() || sit->second.destGport == -1)
            {
                SWSS_LOG_ERROR("port %s (%d): %s mirror session %s has no hardware destination",
                        sai_serialize_object_id(port_id).c_str(), bcm,
                        ingress ? "ingress" : "egress",
                        sai_serialize_object_id(sess).c_str());
                return SAI_STATUS_INVALID_PARAMETER;
            }

            desired.insert(sess);
        }
    }

    const std::set<sai_object_id_t> current = bindMap.count(port_id) ? bindMap[port_id] : std::set<sai_object_id_t>();

    if (desired == current)
    {
        return SAI_STATUS_SUCCESS;
    }

    // Remove old bindings first to free MTP slots before adding new ones.
    std::vector<sai_object_id_t> removed;

    for (sai_object_id_t sess: current)
    {
        if (desired.count(sess) != 0)
        {
            continue;
        }

        auto sit = m_mirrorSessions.find(sess);

        if (sit != m_mirrorSessions.end() && sit->second.destGport != -1)
        {
            int rv = hwbcm_mirror_port_delete(bcm, dir, sit->second.destGport);

            if (rv < 0)
            {
                SWSS_LOG_WARN("port %s (%d): detach %s mirror %s (0x%x) failed: %d",
                        sai_serialize_object_id(port_id).c_str(), bcm,
                        ingress ? "ingress" : "egress",
                        sai_serialize_object_id(sess).c_str(),
                        (unsigned)sit->second.destGport, rv);
            }
            else
            {
                removed.push_back(sess);
            }
        }
    }

    // Add newly requested bindings; roll back on failure (e.g. MTP slots exhausted).
    std::vector<sai_object_id_t> added;

    for (sai_object_id_t sess: desired)
    {
        if (current.count(sess) != 0)
        {
            continue;
        }

        int dest = m_mirrorSessions[sess].destGport;
        int rv = hwbcm_mirror_port_add(bcm, dir, dest);

        if (rv < 0)
        {
            SWSS_LOG_ERROR("port %s (%d): attach %s mirror %s (0x%x) failed: %d",
                    sai_serialize_object_id(port_id).c_str(), bcm,
                    ingress ? "ingress" : "egress",
                    sai_serialize_object_id(sess).c_str(),
                    (unsigned)dest, rv);

            for (sai_object_id_t aSess: added)
            {
                (void)hwbcm_mirror_port_delete(bcm, dir, m_mirrorSessions[aSess].destGport);
            }

            for (sai_object_id_t rSess: removed)
            {
                (void)hwbcm_mirror_port_add(bcm, dir, m_mirrorSessions[rSess].destGport);
            }

            return mirrorHwStatus(rv);
        }

        added.push_back(sess);
    }

    if (desired.empty())
    {
        bindMap.erase(port_id);
    }
    else
    {
        bindMap[port_id] = desired;
    }

    SWSS_LOG_NOTICE("port %s (bcm %d): %s mirror sessions updated (%zu active)",
            sai_serialize_object_id(port_id).c_str(), bcm,
            ingress ? "ingress" : "egress", desired.size());

    return SAI_STATUS_SUCCESS;
}

void SwitchBCM56860HW::mirrorOnPortRemoved(
        _In_ sai_object_id_t port_id)
{
    SWSS_LOG_ENTER();

    int bcm = bcmPortOf(port_id);

    auto iit = m_portIngMirrors.find(port_id);

    if (iit != m_portIngMirrors.end())
    {
        if (bcm >= 0)
        {
            for (sai_object_id_t sess: iit->second)
            {
                auto sit = m_mirrorSessions.find(sess);

                if (sit != m_mirrorSessions.end() && sit->second.destGport != -1)
                {
                    (void)hwbcm_mirror_port_delete(bcm, HWBCM_MIRROR_DIR_INGRESS, sit->second.destGport);
                }
            }
        }

        m_portIngMirrors.erase(iit);
    }

    auto eit = m_portEgrMirrors.find(port_id);

    if (eit != m_portEgrMirrors.end())
    {
        if (bcm >= 0)
        {
            for (sai_object_id_t sess: eit->second)
            {
                auto sit = m_mirrorSessions.find(sess);

                if (sit != m_mirrorSessions.end() && sit->second.destGport != -1)
                {
                    (void)hwbcm_mirror_port_delete(bcm, HWBCM_MIRROR_DIR_EGRESS, sit->second.destGport);
                }
            }
        }

        m_portEgrMirrors.erase(eit);
    }
}
