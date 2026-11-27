/*
 * SwitchBCM56860HW_acl.cpp — data-plane ACL of the Trident2+ hardware switch:
 * ACL_TABLE, ACL_ENTRY, ACL_COUNTER, ACL_RANGE, ACL_TABLE_GROUP(+MEMBER) and
 * the bind points (PORT/LAG/VLAN/ROUTER_INTERFACE/SWITCH *_ACL attributes).
 *
 * Called by the dispatcher in SwitchBCM56860HW.cpp after the base class has
 * stored the object (create/set) or before it removes it (remove), so the
 * full attribute set is always read back from the vslib store: an entry is
 * (re)programmed from what is stored, never from a partial attr_list.
 *
 * Model
 * -----
 *  SAI ACL table        -> one bcm field group (IFP / EFP / VFP by stage),
 *                          created at table create (TCAM is claimed early so
 *                          a resource error reaches orchagent at table time).
 *                          Group priority: unique per stage, 10..40 (the CoPP
 *                          group lives outside that range).
 *  SAI ACL entry        -> N bcm field entries, one per "binding scope
 *                          variant". A table bound to ports/LAGs gets one
 *                          IFP entry whose InPorts is the union of the bound
 *                          ports and LAG members; each bound VLAN (or RIF,
 *                          through its VLAN) adds one entry qualified on
 *                          OuterVlanId; a switch binding is one entry without
 *                          a port qualifier. EFP has no InPorts bitmap, so an
 *                          egress table bound to k ports gets k entries
 *                          (OutPort = p). An unbound table has no hardware
 *                          entries at all.
 *  SAI ACL counter      -> the sum of the bcm field stats of every variant of
 *                          every entry using it, plus an offset that absorbs
 *                          retired variants and SAI "set PACKETS/BYTES" clears.
 *  SAI ACL range        -> bcm_field_range (FP_RANGE_CHECK).
 *  ACL table group      -> bookkeeping only (SAI parallel groups == parallel
 *                          IFP groups, which is what the hardware does).
 *
 * Reprogramming is hitless where possible: variants that survive a change are
 * rewritten in place (qualify_clear + reinstall keeps the TCAM row and the
 * stat), added variants are installed before surplus ones are removed.
 */
#include "SwitchBCM56860HW.h"
#include "hwbcm/hwbcm.h"
#include "hwbcm/hwbcm_acl.h"

#include "swss/logger.h"
#include "meta/sai_serialize.h"

#include <arpa/inet.h>

#include <cstring>
#include <set>
#include <algorithm>

using namespace saivs;

namespace
{
    /* Group priorities reserved for ACL tables in every stage (CoPP takes its
     * own IFP group outside this range). */
    const int ACL_GROUP_PRIO_MIN = 10;
    const int ACL_GROUP_PRIO_MAX = 40;

    /* Safety valve against binding x in-port x out-port explosions. */
    const size_t ACL_MAX_VARIANTS = 256;

    int hwStageOf(int32_t saiStage)
    {
        SWSS_LOG_ENTER();

        switch (saiStage)
        {
            case SAI_ACL_STAGE_INGRESS:     return HWBCM_ACL_STAGE_INGRESS;
            case SAI_ACL_STAGE_EGRESS:      return HWBCM_ACL_STAGE_EGRESS;
            case SAI_ACL_STAGE_PRE_INGRESS: return HWBCM_ACL_STAGE_LOOKUP;
            default:                        return -1;
        }
    }

    const char *stageStr(int32_t saiStage)
    {
        SWSS_LOG_ENTER();

        switch (saiStage)
        {
            case SAI_ACL_STAGE_INGRESS:     return "ingress";
            case SAI_ACL_STAGE_EGRESS:      return "egress";
            case SAI_ACL_STAGE_PRE_INGRESS: return "pre-ingress";
            default:                        return "?";
        }
    }

    /* Bind attribute -> SAI stage, -1 if not a bind attribute. */
    int32_t bindStageOf(sai_object_type_t ot, sai_attr_id_t id)
    {
        SWSS_LOG_ENTER();

        switch (ot)
        {
            case SAI_OBJECT_TYPE_PORT:
                if (id == SAI_PORT_ATTR_INGRESS_ACL) return SAI_ACL_STAGE_INGRESS;
                if (id == SAI_PORT_ATTR_EGRESS_ACL) return SAI_ACL_STAGE_EGRESS;
                break;
            case SAI_OBJECT_TYPE_LAG:
                if (id == SAI_LAG_ATTR_INGRESS_ACL) return SAI_ACL_STAGE_INGRESS;
                if (id == SAI_LAG_ATTR_EGRESS_ACL) return SAI_ACL_STAGE_EGRESS;
                break;
            case SAI_OBJECT_TYPE_VLAN:
                if (id == SAI_VLAN_ATTR_INGRESS_ACL) return SAI_ACL_STAGE_INGRESS;
                if (id == SAI_VLAN_ATTR_EGRESS_ACL) return SAI_ACL_STAGE_EGRESS;
                break;
            case SAI_OBJECT_TYPE_ROUTER_INTERFACE:
                if (id == SAI_ROUTER_INTERFACE_ATTR_INGRESS_ACL) return SAI_ACL_STAGE_INGRESS;
                if (id == SAI_ROUTER_INTERFACE_ATTR_EGRESS_ACL) return SAI_ACL_STAGE_EGRESS;
                break;
            case SAI_OBJECT_TYPE_SWITCH:
                if (id == SAI_SWITCH_ATTR_INGRESS_ACL) return SAI_ACL_STAGE_INGRESS;
                if (id == SAI_SWITCH_ATTR_EGRESS_ACL) return SAI_ACL_STAGE_EGRESS;
                if (id == SAI_SWITCH_ATTR_PRE_INGRESS_ACL) return SAI_ACL_STAGE_PRE_INGRESS;
                break;
            default:
                break;
        }

        return -1;
    }

    /* SAI_ACL_TABLE_ATTR_FIELD_* (bool) -> HWBCM_ACL_Q_* for one stage.
     * Returns false for a field the hardware cannot match. */
    bool tableFieldQual(sai_attr_id_t id, int hwStage, uint32_t &q)
    {
        SWSS_LOG_ENTER();

        switch (id)
        {
            case SAI_ACL_TABLE_ATTR_FIELD_SRC_IP:           q = HWBCM_ACL_Q_SRC_IP; return true;
            case SAI_ACL_TABLE_ATTR_FIELD_DST_IP:           q = HWBCM_ACL_Q_DST_IP; return true;
            case SAI_ACL_TABLE_ATTR_FIELD_SRC_IPV6:         q = HWBCM_ACL_Q_SRC_IP6; return true;
            case SAI_ACL_TABLE_ATTR_FIELD_DST_IPV6:         q = HWBCM_ACL_Q_DST_IP6; return true;
            case SAI_ACL_TABLE_ATTR_FIELD_SRC_MAC:          q = HWBCM_ACL_Q_SRC_MAC; return true;
            case SAI_ACL_TABLE_ATTR_FIELD_DST_MAC:          q = HWBCM_ACL_Q_DST_MAC; return true;
            case SAI_ACL_TABLE_ATTR_FIELD_IN_PORTS:
                q = (hwStage == HWBCM_ACL_STAGE_INGRESS) ? HWBCM_ACL_Q_IN_PORTS : HWBCM_ACL_Q_IN_PORT;
                return true;
            case SAI_ACL_TABLE_ATTR_FIELD_IN_PORT:
                q = (hwStage == HWBCM_ACL_STAGE_INGRESS) ? HWBCM_ACL_Q_IN_PORTS : HWBCM_ACL_Q_IN_PORT;
                return true;
            case SAI_ACL_TABLE_ATTR_FIELD_OUT_PORTS:
            case SAI_ACL_TABLE_ATTR_FIELD_OUT_PORT:         q = HWBCM_ACL_Q_OUT_PORT; return true;
            case SAI_ACL_TABLE_ATTR_FIELD_OUTER_VLAN_ID:    q = HWBCM_ACL_Q_VLAN; return true;
            case SAI_ACL_TABLE_ATTR_FIELD_L4_SRC_PORT:      q = HWBCM_ACL_Q_L4_SRC; return true;
            case SAI_ACL_TABLE_ATTR_FIELD_L4_DST_PORT:      q = HWBCM_ACL_Q_L4_DST; return true;
            case SAI_ACL_TABLE_ATTR_FIELD_ETHER_TYPE:       q = HWBCM_ACL_Q_ETHER_TYPE; return true;
            case SAI_ACL_TABLE_ATTR_FIELD_IP_PROTOCOL:
            case SAI_ACL_TABLE_ATTR_FIELD_IPV6_NEXT_HEADER: q = HWBCM_ACL_Q_IP_PROTO; return true;
            case SAI_ACL_TABLE_ATTR_FIELD_DSCP:
            case SAI_ACL_TABLE_ATTR_FIELD_ECN:
            case SAI_ACL_TABLE_ATTR_FIELD_TOS:              q = HWBCM_ACL_Q_TOS; return true;
            case SAI_ACL_TABLE_ATTR_FIELD_TTL:              q = HWBCM_ACL_Q_TTL; return true;
            case SAI_ACL_TABLE_ATTR_FIELD_TCP_FLAGS:        q = HWBCM_ACL_Q_TCP_FLAGS; return true;
            case SAI_ACL_TABLE_ATTR_FIELD_ACL_IP_TYPE:      q = HWBCM_ACL_Q_IP_TYPE; return true;
            case SAI_ACL_TABLE_ATTR_FIELD_ACL_IP_FRAG:      q = HWBCM_ACL_Q_IP_FRAG; return true;
            case SAI_ACL_TABLE_ATTR_FIELD_TC:               q = HWBCM_ACL_Q_TC; return true;
            case SAI_ACL_TABLE_ATTR_FIELD_ICMP_TYPE:
            case SAI_ACL_TABLE_ATTR_FIELD_ICMP_CODE:
            case SAI_ACL_TABLE_ATTR_FIELD_ICMPV6_TYPE:
            case SAI_ACL_TABLE_ATTR_FIELD_ICMPV6_CODE:      q = HWBCM_ACL_Q_ICMP; return true;
            default:
                q = 0;
                return false;
        }
    }

