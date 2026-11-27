/*
 * SwitchBCM56860HW_copp.cpp — control-plane policing (CoPP) on the Trident2+
 * hardware switch: SAI_OBJECT_TYPE_POLICER, SAI_OBJECT_TYPE_HOSTIF_TRAP_GROUP
 * (QUEUE, POLICER) and the policing side of SAI_OBJECT_TYPE_HOSTIF_TRAP
 * (TRAP_GROUP, TRAP_PRIORITY, PACKET_ACTION).
 *
 * Split of work:
 *  - hwCreateHostifTrap (core, A2) keeps steering the protocols to the CPU
 *    (switch controls, L2 user entries, my-station + L2TOCPU egress).
 *  - This file programs, per trap, IFP entries (hwbcm_copp.c) that pick the
 *    CPU queue of the trap group, attach the trap group's policer, and cancel
 *    the CPU copy for colours whose policer action is DROP.
 *
 * The object hash (SwitchStateBase) is the only source of truth for SAI
 * attributes: every apply re-reads the trap, its group and the group's
 * policer, so the order in which orchagent creates / sets them (copporch
 * creates the group, then the policer, then sets the group's POLICER, then
 * creates the traps) does not matter. Only the HW handles are kept here.
 *
 * Hardware programming errors are logged and propagated to the caller;
 * unsupported trap types with no TD2+ CoPP classifier remain bookkeeping-only.
 */

#include "SwitchBCM56860HW.h"

#include "swss/logger.h"
#include "meta/sai_serialize.h"

#include "hwbcm/hwbcm.h"
#include "hwbcm/hwbcm_copp.h"

#include <cinttypes>

using namespace saivs;

namespace
{
    /* sai_hostif_trap_type_t -> hwbcm_copp_trap_t, HWBCM_COPP_TRAP_NONE if
     * the trap cannot be classified on TD2+. */
    int coppTrapKind(
            _In_ int32_t type)
    {
        SWSS_LOG_ENTER();

        switch (type)
        {
            case SAI_HOSTIF_TRAP_TYPE_ARP_REQUEST:                 return HWBCM_COPP_TRAP_ARP_REQUEST;
            case SAI_HOSTIF_TRAP_TYPE_ARP_RESPONSE:                return HWBCM_COPP_TRAP_ARP_RESPONSE;
            case SAI_HOSTIF_TRAP_TYPE_IPV6_NEIGHBOR_DISCOVERY:     return HWBCM_COPP_TRAP_IPV6_ND;
            case SAI_HOSTIF_TRAP_TYPE_IPV6_NEIGHBOR_SOLICITATION:  return HWBCM_COPP_TRAP_IPV6_NS;
            case SAI_HOSTIF_TRAP_TYPE_IPV6_NEIGHBOR_ADVERTISEMENT: return HWBCM_COPP_TRAP_IPV6_NA;
            case SAI_HOSTIF_TRAP_TYPE_LLDP:                        return HWBCM_COPP_TRAP_LLDP;
            case SAI_HOSTIF_TRAP_TYPE_LACP:                        return HWBCM_COPP_TRAP_LACP;
            case SAI_HOSTIF_TRAP_TYPE_STP:                         return HWBCM_COPP_TRAP_STP;
            case SAI_HOSTIF_TRAP_TYPE_PVRST:                       return HWBCM_COPP_TRAP_PVRST;
            case SAI_HOSTIF_TRAP_TYPE_UDLD:                        return HWBCM_COPP_TRAP_UDLD;
            case SAI_HOSTIF_TRAP_TYPE_EAPOL:                       return HWBCM_COPP_TRAP_EAPOL;
            case SAI_HOSTIF_TRAP_TYPE_DHCP:
            case SAI_HOSTIF_TRAP_TYPE_DHCP_L2:                     return HWBCM_COPP_TRAP_DHCP;
            case SAI_HOSTIF_TRAP_TYPE_DHCPV6:
            case SAI_HOSTIF_TRAP_TYPE_DHCPV6_L2:                   return HWBCM_COPP_TRAP_DHCPV6;
            case SAI_HOSTIF_TRAP_TYPE_BGP:                         return HWBCM_COPP_TRAP_BGP;
            case SAI_HOSTIF_TRAP_TYPE_BGPV6:                       return HWBCM_COPP_TRAP_BGPV6;
            case SAI_HOSTIF_TRAP_TYPE_SSH:                         return HWBCM_COPP_TRAP_SSH;
            case SAI_HOSTIF_TRAP_TYPE_SNMP:                        return HWBCM_COPP_TRAP_SNMP;
            case SAI_HOSTIF_TRAP_TYPE_IP2ME:                       return HWBCM_COPP_TRAP_IP2ME;
            case SAI_HOSTIF_TRAP_TYPE_TTL_ERROR:                   return HWBCM_COPP_TRAP_TTL_ERROR;
            case SAI_HOSTIF_TRAP_TYPE_OSPF:                        return HWBCM_COPP_TRAP_OSPF;
            case SAI_HOSTIF_TRAP_TYPE_OSPFV6:                      return HWBCM_COPP_TRAP_OSPFV6;
            case SAI_HOSTIF_TRAP_TYPE_VRRP:                        return HWBCM_COPP_TRAP_VRRP;
            case SAI_HOSTIF_TRAP_TYPE_VRRPV6:                      return HWBCM_COPP_TRAP_VRRPV6;
            case SAI_HOSTIF_TRAP_TYPE_PIM:                         return HWBCM_COPP_TRAP_PIM;
            case SAI_HOSTIF_TRAP_TYPE_IGMP_TYPE_QUERY:
            case SAI_HOSTIF_TRAP_TYPE_IGMP_TYPE_LEAVE:
            case SAI_HOSTIF_TRAP_TYPE_IGMP_TYPE_V1_REPORT:
            case SAI_HOSTIF_TRAP_TYPE_IGMP_TYPE_V2_REPORT:
            case SAI_HOSTIF_TRAP_TYPE_IGMP_TYPE_V3_REPORT:         return HWBCM_COPP_TRAP_IGMP;
            case SAI_HOSTIF_TRAP_TYPE_BFD:                         return HWBCM_COPP_TRAP_BFD;
            case SAI_HOSTIF_TRAP_TYPE_BFDV6:                       return HWBCM_COPP_TRAP_BFDV6;
            case SAI_HOSTIF_TRAP_TYPE_LDP:                         return HWBCM_COPP_TRAP_LDP;

            default:
                return HWBCM_COPP_TRAP_NONE;
        }
    }