    int ipTypeOf(int32_t t)
    {
        SWSS_LOG_ENTER();

        switch (t)
        {
            case SAI_ACL_IP_TYPE_ANY:         return HWBCM_ACL_IPT_ANY;
            case SAI_ACL_IP_TYPE_IP:          return HWBCM_ACL_IPT_IP;
            case SAI_ACL_IP_TYPE_NON_IP:      return HWBCM_ACL_IPT_NON_IP;
            case SAI_ACL_IP_TYPE_IPV4ANY:     return HWBCM_ACL_IPT_IPV4;
            case SAI_ACL_IP_TYPE_NON_IPV4:    return HWBCM_ACL_IPT_NON_IPV4;
            case SAI_ACL_IP_TYPE_IPV6ANY:     return HWBCM_ACL_IPT_IPV6;
            case SAI_ACL_IP_TYPE_NON_IPV6:    return HWBCM_ACL_IPT_NON_IPV6;
            case SAI_ACL_IP_TYPE_ARP:         return HWBCM_ACL_IPT_ARP;
            case SAI_ACL_IP_TYPE_ARP_REQUEST: return HWBCM_ACL_IPT_ARP_REQ;
            case SAI_ACL_IP_TYPE_ARP_REPLY:   return HWBCM_ACL_IPT_ARP_REPLY;
            default:                          return -1;
        }
    }

    int ipFragOf(int32_t f)
    {
        SWSS_LOG_ENTER();

        switch (f)
        {
            case SAI_ACL_IP_FRAG_ANY:              return HWBCM_ACL_FRAG_ANY;
            case SAI_ACL_IP_FRAG_NON_FRAG:         return HWBCM_ACL_FRAG_NON;
            case SAI_ACL_IP_FRAG_NON_FRAG_OR_HEAD: return HWBCM_ACL_FRAG_NON_OR_HEAD;
            case SAI_ACL_IP_FRAG_HEAD:             return HWBCM_ACL_FRAG_HEAD;
            case SAI_ACL_IP_FRAG_NON_HEAD:         return HWBCM_ACL_FRAG_NON_HEAD;
            default:                               return -1;
        }
    }

    /* What one SAI ACL entry asks for, before binding expansion. */
    struct ParsedRule
    {
        hwbcm_acl_rule_t base;                     /* everything but port/VLAN scope */
        bool admin = true;
        int prio = 0;
        sai_object_id_t table = SAI_NULL_OBJECT_ID;
        sai_object_id_t counter = SAI_NULL_OBJECT_ID;

        bool hasInPorts = false;
        std::vector<int> inPorts;                  /* bcm ports */
        bool hasOutPorts = false;
        std::vector<int> outPorts;
        bool hasVlan = false;
        uint16_t vlan = 0;
        uint16_t vlanMask = 0;
    };

    void setPbmp(hwbcm_acl_rule_t &r, const std::vector<int> &ports)
    {
        SWSS_LOG_ENTER();

        memset(r.in_pbmp, 0, sizeof(r.in_pbmp));

        for (int p: ports)
        {
            hwbcm_acl_rule_inport_add(&r, p);
        }

        // an explicitly empty set still qualifies (and matches nothing)
        r.quals |= HWBCM_ACL_Q_IN_PORTS;
    }

    bool contains(const std::vector<int> &v, int x)
    {
        SWSS_LOG_ENTER();

        return std::find(v.begin(), v.end(), x) != v.end();
    }

    sai_status_t hwStatus(int rv)
    {
        SWSS_LOG_ENTER();

        return SwitchBCM56860HW::bcmRvToSai(rv);
    }
}

// ---------------------------------------------------------------------------
// capabilities
// ---------------------------------------------------------------------------

sai_status_t SwitchBCM56860HW::set_acl_capabilities()
{
    SWSS_LOG_ENTER();

    /* IFP (all of these are programmed by aclProgramEntry) */
    m_aclIngressActions = {
        SAI_ACL_ACTION_TYPE_PACKET_ACTION,
        SAI_ACL_ACTION_TYPE_REDIRECT,
        SAI_ACL_ACTION_TYPE_COUNTER,
        SAI_ACL_ACTION_TYPE_MIRROR_INGRESS,
        SAI_ACL_ACTION_TYPE_MIRROR_EGRESS,
        SAI_ACL_ACTION_TYPE_SET_TC,
        SAI_ACL_ACTION_TYPE_SET_PACKET_COLOR,
        SAI_ACL_ACTION_TYPE_SET_OUTER_VLAN_ID,
        SAI_ACL_ACTION_TYPE_SET_OUTER_VLAN_PRI,
        SAI_ACL_ACTION_TYPE_SET_DSCP,
        SAI_ACL_ACTION_TYPE_SET_ECN,
    };

    /* EFP: drop / rewrite / egress mirror only (sdk trx/field.c:10446) */
    m_aclEgressActions = {
        SAI_ACL_ACTION_TYPE_PACKET_ACTION,
        SAI_ACL_ACTION_TYPE_COUNTER,
        SAI_ACL_ACTION_TYPE_MIRROR_EGRESS,
        SAI_ACL_ACTION_TYPE_SET_OUTER_VLAN_ID,
        SAI_ACL_ACTION_TYPE_SET_OUTER_VLAN_PRI,
        SAI_ACL_ACTION_TYPE_SET_DSCP,
        SAI_ACL_ACTION_TYPE_SET_ECN,
    };

    sai_attribute_t attr;

    attr.id = SAI_SWITCH_ATTR_MAX_ACL_ACTION_COUNT;
    attr.value.u32 = static_cast<uint32_t>(std::max(m_aclIngressActions.size(), m_aclEgressActions.size()));

    CHECK_STATUS(set(SAI_OBJECT_TYPE_SWITCH, m_switch_id, &attr));

    attr.id = SAI_SWITCH_ATTR_ACL_STAGE_INGRESS;
    attr.value.aclcapability.is_action_list_mandatory = false;
    attr.value.aclcapability.action_list.list = m_aclIngressActions.data();
    attr.value.aclcapability.action_list.count = static_cast<uint32_t>(m_aclIngressActions.size());

    CHECK_STATUS(set(SAI_OBJECT_TYPE_SWITCH, m_switch_id, &attr));

    attr.id = SAI_SWITCH_ATTR_ACL_STAGE_EGRESS;
    attr.value.aclcapability.is_action_list_mandatory = false;
    attr.value.aclcapability.action_list.list = m_aclEgressActions.data();
    attr.value.aclcapability.action_list.count = static_cast<uint32_t>(m_aclEgressActions.size());

    return set(SAI_OBJECT_TYPE_SWITCH, m_switch_id, &attr);
}

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

bool SwitchBCM56860HW::aclStoredAttrs(
        _In_ sai_object_type_t ot,
        _In_ sai_object_id_t oid,
        _Out_ std::vector<sai_attribute_t> &attrs)
{
    SWSS_LOG_ENTER();

    attrs.clear();

    auto oit = m_objectHash.find(ot);

    if (oit == m_objectHash.end())
    {
        return false;
    }

    auto it = oit->second.find(sai_serialize_object_id(oid));

    if (it == oit->second.end())
    {
        return false;
    }

    for (auto &kv: it->second)
    {
        if (kv.second && kv.second->getAttr())
        {
            // shallow copy: list pointers stay owned by the SaiAttrWrap
            attrs.push_back(*kv.second->getAttr());
        }
    }

    return true;
}

std::set<sai_object_id_t> SwitchBCM56860HW::aclTablesOf(
        _In_ sai_object_id_t acl) const
{
    SWSS_LOG_ENTER();

    std::set<sai_object_id_t> out;

    if (acl == SAI_NULL_OBJECT_ID)
    {
        return out;
    }

    if (m_aclTables.find(acl) != m_aclTables.end())
    {
        out.insert(acl);
        return out;
    }

    auto git = m_aclGroups.find(acl);

    if (git != m_aclGroups.end())
    {
        for (auto mid: git->second.members)
        {
            auto mit = m_aclGroupMembers.find(mid);

            if (mit != m_aclGroupMembers.end())
            {
                out.insert(mit->second.second);
            }
        }
    }

    return out;
}

void SwitchBCM56860HW::aclPortsOf(
        _In_ sai_object_id_t port_or_lag,
        _Inout_ std::vector<int> &out) const
{
    SWSS_LOG_ENTER();

    int bcm = bcmPortOf(port_or_lag);

    if (bcm >= 0)
    {
        out.push_back(bcm);
        return;
    }

    auto lit = m_lagMembers.find(port_or_lag);

    if (lit != m_lagMembers.end())
    {
        for (auto member: lit->second)
        {
            int p = bcmPortOf(member);

            if (p >= 0)
            {
                out.push_back(p);
            }
        }
    }
}

SwitchBCM56860HW::AclScope SwitchBCM56860HW::aclScopeOf(
        _In_ sai_object_id_t table_id)
{
    SWSS_LOG_ENTER();

    AclScope scope;

    auto tit = m_aclTables.find(table_id);

    if (tit == m_aclTables.end())
    {
        return scope;
    }

    for (auto &kv: m_aclBindings)
    {
        if (kv.first.second != tit->second.saiStage)
        {
            continue;
        }

        auto tables = aclTablesOf(kv.second.acl);

        if (tables.find(table_id) == tables.end())
        {
            continue;
        }

        sai_object_id_t obj = kv.first.first;

        switch (kv.second.ot)
        {
            case SAI_OBJECT_TYPE_SWITCH:
                scope.all = true;
                break;

            case SAI_OBJECT_TYPE_PORT:
            case SAI_OBJECT_TYPE_LAG:
                {
                    std::vector<int> ports;
                    aclPortsOf(obj, ports);
                    scope.ports.insert(ports.begin(), ports.end());
                }
                break;

            case SAI_OBJECT_TYPE_VLAN:
                {
                    int vid = vidOf(obj);

                    if (vid > 0)
                    {
                        scope.vlans.insert(vid);
                    }
                }
                break;

            case SAI_OBJECT_TYPE_ROUTER_INTERFACE:
                {
                    auto rit = m_rifToVid.find(obj);

                    if (rit != m_rifToVid.end() && rit->second > 0)
                    {
                        scope.vlans.insert(rit->second);
                    }
                    else
                    {
                        SWSS_LOG_WARN("ACL bound to RIF %s without a known VLAN: no hardware effect",
                                sai_serialize_object_id(obj).c_str());
                    }
                }
                break;

            default:
                break;
        }
    }

    return scope;
}

void SwitchBCM56860HW::aclRetire(
        _In_ sai_object_id_t counter_id,
        _In_ const AclHwEntry &h)
{
    SWSS_LOG_ENTER();

    if (h.stat >= 0)
    {
        uint64_t pkts = 0, bytes = 0;

        if (hwbcm_acl_stat_get(h.stat, &pkts, &bytes) == 0)
        {
            auto cit = m_aclCounters.find(counter_id);

            if (cit != m_aclCounters.end())
            {
                cit->second.pktOffset += pkts;
                cit->second.byteOffset += bytes;
            }
        }
    }

    int rv = hwbcm_acl_entry_delete(h.eid, h.stat);

    if (rv != 0)
    {
        SWSS_LOG_ERROR("hwbcm_acl_entry_delete(%d, stat %d) failed: %d", h.eid, h.stat, rv);
    }
}

// ---------------------------------------------------------------------------
// entry programming
// ---------------------------------------------------------------------------