    bool isDrop(
            _In_ int32_t action)
    {
        SWSS_LOG_ENTER();

        return action == SAI_PACKET_ACTION_DROP || action == SAI_PACKET_ACTION_DENY;
    }
}

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

std::vector<sai_object_id_t> SwitchBCM56860HW::coppAllTraps()
{
    SWSS_LOG_ENTER();

    std::vector<sai_object_id_t> out;

    auto it = m_objectHash.find(SAI_OBJECT_TYPE_HOSTIF_TRAP);

    if (it == m_objectHash.end())
    {
        return out;
    }

    for (auto &kv: it->second)
    {
        sai_object_id_t oid = SAI_NULL_OBJECT_ID;

        sai_deserialize_object_id(kv.first, oid);

        out.push_back(oid);
    }

    return out;
}

sai_object_id_t SwitchBCM56860HW::coppTrapGroupOf(
        _In_ sai_object_id_t trap_id)
{
    SWSS_LOG_ENTER();

    sai_attribute_t a;

    if (getStoredAttr(SAI_OBJECT_TYPE_HOSTIF_TRAP, trap_id, SAI_HOSTIF_TRAP_ATTR_TRAP_GROUP, a) &&
            a.value.oid != SAI_NULL_OBJECT_ID)
    {
        return a.value.oid;
    }

    // SAI: unset TRAP_GROUP means the switch's default trap group
    if (getStoredAttr(SAI_OBJECT_TYPE_SWITCH, m_switch_id, SAI_SWITCH_ATTR_DEFAULT_TRAP_GROUP, a))
    {
        return a.value.oid;
    }

    return SAI_NULL_OBJECT_ID;
}

bool SwitchBCM56860HW::coppReadPolicer(
        _In_ sai_object_id_t policer_id,
        _Out_ CoppPolicerParams &p)
{
    SWSS_LOG_ENTER();

    p = CoppPolicerParams();

    sai_attribute_t a;

    if (getStoredAttr(SAI_OBJECT_TYPE_POLICER, policer_id, SAI_POLICER_ATTR_METER_TYPE, a))
    {
        switch (a.value.s32)
        {
            case SAI_METER_TYPE_PACKETS: p.packets = 1; break;
            case SAI_METER_TYPE_BYTES:   p.packets = 0; break;

            default:
                SWSS_LOG_WARN("policer %s: meter type %d not supported",
                        sai_serialize_object_id(policer_id).c_str(), a.value.s32);
                return false;
        }
    }

    if (getStoredAttr(SAI_OBJECT_TYPE_POLICER, policer_id, SAI_POLICER_ATTR_MODE, a))
    {
        switch (a.value.s32)
        {
            case SAI_POLICER_MODE_SR_TCM:        p.mode = HWBCM_COPP_POLICER_SR_TCM; break;
            case SAI_POLICER_MODE_TR_TCM:        p.mode = HWBCM_COPP_POLICER_TR_TCM; break;
            case SAI_POLICER_MODE_STORM_CONTROL: p.mode = HWBCM_COPP_POLICER_COMMITTED; break;

            default:
                SWSS_LOG_WARN("policer %s: mode %d not supported",
                        sai_serialize_object_id(policer_id).c_str(), a.value.s32);
                return false;
        }
    }

    if (getStoredAttr(SAI_OBJECT_TYPE_POLICER, policer_id, SAI_POLICER_ATTR_COLOR_SOURCE, a))
    {
        p.colorBlind = (a.value.s32 == SAI_POLICER_COLOR_SOURCE_BLIND) ? 1 : 0;
    }

    if (getStoredAttr(SAI_OBJECT_TYPE_POLICER, policer_id, SAI_POLICER_ATTR_CIR, a)) p.cir = a.value.u64;
    if (getStoredAttr(SAI_OBJECT_TYPE_POLICER, policer_id, SAI_POLICER_ATTR_CBS, a)) p.cbs = a.value.u64;
    if (getStoredAttr(SAI_OBJECT_TYPE_POLICER, policer_id, SAI_POLICER_ATTR_PIR, a)) p.pir = a.value.u64;
    if (getStoredAttr(SAI_OBJECT_TYPE_POLICER, policer_id, SAI_POLICER_ATTR_PBS, a)) p.pbs = a.value.u64;

    // colour actions (SAI default FORWARD). TRAP/COPY on a colour has no
    // meaning for a CPU-bound packet: treated as FORWARD.
    if (getStoredAttr(SAI_OBJECT_TYPE_POLICER, policer_id, SAI_POLICER_ATTR_GREEN_PACKET_ACTION, a))
        p.dropGreen = isDrop(a.value.s32) ? 1 : 0;
    if (getStoredAttr(SAI_OBJECT_TYPE_POLICER, policer_id, SAI_POLICER_ATTR_YELLOW_PACKET_ACTION, a))
        p.dropYellow = isDrop(a.value.s32) ? 1 : 0;
    if (getStoredAttr(SAI_OBJECT_TYPE_POLICER, policer_id, SAI_POLICER_ATTR_RED_PACKET_ACTION, a))
        p.dropRed = isDrop(a.value.s32) ? 1 : 0;

    return true;
}

static void coppToC(
        _In_ int packets,
        _In_ int mode,
        _In_ int colorBlind,
        _In_ uint64_t cir,
        _In_ uint64_t cbs,
        _In_ uint64_t pir,
        _In_ uint64_t pbs,
        _Out_ hwbcm_copp_policer_cfg_t &c)
{
    SWSS_LOG_ENTER();

    c.packets = packets;
    c.mode = mode;
    c.color_blind = colorBlind;
    c.cir = cir;
    c.cbs = cbs;
    c.pir = pir;
    c.pbs = pbs;
}