sai_status_t SwitchBCM56860HW::aclProgramEntry(
        _In_ sai_object_id_t entry_id)
{
    SWSS_LOG_ENTER();

    auto eit = m_aclEntries.find(entry_id);

    if (eit == m_aclEntries.end())
    {
        return SAI_STATUS_ITEM_NOT_FOUND;
    }

    AclEntryHw &E = eit->second;

    std::string sid = sai_serialize_object_id(entry_id);

    std::vector<sai_attribute_t> attrs;

    if (!aclStoredAttrs(SAI_OBJECT_TYPE_ACL_ENTRY, entry_id, attrs))
    {
        SWSS_LOG_ERROR("ACL entry %s not in the object store", sid.c_str());
        return SAI_STATUS_ITEM_NOT_FOUND;
    }

    auto tit = m_aclTables.find(E.table);

    if (tit == m_aclTables.end())
    {
        SWSS_LOG_ERROR("ACL entry %s: table %s has no hardware group", sid.c_str(),
                sai_serialize_object_id(E.table).c_str());
        return SAI_STATUS_FAILURE;
    }

    const AclTableHw &T = tit->second;

    // --- parse the stored attributes -------------------------------------

    ParsedRule pr;
    hwbcm_acl_rule_init(&pr.base);
    hwbcm_acl_rule_t &r = pr.base;

    for (const auto &a: attrs)
    {
        const sai_attr_id_t id = a.id;

        if (id == SAI_ACL_ENTRY_ATTR_TABLE_ID)    { pr.table = a.value.oid; continue; }
        if (id == SAI_ACL_ENTRY_ATTR_PRIORITY)    { pr.prio = static_cast<int>(a.value.u32); continue; }
        if (id == SAI_ACL_ENTRY_ATTR_ADMIN_STATE) { pr.admin = a.value.booldata; continue; }

        if (id >= SAI_ACL_ENTRY_ATTR_FIELD_START && id <= SAI_ACL_ENTRY_ATTR_FIELD_END)
        {
            const sai_acl_field_data_t &f = a.value.aclfield;

            if (!f.enable)
            {
                continue;
            }

            switch (id)
            {
                case SAI_ACL_ENTRY_ATTR_FIELD_SRC_IP:
                    r.quals |= HWBCM_ACL_Q_SRC_IP;
                    r.src_ip = ntohl(f.data.ip4);
                    r.src_ip_mask = ntohl(f.mask.ip4);
                    break;

                case SAI_ACL_ENTRY_ATTR_FIELD_DST_IP:
                    r.quals |= HWBCM_ACL_Q_DST_IP;
                    r.dst_ip = ntohl(f.data.ip4);
                    r.dst_ip_mask = ntohl(f.mask.ip4);
                    break;

                case SAI_ACL_ENTRY_ATTR_FIELD_SRC_IPV6:
                    r.quals |= HWBCM_ACL_Q_SRC_IP6;
                    memcpy(r.src_ip6, f.data.ip6, 16);
                    memcpy(r.src_ip6_mask, f.mask.ip6, 16);
                    break;

                case SAI_ACL_ENTRY_ATTR_FIELD_DST_IPV6:
                    r.quals |= HWBCM_ACL_Q_DST_IP6;
                    memcpy(r.dst_ip6, f.data.ip6, 16);
                    memcpy(r.dst_ip6_mask, f.mask.ip6, 16);
                    break;

                case SAI_ACL_ENTRY_ATTR_FIELD_SRC_MAC:
                    r.quals |= HWBCM_ACL_Q_SRC_MAC;
                    memcpy(r.src_mac, f.data.mac, 6);
                    memcpy(r.src_mac_mask, f.mask.mac, 6);
                    break;

                case SAI_ACL_ENTRY_ATTR_FIELD_DST_MAC:
                    r.quals |= HWBCM_ACL_Q_DST_MAC;
                    memcpy(r.dst_mac, f.data.mac, 6);
                    memcpy(r.dst_mac_mask, f.mask.mac, 6);
                    break;

                case SAI_ACL_ENTRY_ATTR_FIELD_IN_PORTS:
                    if (f.data.objlist.count > 0 && f.data.objlist.list == nullptr)
                    {
                        return SAI_STATUS_INVALID_PARAMETER;
                    }
                    pr.hasInPorts = true;
                    for (uint32_t i = 0; i < f.data.objlist.count; i++)
                    {
                        aclPortsOf(f.data.objlist.list[i], pr.inPorts);
                    }
                    break;

                case SAI_ACL_ENTRY_ATTR_FIELD_IN_PORT:
                    pr.hasInPorts = true;
                    aclPortsOf(f.data.oid, pr.inPorts);
                    break;

                case SAI_ACL_ENTRY_ATTR_FIELD_OUT_PORTS:
                    if (f.data.objlist.count > 0 && f.data.objlist.list == nullptr)
                    {
                        return SAI_STATUS_INVALID_PARAMETER;
                    }
                    pr.hasOutPorts = true;
                    for (uint32_t i = 0; i < f.data.objlist.count; i++)
                    {
                        aclPortsOf(f.data.objlist.list[i], pr.outPorts);
                    }
                    break;

                case SAI_ACL_ENTRY_ATTR_FIELD_OUT_PORT:
                    pr.hasOutPorts = true;
                    aclPortsOf(f.data.oid, pr.outPorts);
                    break;

                case SAI_ACL_ENTRY_ATTR_FIELD_OUTER_VLAN_ID:
                    pr.hasVlan = true;
                    pr.vlan = static_cast<uint16_t>(f.data.u16 & 0x0fff);
                    pr.vlanMask = static_cast<uint16_t>(f.mask.u16 & 0x0fff);
                    break;

                case SAI_ACL_ENTRY_ATTR_FIELD_L4_SRC_PORT:
                    r.quals |= HWBCM_ACL_Q_L4_SRC;
                    r.l4_src = f.data.u16;
                    r.l4_src_mask = f.mask.u16;
                    break;

                case SAI_ACL_ENTRY_ATTR_FIELD_L4_DST_PORT:
                    r.quals |= HWBCM_ACL_Q_L4_DST;
                    r.l4_dst = f.data.u16;
                    r.l4_dst_mask = f.mask.u16;
                    break;

                case SAI_ACL_ENTRY_ATTR_FIELD_ETHER_TYPE:
                    r.quals |= HWBCM_ACL_Q_ETHER_TYPE;
                    r.ether_type = f.data.u16;
                    r.ether_type_mask = f.mask.u16;
                    break;

                case SAI_ACL_ENTRY_ATTR_FIELD_IP_PROTOCOL:
                case SAI_ACL_ENTRY_ATTR_FIELD_IPV6_NEXT_HEADER:
                    r.quals |= HWBCM_ACL_Q_IP_PROTO;
                    r.ip_proto = f.data.u8;
                    r.ip_proto_mask = f.mask.u8;
                    break;

                case SAI_ACL_ENTRY_ATTR_FIELD_DSCP:
                    r.quals |= HWBCM_ACL_Q_TOS;
                    r.tos = static_cast<uint8_t>((r.tos & 0x03) | ((f.data.u8 << 2) & 0xfc));
                    r.tos_mask = static_cast<uint8_t>((r.tos_mask & 0x03) | ((f.mask.u8 << 2) & 0xfc));
                    break;

                case SAI_ACL_ENTRY_ATTR_FIELD_ECN:
                    r.quals |= HWBCM_ACL_Q_TOS;
                    r.tos = static_cast<uint8_t>((r.tos & 0xfc) | (f.data.u8 & 0x03));
                    r.tos_mask = static_cast<uint8_t>((r.tos_mask & 0xfc) | (f.mask.u8 & 0x03));
                    break;

                case SAI_ACL_ENTRY_ATTR_FIELD_TOS:
                    r.quals |= HWBCM_ACL_Q_TOS;
                    r.tos = f.data.u8;
                    r.tos_mask = f.mask.u8;
                    break;

                case SAI_ACL_ENTRY_ATTR_FIELD_TTL:
                    r.quals |= HWBCM_ACL_Q_TTL;
                    r.ttl = f.data.u8;
                    r.ttl_mask = f.mask.u8;
                    break;

                case SAI_ACL_ENTRY_ATTR_FIELD_TCP_FLAGS:
                    if (f.mask.u8 & 0xc0)
                    {
                        /* TD2+ TcpControl is 6 bits (FIN..URG): ECE/CWR cannot
                         * be matched, and ignoring them would widen the rule */
                        SWSS_LOG_ERROR("ACL entry %s: TCP_FLAGS mask 0x%02x uses ECE/CWR, not matchable on TD2+",
                                sid.c_str(), f.mask.u8);
                        return SAI_STATUS_NOT_SUPPORTED;
                    }
                    r.quals |= HWBCM_ACL_Q_TCP_FLAGS;
                    r.tcp_flags = f.data.u8;
                    r.tcp_flags_mask = f.mask.u8;
                    break;

                case SAI_ACL_ENTRY_ATTR_FIELD_ICMP_TYPE:
                case SAI_ACL_ENTRY_ATTR_FIELD_ICMPV6_TYPE:
                    r.quals |= HWBCM_ACL_Q_ICMP;
                    r.icmp = static_cast<uint16_t>((r.icmp & 0x00ff) | (f.data.u8 << 8));
                    r.icmp_mask = static_cast<uint16_t>((r.icmp_mask & 0x00ff) | (f.mask.u8 << 8));
                    break;

                case SAI_ACL_ENTRY_ATTR_FIELD_ICMP_CODE:
                case SAI_ACL_ENTRY_ATTR_FIELD_ICMPV6_CODE:
                    r.quals |= HWBCM_ACL_Q_ICMP;
                    r.icmp = static_cast<uint16_t>((r.icmp & 0xff00) | f.data.u8);
                    r.icmp_mask = static_cast<uint16_t>((r.icmp_mask & 0xff00) | f.mask.u8);
                    break;

                case SAI_ACL_ENTRY_ATTR_FIELD_ACL_IP_TYPE:
                    {
                        int t = ipTypeOf(f.data.s32);

                        if (t < 0)
                        {
                            SWSS_LOG_ERROR("ACL entry %s: IP_TYPE %d unsupported", sid.c_str(), f.data.s32);
                            return SAI_STATUS_NOT_SUPPORTED;
                        }

                        if (t != HWBCM_ACL_IPT_ANY)
                        {
                            r.quals |= HWBCM_ACL_Q_IP_TYPE;
                            r.ip_type = t;
                        }
                    }
                    break;

                case SAI_ACL_ENTRY_ATTR_FIELD_ACL_IP_FRAG:
                    {
                        int fr = ipFragOf(f.data.s32);

                        if (fr < 0)
                        {
                            SWSS_LOG_ERROR("ACL entry %s: IP_FRAG %d unknown", sid.c_str(), f.data.s32);
                            return SAI_STATUS_INVALID_PARAMETER;
                        }
                        r.quals |= HWBCM_ACL_Q_IP_FRAG;
                        r.ip_frag = fr;
                    }
                    break;

                case SAI_ACL_ENTRY_ATTR_FIELD_TC:
                    r.quals |= HWBCM_ACL_Q_TC;
                    r.tc = f.data.u8;
                    r.tc_mask = f.mask.u8;
                    break;

                case SAI_ACL_ENTRY_ATTR_FIELD_ACL_RANGE_TYPE:
                    if (f.data.objlist.count > 0 && f.data.objlist.list == nullptr)
                    {
                        return SAI_STATUS_INVALID_PARAMETER;
                    }
                    r.quals |= HWBCM_ACL_Q_RANGE;
                    for (uint32_t i = 0; i < f.data.objlist.count; i++)
                    {
                        auto rit = m_aclRanges.find(f.data.objlist.list[i]);

                        if (rit == m_aclRanges.end() || r.nranges >= HWBCM_ACL_MAX_RANGES)
                        {
                            SWSS_LOG_ERROR("ACL entry %s: range %s unknown or too many ranges", sid.c_str(),
                                    sai_serialize_object_id(f.data.objlist.list[i]).c_str());
                            return SAI_STATUS_INVALID_PARAMETER;
                        }
                        r.ranges[r.nranges++] = rit->second;
                    }
                    break;

                default:
                    {
                        auto meta = sai_metadata_get_attr_metadata(SAI_OBJECT_TYPE_ACL_ENTRY, id);

                        /* refusing beats silently widening the match */
                        SWSS_LOG_ERROR("ACL entry %s: match field %s not supported in hardware", sid.c_str(),
                                meta ? meta->attridname : "?");
                        return SAI_STATUS_NOT_SUPPORTED;
                    }
            }

            continue;
        }

        if (id >= SAI_ACL_ENTRY_ATTR_ACTION_START && id <= SAI_ACL_ENTRY_ATTR_ACTION_END)
        {
            const sai_acl_action_data_t &act = a.value.aclaction;

            if (!act.enable)
            {
                continue;
            }

            switch (id)
            {
                case SAI_ACL_ENTRY_ATTR_ACTION_PACKET_ACTION:
                    switch (act.parameter.s32)
                    {
                        case SAI_PACKET_ACTION_DROP:
                        case SAI_PACKET_ACTION_DENY:        r.acts |= HWBCM_ACL_A_DROP; break;
                        case SAI_PACKET_ACTION_TRAP:        r.acts |= HWBCM_ACL_A_DROP | HWBCM_ACL_A_COPY_CPU; break;
                        case SAI_PACKET_ACTION_COPY:
                        case SAI_PACKET_ACTION_LOG:         r.acts |= HWBCM_ACL_A_COPY_CPU; break;
                        case SAI_PACKET_ACTION_COPY_CANCEL: r.acts |= HWBCM_ACL_A_COPY_CPU_CANCEL; break;
                        case SAI_PACKET_ACTION_DONOTDROP:   r.acts |= HWBCM_ACL_A_DROP_CANCEL; break;
                        case SAI_PACKET_ACTION_FORWARD:
                        case SAI_PACKET_ACTION_TRANSIT:
                        default:
                            /* "forward": the entry exists (and wins over lower
                             * priority entries of the same table), no action */
                            break;
                    }
                    break;

                case SAI_ACL_ENTRY_ATTR_ACTION_REDIRECT:
                    {
                        sai_object_id_t target = act.parameter.oid;

                        if (m_bridgePortToVp.find(target) == m_bridgePortToVp.end())
                        {
                            sai_object_id_t bpt = bridgePortTarget(target);

                            if (bpt != SAI_NULL_OBJECT_ID)
                            {
                                target = bpt;
                            }
                        }

                        int port = bcmPortOf(target);
                        int trunk = trunkOf(target);
                        auto nit = m_nhToEgr.find(target);
                        auto git = m_nhgToEcmp.find(target);

                        if (port >= 0)
                        {
                            r.acts |= HWBCM_ACL_A_REDIRECT_PORT;
                            r.redirect = port;
                        }
                        else if (trunk >= 0)
                        {
                            r.acts |= HWBCM_ACL_A_REDIRECT_TRUNK;
                            r.redirect = trunk;
                        }
                        else if (nit != m_nhToEgr.end())
                        {
                            r.acts |= HWBCM_ACL_A_REDIRECT_L3;
                            r.redirect = nit->second;
                        }
                        else if (git != m_nhgToEcmp.end())
                        {
                            r.acts |= HWBCM_ACL_A_REDIRECT_L3;
                            r.redirect = git->second;
                        }
                        else
                        {
                            SWSS_LOG_ERROR("ACL entry %s: redirect target %s has no hardware object", sid.c_str(),
                                    sai_serialize_object_id(act.parameter.oid).c_str());
                            return SAI_STATUS_INVALID_PARAMETER;
                        }
                    }
                    break;

                case SAI_ACL_ENTRY_ATTR_ACTION_COUNTER:
                    pr.counter = act.parameter.oid;
                    break;

                case SAI_ACL_ENTRY_ATTR_ACTION_MIRROR_INGRESS:
                case SAI_ACL_ENTRY_ATTR_ACTION_MIRROR_EGRESS:
                    {
                        bool ing = (id == SAI_ACL_ENTRY_ATTR_ACTION_MIRROR_INGRESS);

                        if (act.parameter.objlist.count > 0 && act.parameter.objlist.list == nullptr)
                        {
                            return SAI_STATUS_INVALID_PARAMETER;
                        }

                        for (uint32_t i = 0; i < act.parameter.objlist.count; i++)
                        {
                            sai_object_id_t session = act.parameter.objlist.list[i];

                            auto mit = m_aclMirrorDest.find(session);

                            if (mit == m_aclMirrorDest.end() || mit->second == -1)
                            {
                                /* installed without the mirror; hwAclMirrorSessionUpdated
                                 * reprograms the entry when the session gets a destination */
                                SWSS_LOG_WARN("ACL entry %s: mirror session %s has no hardware destination yet",
                                        sid.c_str(), sai_serialize_object_id(session).c_str());
                                continue;
                            }

                            if (ing && r.n_mirror_ing < HWBCM_ACL_MAX_MIRRORS)
                            {
                                r.acts |= HWBCM_ACL_A_MIRROR_ING;
                                r.mirror_ing[r.n_mirror_ing++] = mit->second;
                            }
                            else if (!ing && r.n_mirror_egr < HWBCM_ACL_MAX_MIRRORS)
                            {
                                r.acts |= HWBCM_ACL_A_MIRROR_EGR;
                                r.mirror_egr[r.n_mirror_egr++] = mit->second;
                            }
                        }
                    }
                    break;

                case SAI_ACL_ENTRY_ATTR_ACTION_SET_TC:
                    r.acts |= HWBCM_ACL_A_SET_TC;
                    r.set_tc = act.parameter.u8;
                    break;

                case SAI_ACL_ENTRY_ATTR_ACTION_SET_DSCP:
                    r.acts |= HWBCM_ACL_A_SET_DSCP;
                    r.set_dscp = act.parameter.u8;
                    break;

                case SAI_ACL_ENTRY_ATTR_ACTION_SET_ECN:
                    r.acts |= HWBCM_ACL_A_SET_ECN;
                    r.set_ecn = act.parameter.u8;
                    break;

                case SAI_ACL_ENTRY_ATTR_ACTION_SET_PACKET_COLOR:
                    r.acts |= HWBCM_ACL_A_SET_COLOR;
                    r.set_color = (act.parameter.s32 == SAI_PACKET_COLOR_RED) ? HWBCM_ACL_COLOR_RED :
                                  (act.parameter.s32 == SAI_PACKET_COLOR_YELLOW) ? HWBCM_ACL_COLOR_YELLOW :
                                  HWBCM_ACL_COLOR_GREEN;
                    break;

                case SAI_ACL_ENTRY_ATTR_ACTION_SET_OUTER_VLAN_ID:
                    r.acts |= HWBCM_ACL_A_SET_OUTER_VLAN;
                    r.set_outer_vlan = static_cast<uint16_t>(act.parameter.u16 & 0x0fff);
                    break;

                case SAI_ACL_ENTRY_ATTR_ACTION_SET_OUTER_VLAN_PRI:
                    r.acts |= HWBCM_ACL_A_SET_OUTER_PRI;
                    r.set_outer_pri = static_cast<uint8_t>(act.parameter.u8 & 0x07);
                    break;

                default:
                    {
                        auto meta = sai_metadata_get_attr_metadata(SAI_OBJECT_TYPE_ACL_ENTRY, id);

                        SWSS_LOG_WARN("ACL entry %s: action %s not programmed to hardware (ignored)", sid.c_str(),
                                meta ? meta->attridname : "?");
                    }
                    break;
            }
        }
    }

    // --- expand over the binding scopes -----------------------------------

    std::vector<hwbcm_acl_rule_t> rules;

    if (pr.admin)
    {
        AclScope scope = aclScopeOf(E.table);

        auto withVlan = [&](hwbcm_acl_rule_t v, int vid) -> bool
        {
            if (vid >= 0)
            {
                if (pr.hasVlan && ((static_cast<uint16_t>(vid) ^ pr.vlan) & pr.vlanMask) != 0)
                {
                    return false;        /* rule VLAN and bound VLAN can never both match */
                }
                v.quals |= HWBCM_ACL_Q_VLAN;
                v.vlan = static_cast<uint16_t>(vid);
                v.vlan_mask = 0x0fff;
            }
            else if (pr.hasVlan)
            {
                v.quals |= HWBCM_ACL_Q_VLAN;
                v.vlan = pr.vlan;
                v.vlan_mask = pr.vlanMask;
            }
            rules.push_back(v);
            return true;
        };

        if (T.hwStage == HWBCM_ACL_STAGE_INGRESS)
        {
            /* IFP: InPorts bitmap, DstPort for out ports */
            std::vector<hwbcm_acl_rule_t> scoped;

            if (scope.all)
            {
                hwbcm_acl_rule_t v = r;

                if (pr.hasInPorts)
                {
                    setPbmp(v, pr.inPorts);
                }
                withVlan(v, -1);
            }
            else
            {
                if (!scope.ports.empty())
                {
                    std::vector<int> ports;

                    for (int p: scope.ports)
                    {
                        if (!pr.hasInPorts || contains(pr.inPorts, p))
                        {
                            ports.push_back(p);
                        }
                    }

                    if (!ports.empty())
                    {
                        hwbcm_acl_rule_t v = r;
                        setPbmp(v, ports);
                        withVlan(v, -1);
                    }
                }

                for (int vid: scope.vlans)
                {
                    hwbcm_acl_rule_t v = r;

                    if (pr.hasInPorts)
                    {
                        setPbmp(v, pr.inPorts);
                    }
                    withVlan(v, vid);
                }
            }

            if (pr.hasOutPorts)
            {
                scoped.swap(rules);

                for (auto &v: scoped)
                {
                    for (int op: pr.outPorts)
                    {
                        hwbcm_acl_rule_t w = v;
                        w.quals |= HWBCM_ACL_Q_OUT_PORT;
                        w.out_port = op;
                        rules.push_back(w);
                    }
                }
            }
        }
        else
        {
            /* EFP / VFP: single InPort / OutPort qualifiers */
            bool egress = (T.hwStage == HWBCM_ACL_STAGE_EGRESS);

            if (scope.all)
            {
                withVlan(r, -1);
            }
            else
            {
                for (int p: scope.ports)
                {
                    hwbcm_acl_rule_t v = r;

                    if (egress)
                    {
                        if (pr.hasOutPorts && !contains(pr.outPorts, p))
                        {
                            continue;
                        }
                        v.quals |= HWBCM_ACL_Q_OUT_PORT;
                        v.out_port = p;
                    }
                    else
                    {
                        if (pr.hasInPorts && !contains(pr.inPorts, p))
                        {
                            continue;
                        }
                        v.quals |= HWBCM_ACL_Q_IN_PORT;
                        v.in_port = p;
                    }
                    withVlan(v, -1);
                }

                for (int vid: scope.vlans)
                {
                    withVlan(r, vid);
                }
            }

            /* rule-level port lists the scope did not already pin */
            std::vector<hwbcm_acl_rule_t> scoped;

            if (egress && pr.hasOutPorts)
            {
                scoped.swap(rules);

                for (auto &v: scoped)
                {
                    if (v.quals & HWBCM_ACL_Q_OUT_PORT)
                    {
                        rules.push_back(v);
                        continue;
                    }
                    for (int op: pr.outPorts)
                    {
                        hwbcm_acl_rule_t w = v;
                        w.quals |= HWBCM_ACL_Q_OUT_PORT;
                        w.out_port = op;
                        rules.push_back(w);
                    }
                }
            }

            if (pr.hasInPorts)
            {
                scoped.clear();
                scoped.swap(rules);

                for (auto &v: scoped)
                {
                    if (v.quals & HWBCM_ACL_Q_IN_PORT)
                    {
                        rules.push_back(v);
                        continue;
                    }
                    for (int ip: pr.inPorts)
                    {
                        hwbcm_acl_rule_t w = v;
                        w.quals |= HWBCM_ACL_Q_IN_PORT;
                        w.in_port = ip;
                        rules.push_back(w);
                    }
                }
            }
        }

        if (rules.size() > ACL_MAX_VARIANTS)
        {
            SWSS_LOG_ERROR("ACL entry %s expands to %zu hardware entries (limit %zu)", sid.c_str(),
                    rules.size(), ACL_MAX_VARIANTS);
            return SAI_STATUS_INSUFFICIENT_RESOURCES;
        }
    }

    // --- push to hardware ------------------------------------------------

    bool withStat = (pr.counter != SAI_NULL_OBJECT_ID) &&
                    (m_aclCounters.find(pr.counter) != m_aclCounters.end());

    sai_object_id_t newCounter = withStat ? pr.counter : SAI_NULL_OBJECT_ID;

    sai_status_t status = SAI_STATUS_SUCCESS;

    std::vector<AclHwEntry> newHw;

    if (newCounter == E.counter)
    {
        /* same counter: install any extra variants first so TCAM full does not
         * leave a partial rule subset after mutating existing variants */
        size_t keep = std::min(E.hw.size(), rules.size());
        std::vector<AclHwEntry> addedHw;

        for (size_t i = keep; i < rules.size(); i++)
        {
            AclHwEntry h;
            int rv = hwbcm_acl_entry_add(T.gid, pr.prio, &rules[i], withStat ? 1 : 0, &h.eid, &h.stat);

            if (rv != 0)
            {
                SWSS_LOG_ERROR("ACL entry %s variant %zu: install failed: %d (rolling back added variants)",
                        sid.c_str(), i, rv);
                for (auto &ah: addedHw)
                {
                    aclRetire(E.counter, ah);
                }
                return hwStatus(rv);
            }
            addedHw.push_back(h);
        }

        for (size_t i = 0; i < keep; i++)
        {
            int rv = hwbcm_acl_entry_update(T.gid, E.hw[i].eid, pr.prio, &rules[i]);

            if (rv == 0)
            {
                newHw.push_back(E.hw[i]);
                continue;
            }

            /* hwbcm removed it from hardware; recreate it from scratch */
            aclRetire(E.counter, E.hw[i]);

            AclHwEntry h;
            rv = hwbcm_acl_entry_add(T.gid, pr.prio, &rules[i], withStat ? 1 : 0, &h.eid, &h.stat);

            if (rv != 0)
            {
                SWSS_LOG_ERROR("ACL entry %s variant %zu: reprogram failed: %d", sid.c_str(), i, rv);
                status = hwStatus(rv);
                continue;
            }
            newHw.push_back(h);
        }

        for (auto &ah: addedHw)
        {
            newHw.push_back(ah);
        }

        for (size_t i = keep; i < E.hw.size(); i++)
        {
            aclRetire(E.counter, E.hw[i]);
        }
    }
    else
    {
        /* counter changed: make before break, the old stats go to the old counter */
        for (size_t i = 0; i < rules.size(); i++)
        {
            AclHwEntry h;
            int rv = hwbcm_acl_entry_add(T.gid, pr.prio, &rules[i], withStat ? 1 : 0, &h.eid, &h.stat);

            if (rv != 0)
            {
                SWSS_LOG_ERROR("ACL entry %s variant %zu: install failed: %d", sid.c_str(), i, rv);

                for (auto &n: newHw)
                {
                    aclRetire(newCounter, n);
                }
                return hwStatus(rv);
            }
            newHw.push_back(h);
        }

        for (auto &o: E.hw)
        {
            aclRetire(E.counter, o);
        }

        auto ocit = m_aclCounters.find(E.counter);

        if (ocit != m_aclCounters.end())
        {
            ocit->second.entries.erase(entry_id);
        }

        auto ncit = m_aclCounters.find(newCounter);

        if (ncit != m_aclCounters.end())
        {
            ncit->second.entries.insert(entry_id);
        }

        E.counter = newCounter;
    }

    E.hw.swap(newHw);

    SWSS_LOG_INFO("ACL entry %s (%s, prio %d): %zu hardware entries%s", sid.c_str(), stageStr(T.saiStage),
            pr.prio, E.hw.size(), pr.admin ? "" : " (admin down)");

    return status;
}