int SwitchBCM56860HW::coppBcmPolicer(
        _In_ sai_object_id_t policer_id,
        _Out_ int *rvOut)
{
    SWSS_LOG_ENTER();

    if (rvOut != nullptr)
    {
        *rvOut = 0;
    }

    auto it = m_coppPolicer.find(policer_id);

    if (it != m_coppPolicer.end())
    {
        return it->second;
    }

    CoppPolicerParams p;

    if (!coppReadPolicer(policer_id, p))
    {
        if (rvOut != nullptr)
        {
            *rvOut = HWBCM_E_UNAVAIL;
        }
        return -1;
    }

    hwbcm_copp_policer_cfg_t c;

    coppToC(p.packets, p.mode, p.colorBlind, p.cir, p.cbs, p.pir, p.pbs, c);

    int pid = -1;

    int rv = hwbcm_copp_policer_create(&c, &pid);

    if (rv < 0)
    {
        SWSS_LOG_ERROR("policer %s: hwbcm_copp_policer_create failed: %d",
                sai_serialize_object_id(policer_id).c_str(), rv);

        if (rvOut != nullptr)
        {
            *rvOut = rv;
        }
        return -1;
    }

    m_coppPolicer[policer_id] = pid;

    SWSS_LOG_NOTICE("policer %s -> bcm policer %d (%s, cir %" PRIu64 " cbs %" PRIu64 ")",
            sai_serialize_object_id(policer_id).c_str(), pid,
            p.packets ? "pps" : "Bps", p.cir, p.cbs);

    return pid;
}

void SwitchBCM56860HW::coppForgetTrap(
        _In_ sai_object_id_t trap_id)
{
    SWSS_LOG_ENTER();

    auto it = m_coppTrapHandle.find(trap_id);

    if (it == m_coppTrapHandle.end())
    {
        return;
    }

    int rv = hwbcm_copp_trap_remove(it->second);

    if (rv < 0)
    {
        SWSS_LOG_WARN("trap %s: hwbcm_copp_trap_remove(%d) failed: %d",
                sai_serialize_object_id(trap_id).c_str(), it->second, rv);
    }

    m_coppTrapHandle.erase(it);
}

sai_status_t SwitchBCM56860HW::coppApplyTrap(
        _In_ sai_object_id_t trap_id)
{
    SWSS_LOG_ENTER();

    const std::string sid = sai_serialize_object_id(trap_id);

    sai_attribute_t a;

    if (!getStoredAttr(SAI_OBJECT_TYPE_HOSTIF_TRAP, trap_id, SAI_HOSTIF_TRAP_ATTR_TRAP_TYPE, a))
    {
        coppForgetTrap(trap_id);
        return SAI_STATUS_SUCCESS;
    }

    const int32_t saiType = a.value.s32;

    hwbcm_copp_trap_cfg_t cfg;

    cfg.trap = coppTrapKind(saiType);
    cfg.action = 0;
    cfg.priority = 0;
    cfg.queue = 0;
    cfg.policer_id = -1;
    cfg.drop_green = 0;
    cfg.drop_yellow = 0;
    cfg.drop_red = 0;

    if (cfg.trap == HWBCM_COPP_TRAP_NONE)
    {
        SWSS_LOG_NOTICE("trap %s type %d: no CoPP classifier on this chip (not policed)", sid.c_str(), saiType);

        coppForgetTrap(trap_id);
        return SAI_STATUS_SUCCESS;
    }

    if (getStoredAttr(SAI_OBJECT_TYPE_HOSTIF_TRAP, trap_id, SAI_HOSTIF_TRAP_ATTR_PACKET_ACTION, a))
    {
        switch (a.value.s32)
        {
            case SAI_PACKET_ACTION_TRAP:
                cfg.action = HWBCM_COPP_ACT_TRAP;
                break;

            case SAI_PACKET_ACTION_COPY:
            case SAI_PACKET_ACTION_LOG:
                cfg.action = HWBCM_COPP_ACT_COPY;
                break;

            default:
                break;
        }
    }

    if (cfg.action == 0)
    {
        // DROP / FORWARD: nothing reaches the CPU, nothing to police
        coppForgetTrap(trap_id);
        return SAI_STATUS_SUCCESS;
    }

    if (getStoredAttr(SAI_OBJECT_TYPE_HOSTIF_TRAP, trap_id, SAI_HOSTIF_TRAP_ATTR_TRAP_PRIORITY, a))
    {
        cfg.priority = a.value.u32;
    }

    sai_object_id_t group = coppTrapGroupOf(trap_id);

    sai_object_id_t policer = SAI_NULL_OBJECT_ID;

    if (group != SAI_NULL_OBJECT_ID)
    {
        if (getStoredAttr(SAI_OBJECT_TYPE_HOSTIF_TRAP_GROUP, group, SAI_HOSTIF_TRAP_GROUP_ATTR_QUEUE, a))
        {
            cfg.queue = (int)a.value.u32;
        }

        if (getStoredAttr(SAI_OBJECT_TYPE_HOSTIF_TRAP_GROUP, group, SAI_HOSTIF_TRAP_GROUP_ATTR_POLICER, a))
        {
            policer = a.value.oid;
        }
    }

    if (policer != SAI_NULL_OBJECT_ID)
    {
        CoppPolicerParams p;
        int prv = 0;

        int pid = coppBcmPolicer(policer, &prv);

        if (pid >= 0 && coppReadPolicer(policer, p))
        {
            cfg.policer_id = pid;
            cfg.drop_green = p.dropGreen;
            cfg.drop_yellow = p.dropYellow;
            cfg.drop_red = p.dropRed;
        }
        else
        {
            SWSS_LOG_ERROR("trap %s: policer %s failed to program (rv=%d), refusing unpoliced trap",
                    sid.c_str(), sai_serialize_object_id(policer).c_str(), prv);
            return (prv < 0) ? bcmRvToSai(prv) : SAI_STATUS_NOT_SUPPORTED;
        }
    }

    int handle = -1;

    auto it = m_coppTrapHandle.find(trap_id);

    if (it != m_coppTrapHandle.end())
    {
        handle = it->second;
    }

    int rv = hwbcm_copp_trap_apply(&cfg, &handle);

    if (handle >= 0)
    {
        m_coppTrapHandle[trap_id] = handle;
    }

    if (rv < 0)
    {
        SWSS_LOG_ERROR("trap %s type %d: hwbcm_copp_trap_apply(queue %d prio %u policer %d) failed: %d (steering unchanged)",
                sid.c_str(), saiType, cfg.queue, cfg.priority, cfg.policer_id, rv);
        return (rv == HWBCM_E_UNAVAIL) ? SAI_STATUS_SUCCESS : bcmRvToSai(rv);
    }

    SWSS_LOG_NOTICE("trap %s type %d: group %s queue %d prio %u policer %d, %d FP entries%s",
            sid.c_str(), saiType,
            sai_serialize_object_id(group).c_str(),
            cfg.queue, cfg.priority, cfg.policer_id,
            hwbcm_copp_trap_entries(handle),
            hwbcm_copp_trap_entries(handle) == 0 ? " (pending ip2me egress)" : "");

    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchBCM56860HW::coppApplyGroup(
        _In_ sai_object_id_t group_id)
{
    SWSS_LOG_ENTER();

    sai_status_t firstErr = SAI_STATUS_SUCCESS;

    for (auto trap: coppAllTraps())
    {
        if (coppTrapGroupOf(trap) == group_id)
        {
            sai_status_t st = coppApplyTrap(trap);
            if (st != SAI_STATUS_SUCCESS && firstErr == SAI_STATUS_SUCCESS)
            {
                firstErr = st;
            }
        }
    }

    return firstErr;
}

sai_status_t SwitchBCM56860HW::coppApplyPolicerUsers(
        _In_ sai_object_id_t policer_id)
{
    SWSS_LOG_ENTER();

    sai_status_t firstErr = SAI_STATUS_SUCCESS;

    for (auto trap: coppAllTraps())
    {
        sai_object_id_t group = coppTrapGroupOf(trap);

        sai_attribute_t a;

        if (group != SAI_NULL_OBJECT_ID &&
                getStoredAttr(SAI_OBJECT_TYPE_HOSTIF_TRAP_GROUP, group, SAI_HOSTIF_TRAP_GROUP_ATTR_POLICER, a) &&
                a.value.oid == policer_id)
        {
            sai_status_t st = coppApplyTrap(trap);
            if (st != SAI_STATUS_SUCCESS && firstErr == SAI_STATUS_SUCCESS)
            {
                firstErr = st;
            }
        }
    }

    return firstErr;
}

// ---------------------------------------------------------------------------
// POLICER
// ---------------------------------------------------------------------------

sai_status_t SwitchBCM56860HW::hwCreatePolicer(
        _In_ sai_object_id_t policer_id,
        _In_ uint32_t attr_count,
        _In_ const sai_attribute_t *attr_list)
{
    SWSS_LOG_ENTER();

    // The bcm policer is created when a trap group first references it
    // (FP policers are per slice; an ACL or storm-control user of the same
    // SAI policer needs its own meter anyway). Only validate here.
    CoppPolicerParams p;

    if (!coppReadPolicer(policer_id, p))
    {
        SWSS_LOG_ERROR("policer %s: unsupported meter type or mode on TD2+",
                sai_serialize_object_id(policer_id).c_str());
        return SAI_STATUS_NOT_SUPPORTED;
    }

    (void)attr_count;
    (void)attr_list;

    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchBCM56860HW::hwSetPolicer(
        _In_ sai_object_id_t policer_id,
        _In_ const sai_attribute_t *attr)
{
    SWSS_LOG_ENTER();

    auto it = m_coppPolicer.find(policer_id);

    if (it == m_coppPolicer.end())
    {
        return SAI_STATUS_SUCCESS; // not in HW yet, next use reads the new value
    }

    switch (attr->id)
    {
        case SAI_POLICER_ATTR_CIR:
        case SAI_POLICER_ATTR_CBS:
        case SAI_POLICER_ATTR_PIR:
        case SAI_POLICER_ATTR_PBS:
            {
                CoppPolicerParams p;

                if (!coppReadPolicer(policer_id, p))
                {
                    break;
                }

                hwbcm_copp_policer_cfg_t c;

                coppToC(p.packets, p.mode, p.colorBlind, p.cir, p.cbs, p.pir, p.pbs, c);

                int rv = hwbcm_copp_policer_set(it->second, &c);

                if (rv < 0)
                {
                    SWSS_LOG_ERROR("policer %s: hwbcm_copp_policer_set(%d) failed: %d",
                            sai_serialize_object_id(policer_id).c_str(), it->second, rv);
                    return bcmRvToSai(rv);
                }
            }
            break;

        case SAI_POLICER_ATTR_GREEN_PACKET_ACTION:
        case SAI_POLICER_ATTR_YELLOW_PACKET_ACTION:
        case SAI_POLICER_ATTR_RED_PACKET_ACTION:

            // colour actions live in the FP entries
            return coppApplyPolicerUsers(policer_id);

        default:
            SWSS_LOG_INFO("policer %s attr %d: no HW effect",
                    sai_serialize_object_id(policer_id).c_str(), attr->id);
            break;
    }

    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchBCM56860HW::hwRemovePolicer(
        _In_ sai_object_id_t policer_id)
{
    SWSS_LOG_ENTER();

    auto it = m_coppPolicer.find(policer_id);

    if (it == m_coppPolicer.end())
    {
        return SAI_STATUS_SUCCESS;
    }

    int rv = hwbcm_copp_policer_destroy(it->second);

    if (rv == HWBCM_COPP_E_BUSY)
    {
        // still attached (a group still points at it): re-apply its users
        // from the current state once, then retry
        coppApplyPolicerUsers(policer_id);

        rv = hwbcm_copp_policer_destroy(it->second);
    }

    if (rv < 0 && rv != HWBCM_COPP_E_NOT_FOUND)
    {
        SWSS_LOG_ERROR("policer %s: hwbcm_copp_policer_destroy(%d) failed: %d",
                sai_serialize_object_id(policer_id).c_str(), it->second, rv);

        return rv == HWBCM_COPP_E_BUSY ? SAI_STATUS_OBJECT_IN_USE : SAI_STATUS_FAILURE;
    }

    m_coppPolicer.erase(it);

    return SAI_STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// HOSTIF_TRAP_GROUP
// ---------------------------------------------------------------------------

sai_status_t SwitchBCM56860HW::hwCreateHostifTrapGroup(
        _In_ sai_object_id_t group_id,
        _In_ uint32_t attr_count,
        _In_ const sai_attribute_t *attr_list)
{
    SWSS_LOG_ENTER();

    // No HW object: a trap group is its traps' FP entries (queue + policer).
    // A new group has no traps yet.
    auto *q = findAttr(attr_count, attr_list, SAI_HOSTIF_TRAP_GROUP_ATTR_QUEUE);

    SWSS_LOG_INFO("trap group %s created (queue %d)",
            sai_serialize_object_id(group_id).c_str(),
            q ? (int)q->value.u32 : 0);

    return coppApplyGroup(group_id);
}

sai_status_t SwitchBCM56860HW::hwSetHostifTrapGroup(
        _In_ sai_object_id_t group_id,
        _In_ const sai_attribute_t *attr)
{
    SWSS_LOG_ENTER();

    switch (attr->id)
    {
        case SAI_HOSTIF_TRAP_GROUP_ATTR_QUEUE:
        case SAI_HOSTIF_TRAP_GROUP_ATTR_POLICER:

            return coppApplyGroup(group_id);

        default:
            SWSS_LOG_INFO("trap group %s attr %d: no HW effect",
                    sai_serialize_object_id(group_id).c_str(), attr->id);
            break;
    }

    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchBCM56860HW::hwRemoveHostifTrapGroup(
        _In_ sai_object_id_t group_id)
{
    SWSS_LOG_ENTER();

    // copporch moves/removes the traps before removing the group; nothing
    // in HW belongs to the group itself.
    SWSS_LOG_INFO("trap group %s removed", sai_serialize_object_id(group_id).c_str());

    return SAI_STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// HOSTIF_TRAP hooks
// ---------------------------------------------------------------------------

sai_status_t SwitchBCM56860HW::hwCoppOnTrapCreate(
        _In_ sai_object_id_t trap_id)
{
    SWSS_LOG_ENTER();

    return coppApplyTrap(trap_id);
}

sai_status_t SwitchBCM56860HW::hwCoppOnTrapSet(
        _In_ sai_object_id_t trap_id,
        _In_ const sai_attribute_t *attr)
{
    SWSS_LOG_ENTER();

    switch (attr->id)
    {
        case SAI_HOSTIF_TRAP_ATTR_PACKET_ACTION:
        case SAI_HOSTIF_TRAP_ATTR_TRAP_GROUP:
            {
                // re-run the CPU steering (hwCreateHostifTrap only looks at
                // TRAP_TYPE / PACKET_ACTION / TRAP_GROUP) with the new values
                sai_attribute_t attrs[3];
                uint32_t n = 0;

                if (getStoredAttr(SAI_OBJECT_TYPE_HOSTIF_TRAP, trap_id, SAI_HOSTIF_TRAP_ATTR_TRAP_TYPE, attrs[n])) n++;
                if (getStoredAttr(SAI_OBJECT_TYPE_HOSTIF_TRAP, trap_id, SAI_HOSTIF_TRAP_ATTR_PACKET_ACTION, attrs[n])) n++;
                if (getStoredAttr(SAI_OBJECT_TYPE_HOSTIF_TRAP, trap_id, SAI_HOSTIF_TRAP_ATTR_TRAP_GROUP, attrs[n])) n++;

                if (attr->id == SAI_HOSTIF_TRAP_ATTR_PACKET_ACTION)
                {
                    sai_status_t st = hwCreateHostifTrap(trap_id, n, attrs);
                    if (st != SAI_STATUS_SUCCESS)
                    {
                        SWSS_LOG_ERROR("trap %s: hwCreateHostifTrap on PACKET_ACTION set failed: %d",
                                sai_serialize_object_id(trap_id).c_str(), st);
                        return st;
                    }
                }

                return coppApplyTrap(trap_id);
            }

        case SAI_HOSTIF_TRAP_ATTR_TRAP_PRIORITY:

            return coppApplyTrap(trap_id);

        default:
            // EXCLUDE_PORT_LIST, MIRROR_SESSION: not implemented;
            // COUNTER_ID: stats come from the FP entries (hwCoppCounterStats)
            SWSS_LOG_INFO("trap %s attr %d: no CoPP effect",
                    sai_serialize_object_id(trap_id).c_str(), attr->id);
            break;
    }

    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchBCM56860HW::hwCoppOnTrapRemove(
        _In_ sai_object_id_t trap_id)
{
    SWSS_LOG_ENTER();

    coppForgetTrap(trap_id);

    sai_attribute_t type;
    if (getStoredAttr(SAI_OBJECT_TYPE_HOSTIF_TRAP, trap_id, SAI_HOSTIF_TRAP_ATTR_TRAP_TYPE, type))
    {
        int reason = hwTrapReasonOf(type.value.s32);
        if (reason >= 0)
        {
            return hwReconcileTrapReason(reason, trap_id);
        }
    }

    return SAI_STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// route / switch hooks
// ---------------------------------------------------------------------------

void SwitchBCM56860HW::hwCoppOnRoute(
        _In_ const std::string &serializedRoute)
{
    SWSS_LOG_ENTER();

    auto it = m_routeEgr.find(serializedRoute);

    if (it == m_routeEgr.end())
    {
        return;
    }

    // Fast path: once IP2ME CPU egress is known (or egress is already the
    // known local egress), avoid the m_objectHash lookup on every route add.
    if (m_coppLocalEgress >= 0 && it->second == m_coppLocalEgress)
    {
        return;
    }

    sai_attribute_t a;

    if (!getStoredAttr(SAI_OBJECT_TYPE_ROUTE_ENTRY, serializedRoute, SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID, a) ||
            a.value.oid != m_cpu_port_id)
    {
        return;
    }

    SWSS_LOG_NOTICE("ip2me route %s: CPU egress %d, installing to-me CoPP classifiers",
            serializedRoute.c_str(), it->second);

    m_coppLocalEgress = it->second;

    int rv = hwbcm_copp_local_egress_set(it->second);

    if (rv < 0)
    {
        SWSS_LOG_ERROR("hwbcm_copp_local_egress_set(%d) failed: %d", it->second, rv);
    }
}

void SwitchBCM56860HW::hwCoppRefresh()
{
    SWSS_LOG_ENTER();

    int rv = hwbcm_copp_refresh();

    if (rv < 0)
    {
        SWSS_LOG_ERROR("hwbcm_copp_refresh failed: %d", rv);
    }
}

bool SwitchBCM56860HW::hwCoppCounterStats(
        _In_ sai_object_id_t counter_id,
        _In_ uint32_t number_of_counters,
        _In_ const sai_stat_id_t *counter_ids,
        _In_ sai_stats_mode_t mode,
        _Out_ uint64_t *counters)
{
    SWSS_LOG_ENTER();

    if (counter_id == SAI_NULL_OBJECT_ID || counter_ids == NULL || counters == NULL)
    {
        return false;
    }

    for (auto trap: coppAllTraps())
    {
        sai_attribute_t a;

        if (!getStoredAttr(SAI_OBJECT_TYPE_HOSTIF_TRAP, trap, SAI_HOSTIF_TRAP_ATTR_COUNTER_ID, a) ||
                a.value.oid != counter_id)
        {
            continue;
        }

        auto it = m_coppTrapHandle.find(trap);

        if (it == m_coppTrapHandle.end())
        {
            return false;
        }

        uint64_t pkts = 0;
        uint64_t bytes = 0;
        uint64_t red = 0;

        if (hwbcm_copp_trap_stats_get(it->second, &pkts, &bytes, &red) < 0)
        {
            return false;
        }

        for (uint32_t i = 0; i < number_of_counters; i++)
        {
            switch (counter_ids[i])
            {
                case SAI_COUNTER_STAT_PACKETS: counters[i] = pkts; break;
                case SAI_COUNTER_STAT_BYTES:   counters[i] = bytes; break;
                default:                       counters[i] = 0; break;
            }
        }

        if (mode == SAI_STATS_MODE_READ_AND_CLEAR)
        {
            (void)hwbcm_copp_trap_stats_clear(it->second);
        }

        return true;
    }

    return false;
}