sai_status_t SwitchBCM56860HW::aclRefreshTable(
        _In_ sai_object_id_t table_id)
{
    SWSS_LOG_ENTER();

    auto tit = m_aclTables.find(table_id);

    if (tit == m_aclTables.end())
    {
        return SAI_STATUS_SUCCESS;
    }

    sai_status_t status = SAI_STATUS_SUCCESS;

    // copy: aclProgramEntry does not touch the table's entry set, but be safe
    std::set<sai_object_id_t> entries = tit->second.entries;

    for (auto e: entries)
    {
        sai_status_t s = aclProgramEntry(e);

        if (s != SAI_STATUS_SUCCESS)
        {
            status = s;
        }
    }

    return status;
}

// ---------------------------------------------------------------------------
// ACL table
// ---------------------------------------------------------------------------

sai_status_t SwitchBCM56860HW::hwCreateAclTable(
        _In_ sai_object_id_t table_id,
        _In_ uint32_t attr_count,
        _In_ const sai_attribute_t *attr_list)
{
    SWSS_LOG_ENTER();

    std::string sid = sai_serialize_object_id(table_id);

    auto *stageAttr = findAttr(attr_count, attr_list, SAI_ACL_TABLE_ATTR_ACL_STAGE);

    if (stageAttr == nullptr)
    {
        SWSS_LOG_ERROR("ACL table %s: no ACL_STAGE", sid.c_str());
        return SAI_STATUS_MANDATORY_ATTRIBUTE_MISSING;
    }

    AclTableHw T;
    T.saiStage = stageAttr->value.s32;
    T.hwStage = hwStageOf(T.saiStage);

    if (T.hwStage < 0)
    {
        SWSS_LOG_ERROR("ACL table %s: stage %d not supported", sid.c_str(), T.saiStage);
        return SAI_STATUS_NOT_SUPPORTED;
    }

    // match fields

    for (uint32_t i = 0; i < attr_count; i++)
    {
        const sai_attribute_t &a = attr_list[i];

        if (a.id == SAI_ACL_TABLE_ATTR_FIELD_ACL_RANGE_TYPE)
        {
            if (a.value.s32list.count > 0)
            {
                T.quals |= HWBCM_ACL_Q_RANGE;
            }
            continue;
        }

        if (a.id < SAI_ACL_TABLE_ATTR_FIELD_START || a.id > SAI_ACL_TABLE_ATTR_FIELD_END)
        {
            continue;
        }

        auto meta = sai_metadata_get_attr_metadata(SAI_OBJECT_TYPE_ACL_TABLE, a.id);

        if (meta == nullptr || meta->attrvaluetype != SAI_ATTR_VALUE_TYPE_BOOL)
        {
            SWSS_LOG_ERROR("ACL table %s: field %s not supported in hardware", sid.c_str(),
                    meta ? meta->attridname : "?");
            return SAI_STATUS_NOT_SUPPORTED;
        }

        if (!a.value.booldata)
        {
            continue;
        }

        uint32_t q = 0;

        if (!tableFieldQual(a.id, T.hwStage, q))
        {
            SWSS_LOG_ERROR("ACL table %s: field %s not supported in hardware", sid.c_str(), meta->attridname);
            return SAI_STATUS_NOT_SUPPORTED;
        }

        if (q & (HWBCM_ACL_Q_IN_PORTS | HWBCM_ACL_Q_IN_PORT | HWBCM_ACL_Q_OUT_PORT))
        {
            T.matchesPorts = true;
        }

        T.quals |= q;
    }

    // qualifiers the bind points need

    auto *bpAttr = findAttr(attr_count, attr_list, SAI_ACL_TABLE_ATTR_ACL_BIND_POINT_TYPE_LIST);

    std::set<int32_t> bps;

    if (bpAttr != nullptr)
    {
        if (bpAttr->value.s32list.count > 0 && bpAttr->value.s32list.list == nullptr)
        {
            return SAI_STATUS_INVALID_PARAMETER;
        }
        for (uint32_t i = 0; i < bpAttr->value.s32list.count; i++)
        {
            bps.insert(bpAttr->value.s32list.list[i]);
        }
    }
    else
    {
        bps = { SAI_ACL_BIND_POINT_TYPE_PORT, SAI_ACL_BIND_POINT_TYPE_LAG };
    }

    bool portBind = bps.count(SAI_ACL_BIND_POINT_TYPE_PORT) || bps.count(SAI_ACL_BIND_POINT_TYPE_LAG);
    bool vlanBind = bps.count(SAI_ACL_BIND_POINT_TYPE_VLAN) || bps.count(SAI_ACL_BIND_POINT_TYPE_ROUTER_INTERFACE);

    switch (T.hwStage)
    {
        case HWBCM_ACL_STAGE_INGRESS:
            T.quals |= HWBCM_ACL_Q_IN_PORTS;    /* IPBM: free in every IFP key */
            break;
        case HWBCM_ACL_STAGE_EGRESS:
            if (portBind) T.quals |= HWBCM_ACL_Q_OUT_PORT;
            break;
        default:
            if (portBind) T.quals |= HWBCM_ACL_Q_IN_PORT;
            break;
    }

    if (vlanBind)
    {
        T.quals |= HWBCM_ACL_Q_VLAN;
    }

    uint32_t bad = T.quals & ~hwbcm_acl_stage_quals(T.hwStage);

    if (bad != 0)
    {
        SWSS_LOG_ERROR("ACL table %s: %s stage cannot match qualifier bits 0x%x", sid.c_str(),
                stageStr(T.saiStage), bad);
        return SAI_STATUS_NOT_SUPPORTED;
    }

    // unique group priority within the stage, 10..40

    auto &used = m_aclPrioUsed[T.hwStage];

    for (int p = ACL_GROUP_PRIO_MIN; p <= ACL_GROUP_PRIO_MAX; p++)
    {
        if (used.find(p) == used.end())
        {
            T.prio = p;
            break;
        }
    }

    if (T.prio < 0)
    {
        SWSS_LOG_ERROR("ACL table %s: all %s group priorities %d..%d in use", sid.c_str(),
                stageStr(T.saiStage), ACL_GROUP_PRIO_MIN, ACL_GROUP_PRIO_MAX);
        return SAI_STATUS_INSUFFICIENT_RESOURCES;
    }

    int rv = hwbcm_acl_group_create(T.hwStage, T.quals, T.prio, &T.gid);

    if (rv != 0)
    {
        SWSS_LOG_ERROR("ACL table %s: hwbcm_acl_group_create(%s, quals 0x%x, prio %d) failed: %d",
                sid.c_str(), stageStr(T.saiStage), T.quals, T.prio, rv);
        return hwStatus(rv);
    }

    used.insert(T.prio);

    SWSS_LOG_NOTICE("ACL table %s: %s group %d, quals 0x%x, priority %d", sid.c_str(),
            stageStr(T.saiStage), T.gid, T.quals, T.prio);

    m_aclTables[table_id] = T;

    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchBCM56860HW::hwRemoveAclTable(
        _In_ sai_object_id_t table_id)
{
    SWSS_LOG_ENTER();

    auto tit = m_aclTables.find(table_id);

    if (tit == m_aclTables.end())
    {
        return SAI_STATUS_SUCCESS;
    }

    // entries normally go first; clean up whatever is left
    for (auto e: tit->second.entries)
    {
        auto eit = m_aclEntries.find(e);

        if (eit == m_aclEntries.end())
        {
            continue;
        }

        for (auto &h: eit->second.hw)
        {
            aclRetire(eit->second.counter, h);
        }

        if (eit->second.counter != SAI_NULL_OBJECT_ID)
        {
            auto cit = m_aclCounters.find(eit->second.counter);
            if (cit != m_aclCounters.end())
            {
                cit->second.entries.erase(e);
            }
        }

        eit->second.hw.clear();
    }

    int rv = hwbcm_acl_group_destroy(tit->second.gid);

    if (rv != 0)
    {
        SWSS_LOG_ERROR("ACL table %s: hwbcm_acl_group_destroy(%d) failed: %d",
                sai_serialize_object_id(table_id).c_str(), tit->second.gid, rv);
        return bcmRvToSai(rv);
    }

    m_aclPrioUsed[tit->second.hwStage].erase(tit->second.prio);

    for (auto e: tit->second.entries)
    {
        m_aclEntries.erase(e);
    }

    m_aclTables.erase(tit);

    return SAI_STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// ACL entry
// ---------------------------------------------------------------------------

sai_status_t SwitchBCM56860HW::hwCreateAclEntry(
        _In_ sai_object_id_t entry_id,
        _In_ uint32_t attr_count,
        _In_ const sai_attribute_t *attr_list)
{
    SWSS_LOG_ENTER();

    auto *tableAttr = findAttr(attr_count, attr_list, SAI_ACL_ENTRY_ATTR_TABLE_ID);

    if (tableAttr == nullptr || m_aclTables.find(tableAttr->value.oid) == m_aclTables.end())
    {
        SWSS_LOG_ERROR("ACL entry %s: table missing or without hardware group",
                sai_serialize_object_id(entry_id).c_str());
        return SAI_STATUS_INVALID_PARAMETER;
    }

    AclEntryHw E;
    E.table = tableAttr->value.oid;

    m_aclEntries[entry_id] = E;
    m_aclTables[E.table].entries.insert(entry_id);

    sai_status_t status = aclProgramEntry(entry_id);

    if (status != SAI_STATUS_SUCCESS)
    {
        // the dispatcher rolls the store back; drop our side too
        auto eit = m_aclEntries.find(entry_id);

        if (eit != m_aclEntries.end())
        {
            for (auto &h: eit->second.hw)
            {
                aclRetire(eit->second.counter, h);
            }

            auto cit = m_aclCounters.find(eit->second.counter);

            if (cit != m_aclCounters.end())
            {
                cit->second.entries.erase(entry_id);
            }

            m_aclEntries.erase(eit);
        }

        m_aclTables[E.table].entries.erase(entry_id);
    }

    return status;
}

sai_status_t SwitchBCM56860HW::hwSetAclEntry(
        _In_ sai_object_id_t entry_id,
        _In_ const sai_attribute_t *attr)
{
    SWSS_LOG_ENTER();

    if (m_aclEntries.find(entry_id) == m_aclEntries.end())
    {
        return SAI_STATUS_SUCCESS;
    }

    // the store already has the new value: reprogram from the stored set
    return aclProgramEntry(entry_id);
}

sai_status_t SwitchBCM56860HW::hwRemoveAclEntry(
        _In_ sai_object_id_t entry_id)
{
    SWSS_LOG_ENTER();

    auto eit = m_aclEntries.find(entry_id);

    if (eit == m_aclEntries.end())
    {
        return SAI_STATUS_SUCCESS;
    }

    for (auto &h: eit->second.hw)
    {
        aclRetire(eit->second.counter, h);
    }

    auto cit = m_aclCounters.find(eit->second.counter);

    if (cit != m_aclCounters.end())
    {
        cit->second.entries.erase(entry_id);
    }

    auto tit = m_aclTables.find(eit->second.table);

    if (tit != m_aclTables.end())
    {
        tit->second.entries.erase(entry_id);
    }

    m_aclEntries.erase(eit);

    return SAI_STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// ACL counter
// ---------------------------------------------------------------------------

sai_status_t SwitchBCM56860HW::hwCreateAclCounter(
        _In_ sai_object_id_t counter_id,
        _In_ uint32_t attr_count,
        _In_ const sai_attribute_t *attr_list)
{
    SWSS_LOG_ENTER();

    AclCounterHw C;

    auto *tableAttr = findAttr(attr_count, attr_list, SAI_ACL_COUNTER_ATTR_TABLE_ID);

    if (tableAttr != nullptr)
    {
        C.table = tableAttr->value.oid;
    }

    // initial values given at create are the starting point
    auto *p = findAttr(attr_count, attr_list, SAI_ACL_COUNTER_ATTR_PACKETS);
    auto *b = findAttr(attr_count, attr_list, SAI_ACL_COUNTER_ATTR_BYTES);

    if (p != nullptr) C.pktOffset = p->value.u64;
    if (b != nullptr) C.byteOffset = b->value.u64;

    m_aclCounters[counter_id] = C;

    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchBCM56860HW::hwSetAclCounter(
        _In_ sai_object_id_t counter_id,
        _In_ const sai_attribute_t *attr)
{
    SWSS_LOG_ENTER();

    auto cit = m_aclCounters.find(counter_id);

    if (cit == m_aclCounters.end() || attr == nullptr)
    {
        return SAI_STATUS_SUCCESS;
    }

    if (attr->id != SAI_ACL_COUNTER_ATTR_PACKETS && attr->id != SAI_ACL_COUNTER_ATTR_BYTES)
    {
        return SAI_STATUS_SUCCESS;
    }

    /* value = offset + live; setting v (normally 0 = clear) rebases the offset
     * so no hardware write is needed (unsigned wrap-around is intended) */
    uint64_t livePkts = 0, liveBytes = 0;

    for (auto e: cit->second.entries)
    {
        auto eit = m_aclEntries.find(e);

        if (eit == m_aclEntries.end())
        {
            continue;
        }

        for (auto &h: eit->second.hw)
        {
            uint64_t p = 0, b = 0;

            if (h.stat >= 0 && hwbcm_acl_stat_get(h.stat, &p, &b) == 0)
            {
                livePkts += p;
                liveBytes += b;
            }
        }
    }

    if (attr->id == SAI_ACL_COUNTER_ATTR_PACKETS)
    {
        cit->second.pktOffset = attr->value.u64 - livePkts;
    }
    else
    {
        cit->second.byteOffset = attr->value.u64 - liveBytes;
    }

    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchBCM56860HW::hwRemoveAclCounter(
        _In_ sai_object_id_t counter_id)
{
    SWSS_LOG_ENTER();

    m_aclCounters.erase(counter_id);

    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchBCM56860HW::hwAclCounterGet(
        _In_ const std::string &serializedObjectId,
        _In_ uint32_t attr_count,
        _Inout_ sai_attribute_t *attr_list)
{
    SWSS_LOG_ENTER();

    if (!m_hwReady || attr_list == nullptr)
    {
        return SAI_STATUS_SUCCESS;
    }

    sai_object_id_t counter_id = SAI_NULL_OBJECT_ID;
    sai_deserialize_object_id(serializedObjectId, counter_id);

    auto cit = m_aclCounters.find(counter_id);

    if (cit == m_aclCounters.end())
    {
        return SAI_STATUS_SUCCESS;
    }

    bool wantP = false, wantB = false;

    for (uint32_t i = 0; i < attr_count; i++)
    {
        if (attr_list[i].id == SAI_ACL_COUNTER_ATTR_PACKETS) wantP = true;
        if (attr_list[i].id == SAI_ACL_COUNTER_ATTR_BYTES) wantB = true;
    }

    if (!wantP && !wantB)
    {
        return SAI_STATUS_SUCCESS;
    }

    uint64_t pkts = cit->second.pktOffset;
    uint64_t bytes = cit->second.byteOffset;

    for (auto e: cit->second.entries)
    {
        auto eit = m_aclEntries.find(e);

        if (eit == m_aclEntries.end())
        {
            continue;
        }

        for (auto &h: eit->second.hw)
        {
            uint64_t p = 0, b = 0;

            if (h.stat < 0)
            {
                continue;
            }

            int rv = hwbcm_acl_stat_get(h.stat, &p, &b);

            if (rv != 0)
            {
                SWSS_LOG_ERROR("hwbcm_acl_stat_get(%d) failed: %d", h.stat, rv);
                return bcmRvToSai(rv);
            }

            pkts += p;
            bytes += b;
        }
    }

    for (uint32_t i = 0; i < attr_count; i++)
    {
        if (attr_list[i].id == SAI_ACL_COUNTER_ATTR_PACKETS) attr_list[i].value.u64 = pkts;
        if (attr_list[i].id == SAI_ACL_COUNTER_ATTR_BYTES) attr_list[i].value.u64 = bytes;
    }

    return SAI_STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// ACL range
// ---------------------------------------------------------------------------

sai_status_t SwitchBCM56860HW::hwCreateAclRange(
        _In_ sai_object_id_t range_id,
        _In_ uint32_t attr_count,
        _In_ const sai_attribute_t *attr_list)
{
    SWSS_LOG_ENTER();

    std::string sid = sai_serialize_object_id(range_id);

    auto *typeAttr = findAttr(attr_count, attr_list, SAI_ACL_RANGE_ATTR_TYPE);
    auto *limAttr = findAttr(attr_count, attr_list, SAI_ACL_RANGE_ATTR_LIMIT);

    if (typeAttr == nullptr || limAttr == nullptr)
    {
        return SAI_STATUS_MANDATORY_ATTRIBUTE_MISSING;
    }

    int dst;

    switch (typeAttr->value.s32)
    {
        case SAI_ACL_RANGE_TYPE_L4_SRC_PORT_RANGE: dst = 0; break;
        case SAI_ACL_RANGE_TYPE_L4_DST_PORT_RANGE: dst = 1; break;
        default:
            SWSS_LOG_ERROR("ACL range %s: type %d not supported in hardware (L4 ports only)", sid.c_str(),
                    typeAttr->value.s32);
            return SAI_STATUS_NOT_SUPPORTED;
    }

    uint32_t mn = limAttr->value.u32range.min;
    uint32_t mx = limAttr->value.u32range.max;

    if (mn > 0xffff || mx > 0xffff || mn > mx)
    {
        return SAI_STATUS_INVALID_PARAMETER;
    }

    int rid = -1;
    int rv = hwbcm_acl_range_create(dst, static_cast<uint16_t>(mn), static_cast<uint16_t>(mx), &rid);

    if (rv != 0)
    {
        SWSS_LOG_ERROR("ACL range %s: hwbcm_acl_range_create(%s %u-%u) failed: %d", sid.c_str(),
                dst ? "dst" : "src", mn, mx, rv);
        return hwStatus(rv);
    }

    m_aclRanges[range_id] = rid;

    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchBCM56860HW::hwRemoveAclRange(
        _In_ sai_object_id_t range_id)
{
    SWSS_LOG_ENTER();

    auto it = m_aclRanges.find(range_id);

    if (it == m_aclRanges.end())
    {
        return SAI_STATUS_SUCCESS;
    }

    int rv = hwbcm_acl_range_destroy(it->second);

    if (rv != 0)
    {
        SWSS_LOG_ERROR("hwbcm_acl_range_destroy(%d) failed: %d", it->second, rv);
        return bcmRvToSai(rv);
    }

    m_aclRanges.erase(it);

    return SAI_STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// ACL table group (+ member)
// ---------------------------------------------------------------------------

sai_status_t SwitchBCM56860HW::hwCreateAclTableGroup(
        _In_ sai_object_id_t group_id,
        _In_ uint32_t attr_count,
        _In_ const sai_attribute_t *attr_list)
{
    SWSS_LOG_ENTER();

    AclGroupHw G;

    auto *stageAttr = findAttr(attr_count, attr_list, SAI_ACL_TABLE_GROUP_ATTR_ACL_STAGE);

    if (stageAttr != nullptr)
    {
        G.saiStage = stageAttr->value.s32;
    }

    /* SEQUENTIAL vs PARALLEL: TD2+ IFP groups always look up in parallel and
     * resolve conflicting actions by group priority, which is PARALLEL. */
    m_aclGroups[group_id] = G;

    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchBCM56860HW::hwRemoveAclTableGroup(
        _In_ sai_object_id_t group_id)
{
    SWSS_LOG_ENTER();

    m_aclGroups.erase(group_id);

    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchBCM56860HW::hwCreateAclTableGroupMember(
        _In_ sai_object_id_t member_id,
        _In_ uint32_t attr_count,
        _In_ const sai_attribute_t *attr_list)
{
    SWSS_LOG_ENTER();

    auto *gAttr = findAttr(attr_count, attr_list, SAI_ACL_TABLE_GROUP_MEMBER_ATTR_ACL_TABLE_GROUP_ID);
    auto *tAttr = findAttr(attr_count, attr_list, SAI_ACL_TABLE_GROUP_MEMBER_ATTR_ACL_TABLE_ID);

    if (gAttr == nullptr || tAttr == nullptr)
    {
        return SAI_STATUS_MANDATORY_ATTRIBUTE_MISSING;
    }

    auto git = m_aclGroups.find(gAttr->value.oid);

    if (git == m_aclGroups.end())
    {
        return SAI_STATUS_INVALID_PARAMETER;
    }

    auto tit = m_aclTables.find(tAttr->value.oid);

    if (tit != m_aclTables.end() && tit->second.saiStage != git->second.saiStage)
    {
        SWSS_LOG_ERROR("ACL group member %s: table stage %s != group stage %s",
                sai_serialize_object_id(member_id).c_str(), stageStr(tit->second.saiStage),
                stageStr(git->second.saiStage));
        return SAI_STATUS_INVALID_PARAMETER;
    }

    git->second.members.insert(member_id);
    m_aclGroupMembers[member_id] = std::make_pair(gAttr->value.oid, tAttr->value.oid);

    // the group may already be bound (portsorch binds before adding members)
    sai_status_t st = aclRefreshTable(tAttr->value.oid);

    if (st != SAI_STATUS_SUCCESS)
    {
        git->second.members.erase(member_id);
        m_aclGroupMembers.erase(member_id);
        (void)aclRefreshTable(tAttr->value.oid);
        return st;
    }

    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchBCM56860HW::hwRemoveAclTableGroupMember(
        _In_ sai_object_id_t member_id)
{
    SWSS_LOG_ENTER();

    auto mit = m_aclGroupMembers.find(member_id);

    if (mit == m_aclGroupMembers.end())
    {
        return SAI_STATUS_SUCCESS;
    }

    sai_object_id_t group = mit->second.first;
    sai_object_id_t table = mit->second.second;

    auto git = m_aclGroups.find(group);

    if (git != m_aclGroups.end())
    {
        git->second.members.erase(member_id);
    }

    m_aclGroupMembers.erase(mit);

    sai_status_t st = aclRefreshTable(table);

    if (st != SAI_STATUS_SUCCESS)
    {
        if (git != m_aclGroups.end())
        {
            git->second.members.insert(member_id);
        }
        m_aclGroupMembers[member_id] = std::make_pair(group, table);
        return st;
    }

    return SAI_STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// binding
// ---------------------------------------------------------------------------

bool SwitchBCM56860HW::hwAclIsBindAttr(
        _In_ sai_object_type_t ot,
        _In_ sai_attr_id_t id)
{
    SWSS_LOG_ENTER();

    return bindStageOf(ot, id) >= 0;
}

sai_status_t SwitchBCM56860HW::aclBindChanged(
        _In_ sai_object_id_t oldAcl,
        _In_ sai_object_id_t newAcl)
{
    SWSS_LOG_ENTER();

    std::set<sai_object_id_t> tables = aclTablesOf(oldAcl);
    std::set<sai_object_id_t> more = aclTablesOf(newAcl);

    tables.insert(more.begin(), more.end());

    sai_status_t status = SAI_STATUS_SUCCESS;

    for (auto t: tables)
    {
        sai_status_t s = aclRefreshTable(t);

        if (s != SAI_STATUS_SUCCESS)
        {
            status = s;
        }
    }

    return status;
}

sai_status_t SwitchBCM56860HW::hwAclBind(
        _In_ sai_object_type_t ot,
        _In_ sai_object_id_t oid,
        _In_ const sai_attribute_t *attr)
{
    SWSS_LOG_ENTER();

    if (attr == nullptr)
    {
        return SAI_STATUS_INVALID_PARAMETER;
    }

    int32_t stage = bindStageOf(ot, attr->id);

    if (stage < 0)
    {
        return SAI_STATUS_SUCCESS;
    }

    auto key = std::make_pair(oid, stage);

    sai_object_id_t oldAcl = SAI_NULL_OBJECT_ID;

    auto bit = m_aclBindings.find(key);

    if (bit != m_aclBindings.end())
    {
        oldAcl = bit->second.acl;
    }

    sai_object_id_t newAcl = attr->value.oid;

    if (oldAcl == newAcl)
    {
        return SAI_STATUS_SUCCESS;
    }

    if (newAcl == SAI_NULL_OBJECT_ID)
    {
        m_aclBindings.erase(key);
    }
    else
    {
        AclBinding b;
        b.ot = ot;
        b.acl = newAcl;
        m_aclBindings[key] = b;
    }

    SWSS_LOG_NOTICE("ACL %s bind %s %s: %s -> %s", stageStr(stage),
            sai_serialize_object_type(ot).c_str(), sai_serialize_object_id(oid).c_str(),
            sai_serialize_object_id(oldAcl).c_str(), sai_serialize_object_id(newAcl).c_str());

    sai_status_t st = aclBindChanged(oldAcl, newAcl);

    if (st != SAI_STATUS_SUCCESS)
    {
        if (oldAcl == SAI_NULL_OBJECT_ID)
        {
            m_aclBindings.erase(key);
        }
        else
        {
            AclBinding prev;
            prev.ot = ot;
            prev.acl = oldAcl;
            m_aclBindings[key] = prev;
        }
        return st;
    }

    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchBCM56860HW::hwAclBindOnCreate(
        _In_ sai_object_type_t ot,
        _In_ sai_object_id_t oid,
        _In_ uint32_t attr_count,
        _In_ const sai_attribute_t *attr_list)
{
    SWSS_LOG_ENTER();

    sai_status_t status = SAI_STATUS_SUCCESS;

    for (uint32_t i = 0; attr_list != nullptr && i < attr_count; i++)
    {
        if (hwAclIsBindAttr(ot, attr_list[i].id))
        {
            sai_status_t s = hwAclBind(ot, oid, &attr_list[i]);

            if (s != SAI_STATUS_SUCCESS)
            {
                status = s;
            }
        }
    }

    return status;
}

void SwitchBCM56860HW::hwAclObjectRemoved(
        _In_ sai_object_id_t oid)
{
    SWSS_LOG_ENTER();

    std::set<sai_object_id_t> tables;

    for (auto it = m_aclBindings.begin(); it != m_aclBindings.end(); )
    {
        if (it->first.first == oid)
        {
            auto t = aclTablesOf(it->second.acl);
            tables.insert(t.begin(), t.end());
            it = m_aclBindings.erase(it);
        }
        else
        {
            ++it;
        }
    }

    for (auto t: tables)
    {
        aclRefreshTable(t);
    }
}

void SwitchBCM56860HW::hwAclLagMembersChanged(
        _In_ sai_object_id_t lag_oid)
{
    SWSS_LOG_ENTER();

    std::set<sai_object_id_t> tables;

    for (auto &kv: m_aclBindings)
    {
        if (kv.first.first == lag_oid)
        {
            auto t = aclTablesOf(kv.second.acl);
            tables.insert(t.begin(), t.end());
        }
    }

    /* entries whose IN_PORTS / OUT_PORT(S) name the LAG expand to its members
     * too; they are found by rescanning every table that matches on ports */
    for (auto &kv: m_aclTables)
    {
        if (kv.second.matchesPorts)
        {
            tables.insert(kv.first);
        }
    }

    for (auto t: tables)
    {
        aclRefreshTable(t);
    }
}

void SwitchBCM56860HW::hwAclMirrorSessionUpdated(
        _In_ sai_object_id_t session_oid,
        _In_ int mirror_dest_gport)
{
    SWSS_LOG_ENTER();

    if (mirror_dest_gport == -1)
    {
        m_aclMirrorDest.erase(session_oid);
    }
    else
    {
        m_aclMirrorDest[session_oid] = mirror_dest_gport;
    }

    // reprogram every entry that names the session
    for (auto &kv: m_aclEntries)
    {
        bool uses = false;

        for (auto id: { SAI_ACL_ENTRY_ATTR_ACTION_MIRROR_INGRESS, SAI_ACL_ENTRY_ATTR_ACTION_MIRROR_EGRESS })
        {
            sai_attribute_t a;

            if (!getStoredAttr(SAI_OBJECT_TYPE_ACL_ENTRY, kv.first, id, a) || !a.value.aclaction.enable)
            {
                continue;
            }

            if (a.value.aclaction.parameter.objlist.list == nullptr)
            {
                continue;
            }

            for (uint32_t i = 0; i < a.value.aclaction.parameter.objlist.count; i++)
            {
                if (a.value.aclaction.parameter.objlist.list[i] == session_oid)
                {
                    uses = true;
                }
            }
        }

        if (uses)
        {
            aclProgramEntry(kv.first);
        }
    }
}
