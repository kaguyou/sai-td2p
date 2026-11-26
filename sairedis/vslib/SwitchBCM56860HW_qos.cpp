/*
 * SwitchBCM56860HW_qos.cpp — QoS and buffers for the Trident2+ hardware switch.
 *
 * Owner: A6. Handles SAI QOS_MAP, SCHEDULER, WRED, BUFFER_POOL, BUFFER_PROFILE,
 * the QoS attributes of QUEUE / SCHEDULER_GROUP / INGRESS_PRIORITY_GROUP, and
 * the port / switch QoS attributes (maps, default TC, PFC, port shaper).
 *
 * What SONiC programs (qosorch / bufferorch / portsorch, 202505):
 *   QOS_MAP objects bound per port (PORT_QOS_MAP) and optionally globally
 *   (SAI_SWITCH_ATTR_QOS_DSCP_TO_TC_MAP); SCHEDULER objects set on the
 *   SCHEDULER_GROUP whose child is the queue (QosOrch::
 *   applySchedulerToQueueSchedulerGroup) and on the port (port shaper);
 *   WRED_PROFILE_ID on unicast queues; BUFFER_POOL / BUFFER_PROFILE with
 *   profiles on PGs (BUFFER_PG) and queues (BUFFER_QUEUE); PRIORITY_FLOW_CONTROL
 *   bitmap per port.
 *
 * Mapping to TD2+ (hwbcm_qos.c, SDK non-ETS mode):
 *   vslib queue  UC index i / MC index 10+i  -> (bcm port, cos i)  m_qosQueueHw
 *   vslib SG tree root / L0 x2 / L1 x10       -> port shaper / - / L1.cos  m_qosSgHw
 *   vslib PG index i                          -> THDI PG i         m_qosPgHw
 *   QOS_MAP content (per port, after switch-level fallback) -> one bcm qos map
 *   per distinct content (m_qosHwMaps, refcounted)
 *   BUFFER_POOL -> service pool id, in creation order per direction
 *
 * Everything is desired state first: bindings are kept in maps and re-applied
 * when the profile/map they point at changes, so SAI set on a profile reaches
 * every queue/PG/port that uses it.
 */
#include "SwitchBCM56860HW.h"

#include "swss/logger.h"
#include "meta/sai_serialize.h"

#include "hwbcm/hwbcm.h"
#include "hwbcm/hwbcm_qos.h"

#include <cstring>
#include <cinttypes>
#include <algorithm>
#include <functional>

using namespace saivs;

/* BCM_E_NOT_FOUND (sdk include/shared/error.h), not reachable from C++. */
#define QOS_HWBCM_E_NOT_FOUND (-7)

// ---------------------------------------------------------------------------
// HW map accessors (for statistics)
// ---------------------------------------------------------------------------

bool SwitchBCM56860HW::qosQueueHwInfo(
        _In_ sai_object_id_t queue_id,
        _Out_ QosQueueHw &out) const
{
    SWSS_LOG_ENTER();

    auto it = m_qosQueueHw.find(queue_id);

    if (it == m_qosQueueHw.end())
    {
        return false;
    }

    out = it->second;

    return true;
}

bool SwitchBCM56860HW::qosPgHwInfo(
        _In_ sai_object_id_t pg_id,
        _Out_ QosPgHw &out) const
{
    SWSS_LOG_ENTER();

    auto it = m_qosPgHw.find(pg_id);

    if (it == m_qosPgHw.end())
    {
        return false;
    }

    out = it->second;

    return true;
}

bool SwitchBCM56860HW::qosSgHwInfo(
        _In_ sai_object_id_t sg_id,
        _Out_ QosSgHw &out) const
{
    SWSS_LOG_ENTER();

    auto it = m_qosSgHw.find(sg_id);

    if (it == m_qosSgHw.end())
    {
        return false;
    }

    out = it->second;

    return true;
}

// ---------------------------------------------------------------------------
// oid -> HW maps
// ---------------------------------------------------------------------------

void SwitchBCM56860HW::qosMapPort(
        _In_ sai_object_id_t port_id)
{
    SWSS_LOG_ENTER();

    const bool cpu = (port_id == m_cpu_port_id);
    const int bcm = cpu ? 0 : bcmPortOf(port_id);

    if (!cpu && bcm < 0)
    {
        return;
    }

    sai_attribute_t attr;

    // queues

    std::vector<sai_object_id_t> queues;

    if (getStoredAttr(SAI_OBJECT_TYPE_PORT, port_id, SAI_PORT_ATTR_QOS_QUEUE_LIST, attr) &&
            attr.value.objlist.list != NULL)
    {
        queues.assign(attr.value.objlist.list, attr.value.objlist.list + attr.value.objlist.count);
    }

    int ucCount = 0;
    std::vector<bool> isMc(queues.size(), cpu);

    for (size_t i = 0; i < queues.size(); i++)
    {
        sai_attribute_t t;

        if (getStoredAttr(SAI_OBJECT_TYPE_QUEUE, queues[i], SAI_QUEUE_ATTR_TYPE, t))
        {
            isMc[i] = (t.value.s32 == SAI_QUEUE_TYPE_MULTICAST);
        }

        if (!isMc[i])
        {
            ucCount++;
        }
    }

    for (size_t i = 0; i < queues.size(); i++)
    {
        QosQueueHw h;

        h.port = port_id;
        h.bcmPort = bcm;
        h.cpu = cpu;
        h.mcast = isMc[i];

        int idx = (int)i;
        sai_attribute_t t;

        if (getStoredAttr(SAI_OBJECT_TYPE_QUEUE, queues[i], SAI_QUEUE_ATTR_INDEX, t))
        {
            idx = (int)t.value.u8;
        }

        // vslib numbers UC queues 0..n-1 and MC queues n..2n-1 (SwitchBCM56850::
        // create_qos_queues_per_port); the CPU port has MC queues 0..31.
        h.cos = (cpu || !h.mcast) ? idx : idx - ucCount;

        m_qosQueueHw[queues[i]] = h;
    }

    if (cpu)
    {
        return;
    }

    // scheduler groups

    std::vector<sai_object_id_t> sgs;

    if (getStoredAttr(SAI_OBJECT_TYPE_PORT, port_id, SAI_PORT_ATTR_QOS_SCHEDULER_GROUP_LIST, attr) &&
            attr.value.objlist.list != NULL)
    {
        sgs.assign(attr.value.objlist.list, attr.value.objlist.list + attr.value.objlist.count);
    }

    std::map<sai_object_id_t, std::vector<sai_object_id_t>> children;
    std::set<sai_object_id_t> isChild;

    for (auto sg: sgs)
    {
        sai_attribute_t c;

        if (getStoredAttr(SAI_OBJECT_TYPE_SCHEDULER_GROUP, sg, SAI_SCHEDULER_GROUP_ATTR_CHILD_LIST, c) &&
                c.value.objlist.list != NULL)
        {
            children[sg].assign(c.value.objlist.list, c.value.objlist.list + c.value.objlist.count);

            for (auto ch: children[sg])
            {
                isChild.insert(ch);
            }
        }
    }

    int l0 = 0;

    for (auto sg: sgs)
    {
        QosSgHw h;

        h.port = port_id;
        h.bcmPort = bcm;

        int queueCos = -1;

        for (auto ch: children[sg])
        {
            auto qit = m_qosQueueHw.find(ch);

            if (qit != m_qosQueueHw.end())
            {
                if (queueCos < 0 || !qit->second.mcast)
                {
                    queueCos = qit->second.cos;
                }
            }
        }

        if (queueCos >= 0)
        {
            h.level = 2;
            h.index = queueCos;
        }
        else if (isChild.find(sg) == isChild.end())
        {
            h.level = 0;
            h.index = 0;
        }
        else
        {
            h.level = 1;
            h.index = l0++;
        }

        m_qosSgHw[sg] = h;
    }

    // ingress priority groups

    std::vector<sai_object_id_t> pgs;

    if (getStoredAttr(SAI_OBJECT_TYPE_PORT, port_id, SAI_PORT_ATTR_INGRESS_PRIORITY_GROUP_LIST, attr) &&
            attr.value.objlist.list != NULL)
    {
        pgs.assign(attr.value.objlist.list, attr.value.objlist.list + attr.value.objlist.count);
    }

    for (size_t i = 0; i < pgs.size(); i++)
    {
        QosPgHw h;

        h.port = port_id;
        h.bcmPort = bcm;
        h.pg = (int)i;

        sai_attribute_t t;

        // SwitchStateBase stores the index through value.oid; the low byte is it.
        if (getStoredAttr(SAI_OBJECT_TYPE_INGRESS_PRIORITY_GROUP, pgs[i], SAI_INGRESS_PRIORITY_GROUP_ATTR_INDEX, t))
        {
            h.pg = (int)t.value.u8;
        }

        if (h.pg < 0 || h.pg >= HWBCM_QOS_NUM_PG)
        {
            SWSS_LOG_WARN("PG %s of port %s has index %d, not mapped to hardware",
                    sai_serialize_object_id(pgs[i]).c_str(),
                    sai_serialize_object_id(port_id).c_str(), h.pg);
            continue;
        }

        m_qosPgHw[pgs[i]] = h;
    }

    SWSS_LOG_INFO("qos: port %s (bcm %d): %zu queues, %zu scheduler groups, %zu PGs mapped",
            sai_serialize_object_id(port_id).c_str(), bcm, queues.size(), sgs.size(), pgs.size());
}

void SwitchBCM56860HW::qosUnmapPort(
        _In_ sai_object_id_t port_id)
{
    SWSS_LOG_ENTER();

    for (auto it = m_qosQueueHw.begin(); it != m_qosQueueHw.end();)
    {
        if (it->second.port == port_id)
        {
            m_qosQueueSched.erase(it->first);
            m_qosQueueWred.erase(it->first);
            m_qosQueueProfile.erase(it->first);
            it = m_qosQueueHw.erase(it);
        }
        else
        {
            ++it;
        }
    }

    for (auto it = m_qosSgHw.begin(); it != m_qosSgHw.end();)
    {
        if (it->second.port == port_id)
        {
            m_qosSgSched.erase(it->first);
            it = m_qosSgHw.erase(it);
        }
        else
        {
            ++it;
        }
    }

    for (auto it = m_qosPgHw.begin(); it != m_qosPgHw.end();)
    {
        if (it->second.port == port_id)
        {
            m_qosPgProfile.erase(it->first);
            m_qosPgPending.erase(it->first);
            it = m_qosPgHw.erase(it);
        }
        else
        {
            ++it;
        }
    }

    auto pit = m_qosPort.find(port_id);

    if (pit != m_qosPort.end())
    {
        qosHwMapRelease(pit->second.hwIngDscpKey);
        qosHwMapRelease(pit->second.hwIngDot1pKey);
        qosHwMapRelease(pit->second.hwEgrDscpKey);

        m_qosPort.erase(pit);
    }
}

sai_status_t SwitchBCM56860HW::qosBuildHwMap(
        _In_ bool warmBoot)
{
    SWSS_LOG_ENTER();

    if (!m_hwReady)
    {
        return SAI_STATUS_SUCCESS;
    }

    hwbcm_qos_init();

    m_qosQueueHw.clear();
    m_qosSgHw.clear();
    m_qosPgHw.clear();

    for (auto port_id: m_port_list)
    {
        qosMapPort(port_id);
    }

    if (m_cpu_port_id != SAI_NULL_OBJECT_ID &&
            std::find(m_port_list.begin(), m_port_list.end(), m_cpu_port_id) == m_port_list.end())
    {
        qosMapPort(m_cpu_port_id);
    }

    SWSS_LOG_NOTICE("qos: hardware map built (%s): %zu queues, %zu scheduler groups, %zu PGs",
            warmBoot ? "warm/SW-only" : "cold",
            m_qosQueueHw.size(), m_qosSgHw.size(), m_qosPgHw.size());

    auto forEach = [&](sai_object_type_t ot, std::function<void(sai_object_id_t)> fn)
    {
        auto oit = m_objectHash.find(ot);

        if (oit == m_objectHash.end())
        {
            return;
        }

        std::vector<sai_object_id_t> oids;

        for (auto &kv: oit->second)
        {
            sai_object_id_t oid = SAI_NULL_OBJECT_ID;

            sai_deserialize_object_id(kv.first, oid);

            oids.push_back(oid);
        }

        for (auto oid: oids)
        {
            fn(oid);
        }
    };

    m_qosMapType.clear();
    m_qosSchedulers.clear();
    m_qosWreds.clear();
    m_qosProfiles.clear();

    forEach(SAI_OBJECT_TYPE_QOS_MAP, [&](sai_object_id_t oid) {
            sai_attribute_t a;
            if (getStoredAttr(SAI_OBJECT_TYPE_QOS_MAP, oid, SAI_QOS_MAP_ATTR_TYPE, a))
            {
                m_qosMapType[oid] = a.value.s32;
            }
    });

    forEach(SAI_OBJECT_TYPE_SCHEDULER, [&](sai_object_id_t oid) { m_qosSchedulers.insert(oid); });
    forEach(SAI_OBJECT_TYPE_WRED, [&](sai_object_id_t oid) { m_qosWreds.insert(oid); });
    forEach(SAI_OBJECT_TYPE_BUFFER_PROFILE, [&](sai_object_id_t oid) { m_qosProfiles.insert(oid); });

    if (warmBoot)
    {
        // Warm boot (Phase 3): rebuild all pure-SW QoS desired-state binding maps
        // from m_objectHash with ZERO HW writes, while preserving m_qosHwMaps,
        // m_qosPools, m_qos*SpUsed, m_qosPgPending and per-port hw*Key / pfcTouched
        // restored by hwStateLoad().
        for (int s = 0; s < QOS_SLOT_COUNT; s++)
        {
            m_qosSwitchMaps[s] = SAI_NULL_OBJECT_ID;
        }
        m_qosSwitchDefaultTc = -1;

        static const sai_attr_id_t wbSwitchAttrs[] = {
            SAI_SWITCH_ATTR_QOS_DSCP_TO_TC_MAP, SAI_SWITCH_ATTR_QOS_DSCP_TO_COLOR_MAP,
            SAI_SWITCH_ATTR_QOS_DOT1P_TO_TC_MAP, SAI_SWITCH_ATTR_QOS_DOT1P_TO_COLOR_MAP,
            SAI_SWITCH_ATTR_QOS_TC_TO_QUEUE_MAP, SAI_SWITCH_ATTR_QOS_TC_AND_COLOR_TO_DSCP_MAP,
            SAI_SWITCH_ATTR_QOS_DEFAULT_TC,
        };
        for (auto id: wbSwitchAttrs)
        {
            sai_attribute_t a;
            if (!getStoredAttr(SAI_OBJECT_TYPE_SWITCH, m_switch_id, id, a))
            {
                continue;
            }
            if (id == SAI_SWITCH_ATTR_QOS_DEFAULT_TC)
            {
                m_qosSwitchDefaultTc = (int)a.value.u8;
            }
            else if (a.value.oid != SAI_NULL_OBJECT_ID)
            {
                int slot = qosSlotOfSwitchAttr(id);
                if (slot >= 0)
                {
                    m_qosSwitchMaps[slot] = a.value.oid;
                }
            }
        }

        static const sai_attr_id_t wbPortAttrs[] = {
            SAI_PORT_ATTR_QOS_DSCP_TO_TC_MAP, SAI_PORT_ATTR_QOS_DSCP_TO_COLOR_MAP,
            SAI_PORT_ATTR_QOS_DOT1P_TO_TC_MAP, SAI_PORT_ATTR_QOS_DOT1P_TO_COLOR_MAP,
            SAI_PORT_ATTR_QOS_TC_TO_QUEUE_MAP, SAI_PORT_ATTR_QOS_TC_TO_PRIORITY_GROUP_MAP,
            SAI_PORT_ATTR_QOS_PFC_PRIORITY_TO_QUEUE_MAP, SAI_PORT_ATTR_QOS_PFC_PRIORITY_TO_PRIORITY_GROUP_MAP,
            SAI_PORT_ATTR_QOS_TC_AND_COLOR_TO_DSCP_MAP, SAI_PORT_ATTR_QOS_DEFAULT_TC,
            SAI_PORT_ATTR_QOS_SCHEDULER_PROFILE_ID, SAI_PORT_ATTR_PRIORITY_FLOW_CONTROL_MODE,
            SAI_PORT_ATTR_PRIORITY_FLOW_CONTROL, SAI_PORT_ATTR_PRIORITY_FLOW_CONTROL_RX,
            SAI_PORT_ATTR_PRIORITY_FLOW_CONTROL_TX,
        };

        for (auto port_id: m_port_list)
        {
            if (bcmPortOf(port_id) < 0)
            {
                continue;
            }
            // Preserve hwIngDscpKey, hwIngDot1pKey, hwEgrDscpKey, pfcTouched restored from .hwstate
            auto &ps = qosPortState(port_id);
            for (int s = 0; s < QOS_SLOT_COUNT; s++) ps.maps[s] = SAI_NULL_OBJECT_ID;
            ps.defaultTc = -1;
            ps.scheduler = SAI_NULL_OBJECT_ID;
            ps.pfcSeparate = false;
            ps.pfcCombined = 0;
            ps.pfcTx = 0;
            ps.pfcRx = 0;

            for (auto id: wbPortAttrs)
            {
                sai_attribute_t a;
                if (!getStoredAttr(SAI_OBJECT_TYPE_PORT, port_id, id, a))
                {
                    continue;
                }
                int slot = qosSlotOfPortAttr(id);
                if (slot >= 0)
                {
                    ps.maps[slot] = a.value.oid;
                    continue;
                }
                switch (id)
                {
                    case SAI_PORT_ATTR_QOS_DEFAULT_TC:
                        ps.defaultTc = (int)a.value.u8;
                        break;
                    case SAI_PORT_ATTR_QOS_SCHEDULER_PROFILE_ID:
                        ps.scheduler = a.value.oid;
                        break;
                    case SAI_PORT_ATTR_PRIORITY_FLOW_CONTROL_MODE:
                        ps.pfcSeparate = (a.value.s32 == SAI_PORT_PRIORITY_FLOW_CONTROL_MODE_SEPARATE);
                        break;
                    case SAI_PORT_ATTR_PRIORITY_FLOW_CONTROL:
                        ps.pfcCombined = a.value.u8;
                        if (a.value.u8 != 0) ps.pfcTouched = true;
                        break;
                    case SAI_PORT_ATTR_PRIORITY_FLOW_CONTROL_RX:
                        ps.pfcRx = a.value.u8;
                        if (a.value.u8 != 0) ps.pfcTouched = true;
                        break;
                    case SAI_PORT_ATTR_PRIORITY_FLOW_CONTROL_TX:
                        ps.pfcTx = a.value.u8;
                        if (a.value.u8 != 0) ps.pfcTouched = true;
                        break;
                    default:
                        break;
                }
            }
        }

        m_qosSgSched.clear();
        for (const auto &kv: m_qosSgHw)
        {
            sai_attribute_t a;
            if (getStoredAttr(SAI_OBJECT_TYPE_SCHEDULER_GROUP, kv.first,
                        SAI_SCHEDULER_GROUP_ATTR_SCHEDULER_PROFILE_ID, a) &&
                a.value.oid != SAI_NULL_OBJECT_ID)
            {
                m_qosSgSched[kv.first] = a.value.oid;
            }
        }

        m_qosQueueSched.clear();
        m_qosQueueWred.clear();
        m_qosQueueProfile.clear();
        for (const auto &kv: m_qosQueueHw)
        {
            sai_attribute_t a;
            if (getStoredAttr(SAI_OBJECT_TYPE_QUEUE, kv.first, SAI_QUEUE_ATTR_SCHEDULER_PROFILE_ID, a) &&
                a.value.oid != SAI_NULL_OBJECT_ID)
            {
                m_qosQueueSched[kv.first] = a.value.oid;
            }
            if (getStoredAttr(SAI_OBJECT_TYPE_QUEUE, kv.first, SAI_QUEUE_ATTR_WRED_PROFILE_ID, a) &&
                a.value.oid != SAI_NULL_OBJECT_ID)
            {
                m_qosQueueWred[kv.first] = a.value.oid;
            }
            if (getStoredAttr(SAI_OBJECT_TYPE_QUEUE, kv.first, SAI_QUEUE_ATTR_BUFFER_PROFILE_ID, a) &&
                a.value.oid != SAI_NULL_OBJECT_ID)
            {
                m_qosQueueProfile[kv.first] = a.value.oid;
            }
        }

        m_qosPgProfile.clear();
        for (const auto &kv: m_qosPgHw)
        {
            sai_attribute_t a;
            if (getStoredAttr(SAI_OBJECT_TYPE_INGRESS_PRIORITY_GROUP, kv.first,
                        SAI_INGRESS_PRIORITY_GROUP_ATTR_BUFFER_PROFILE, a) &&
                a.value.oid != SAI_NULL_OBJECT_ID)
            {
                m_qosPgProfile[kv.first] = a.value.oid;
            }
        }

        SWSS_LOG_NOTICE("qos: warm SW bindings rebuilt (maps=%zu hwMaps=%zu pools=%zu scheds=%zu sgSched=%zu qSched=%zu qWred=%zu qProf=%zu pgProf=%zu pgPending=%zu)",
                m_qosMapType.size(), m_qosHwMaps.size(), m_qosPools.size(),
                m_qosSchedulers.size(), m_qosSgSched.size(), m_qosQueueSched.size(),
                m_qosQueueWred.size(), m_qosQueueProfile.size(), m_qosPgProfile.size(),
                m_qosPgPending.size());
        return SAI_STATUS_SUCCESS;
    }

    // Resync desired state from the object hash (cold fallback with warm DB).
    // On a normal cold start these tables are empty and this is a no-op.

    forEach(SAI_OBJECT_TYPE_BUFFER_POOL, [&](sai_object_id_t oid) {
            if (m_qosPools.find(oid) == m_qosPools.end())
            {
                (void)hwCreateBufferPool(oid, 0, NULL);
            }
    });

    static const sai_attr_id_t switchAttrs[] = {
        SAI_SWITCH_ATTR_QOS_DSCP_TO_TC_MAP, SAI_SWITCH_ATTR_QOS_DSCP_TO_COLOR_MAP,
        SAI_SWITCH_ATTR_QOS_DOT1P_TO_TC_MAP, SAI_SWITCH_ATTR_QOS_DOT1P_TO_COLOR_MAP,
        SAI_SWITCH_ATTR_QOS_TC_TO_QUEUE_MAP, SAI_SWITCH_ATTR_QOS_TC_AND_COLOR_TO_DSCP_MAP,
        SAI_SWITCH_ATTR_QOS_DEFAULT_TC,
    };

    for (auto id: switchAttrs)
    {
        sai_attribute_t a;

        if (getStoredAttr(SAI_OBJECT_TYPE_SWITCH, m_switch_id, id, a) &&
                (id == SAI_SWITCH_ATTR_QOS_DEFAULT_TC || a.value.oid != SAI_NULL_OBJECT_ID))
        {
            (void)qosHwSetSwitch(&a);
        }
    }

    static const sai_attr_id_t portAttrs[] = {
        SAI_PORT_ATTR_QOS_DSCP_TO_TC_MAP, SAI_PORT_ATTR_QOS_DSCP_TO_COLOR_MAP,
        SAI_PORT_ATTR_QOS_DOT1P_TO_TC_MAP, SAI_PORT_ATTR_QOS_DOT1P_TO_COLOR_MAP,
        SAI_PORT_ATTR_QOS_TC_TO_QUEUE_MAP, SAI_PORT_ATTR_QOS_TC_TO_PRIORITY_GROUP_MAP,
        SAI_PORT_ATTR_QOS_PFC_PRIORITY_TO_QUEUE_MAP, SAI_PORT_ATTR_QOS_PFC_PRIORITY_TO_PRIORITY_GROUP_MAP,
        SAI_PORT_ATTR_QOS_TC_AND_COLOR_TO_DSCP_MAP, SAI_PORT_ATTR_QOS_DEFAULT_TC,
        SAI_PORT_ATTR_QOS_SCHEDULER_PROFILE_ID, SAI_PORT_ATTR_PRIORITY_FLOW_CONTROL_MODE,
        SAI_PORT_ATTR_PRIORITY_FLOW_CONTROL, SAI_PORT_ATTR_PRIORITY_FLOW_CONTROL_RX,
        SAI_PORT_ATTR_PRIORITY_FLOW_CONTROL_TX,
    };

    for (auto port_id: m_port_list)
    {
        if (bcmPortOf(port_id) < 0)
        {
            continue;
        }

        for (auto id: portAttrs)
        {
            sai_attribute_t a;

            if (getStoredAttr(SAI_OBJECT_TYPE_PORT, port_id, id, a))
            {
                (void)qosHwSetPort(port_id, &a);
            }
        }
    }

    std::vector<sai_object_id_t> sgs, queues, pgs;

    for (auto &kv: m_qosSgHw) sgs.push_back(kv.first);
    for (auto &kv: m_qosQueueHw) queues.push_back(kv.first);
    for (auto &kv: m_qosPgHw) pgs.push_back(kv.first);

    for (auto sg: sgs)
    {
        sai_attribute_t a;

        if (getStoredAttr(SAI_OBJECT_TYPE_SCHEDULER_GROUP, sg, SAI_SCHEDULER_GROUP_ATTR_SCHEDULER_PROFILE_ID, a) &&
                a.value.oid != SAI_NULL_OBJECT_ID)
        {
            (void)hwSetSchedulerGroup(sg, &a);
        }
    }

    static const sai_attr_id_t queueAttrs[] = {
        SAI_QUEUE_ATTR_SCHEDULER_PROFILE_ID, SAI_QUEUE_ATTR_WRED_PROFILE_ID, SAI_QUEUE_ATTR_BUFFER_PROFILE_ID,
    };

    for (auto q: queues)
    {
        for (auto id: queueAttrs)
        {
            sai_attribute_t a;

            if (getStoredAttr(SAI_OBJECT_TYPE_QUEUE, q, id, a) && a.value.oid != SAI_NULL_OBJECT_ID)
            {
                (void)hwSetQueue(q, &a);
            }
        }
    }

    for (auto pg: pgs)
    {
        sai_attribute_t a;

        if (getStoredAttr(SAI_OBJECT_TYPE_INGRESS_PRIORITY_GROUP, pg, SAI_INGRESS_PRIORITY_GROUP_ATTR_BUFFER_PROFILE, a) &&
                a.value.oid != SAI_NULL_OBJECT_ID)
        {
            (void)hwSetIngressPriorityGroup(pg, &a);
        }
    }

    return SAI_STATUS_SUCCESS;
}

void SwitchBCM56860HW::qosOnPortMapped(
        _In_ sai_object_id_t port_id)
{
    SWSS_LOG_ENTER();

    if (!m_hwReady)
    {
        return;
    }

    hwbcm_qos_init();

    qosMapPort(port_id);

    // a new port inherits the switch-level maps / default TC

    (void)qosApplyPortAll(port_id);
}

void SwitchBCM56860HW::qosOnPortRemoved(
        _In_ sai_object_id_t port_id)
{
    SWSS_LOG_ENTER();

    qosUnmapPort(port_id);
}

// ---------------------------------------------------------------------------
// port / switch attributes
// ---------------------------------------------------------------------------

int SwitchBCM56860HW::qosSlotOfPortAttr(
        _In_ sai_attr_id_t id)
{
    SWSS_LOG_ENTER();

    switch (id)
    {
        case SAI_PORT_ATTR_QOS_DSCP_TO_TC_MAP:                    return QOS_SLOT_DSCP_TO_TC;
        case SAI_PORT_ATTR_QOS_DSCP_TO_COLOR_MAP:                 return QOS_SLOT_DSCP_TO_COLOR;
        case SAI_PORT_ATTR_QOS_DOT1P_TO_TC_MAP:                   return QOS_SLOT_DOT1P_TO_TC;
        case SAI_PORT_ATTR_QOS_DOT1P_TO_COLOR_MAP:                return QOS_SLOT_DOT1P_TO_COLOR;
        case SAI_PORT_ATTR_QOS_TC_TO_QUEUE_MAP:                   return QOS_SLOT_TC_TO_QUEUE;
        case SAI_PORT_ATTR_QOS_TC_TO_PRIORITY_GROUP_MAP:          return QOS_SLOT_TC_TO_PG;
        case SAI_PORT_ATTR_QOS_PFC_PRIORITY_TO_QUEUE_MAP:         return QOS_SLOT_PFC_TO_QUEUE;
        case SAI_PORT_ATTR_QOS_PFC_PRIORITY_TO_PRIORITY_GROUP_MAP: return QOS_SLOT_PFC_TO_PG;
        case SAI_PORT_ATTR_QOS_TC_AND_COLOR_TO_DSCP_MAP:          return QOS_SLOT_TC_COLOR_TO_DSCP;
        default:                                                  return -1;
    }
}

int SwitchBCM56860HW::qosSlotOfSwitchAttr(
        _In_ sai_attr_id_t id)
{
    SWSS_LOG_ENTER();

    switch (id)
    {
        case SAI_SWITCH_ATTR_QOS_DSCP_TO_TC_MAP:           return QOS_SLOT_DSCP_TO_TC;
        case SAI_SWITCH_ATTR_QOS_DSCP_TO_COLOR_MAP:        return QOS_SLOT_DSCP_TO_COLOR;
        case SAI_SWITCH_ATTR_QOS_DOT1P_TO_TC_MAP:          return QOS_SLOT_DOT1P_TO_TC;
        case SAI_SWITCH_ATTR_QOS_DOT1P_TO_COLOR_MAP:       return QOS_SLOT_DOT1P_TO_COLOR;
        case SAI_SWITCH_ATTR_QOS_TC_TO_QUEUE_MAP:          return QOS_SLOT_TC_TO_QUEUE;
        case SAI_SWITCH_ATTR_QOS_TC_AND_COLOR_TO_DSCP_MAP: return QOS_SLOT_TC_COLOR_TO_DSCP;
        default:                                           return -1;
    }
}

bool SwitchBCM56860HW::qosIsPortAttr(
        _In_ sai_attr_id_t id)
{
    SWSS_LOG_ENTER();

    if (qosSlotOfPortAttr(id) >= 0)
    {
        return true;
    }

    switch (id)
    {
        case SAI_PORT_ATTR_QOS_DEFAULT_TC:
        case SAI_PORT_ATTR_QOS_SCHEDULER_PROFILE_ID:
        case SAI_PORT_ATTR_PRIORITY_FLOW_CONTROL:
        case SAI_PORT_ATTR_PRIORITY_FLOW_CONTROL_MODE:
        case SAI_PORT_ATTR_PRIORITY_FLOW_CONTROL_RX:
        case SAI_PORT_ATTR_PRIORITY_FLOW_CONTROL_TX:
        case SAI_PORT_ATTR_QOS_INGRESS_BUFFER_PROFILE_LIST:
        case SAI_PORT_ATTR_QOS_EGRESS_BUFFER_PROFILE_LIST:
        case SAI_PORT_ATTR_QOS_TC_AND_COLOR_TO_DOT1P_MAP:
            return true;
        default:
            return false;
    }
}

bool SwitchBCM56860HW::qosIsSwitchAttr(
        _In_ sai_attr_id_t id)
{
    SWSS_LOG_ENTER();

    return qosSlotOfSwitchAttr(id) >= 0 || id == SAI_SWITCH_ATTR_QOS_DEFAULT_TC;
}

bool SwitchBCM56860HW::qosGetSwitchAttr(
        _Inout_ sai_attribute_t &attr) const
{
    SWSS_LOG_ENTER();

    if (attr.id != SAI_SWITCH_ATTR_TOTAL_BUFFER_SIZE)
    {
        return false;
    }

    uint64_t kb = 0;

    if (hwbcm_qos_total_buffer_kb_get(&kb) < 0)
    {
        return false;
    }

    attr.value.u64 = kb;
    return true;
}

bool SwitchBCM56860HW::qosGetBufferPoolAttr(
        _In_ sai_object_id_t pool_id,
        _Inout_ sai_attribute_t &attr) const
{
    SWSS_LOG_ENTER();

    if (attr.id != SAI_BUFFER_POOL_ATTR_SHARED_SIZE)
    {
        return false;
    }

    auto it = m_qosPools.find(pool_id);

    if (it == m_qosPools.end())
    {
        return false;
    }

    const QosPool &p = it->second;
    bool ingress = (p.type == SAI_BUFFER_POOL_TYPE_INGRESS || p.type == SAI_BUFFER_POOL_TYPE_BOTH);
    int sp = ingress ? p.ingressSp : p.egressSp;

    if (sp < 0)
    {
        return false;
    }

    uint32_t ing = 0, egr = 0;

    if (hwbcm_qos_pool_shared_get(sp, &ing, &egr) < 0)
    {
        return false;
    }

    attr.value.u64 = ingress ? (uint64_t)ing : (uint64_t)egr;
    return true;
}

SwitchBCM56860HW::QosPortState &SwitchBCM56860HW::qosPortState(
        _In_ sai_object_id_t port_id)
{
    SWSS_LOG_ENTER();

    return m_qosPort[port_id];
}

sai_object_id_t SwitchBCM56860HW::qosEffectiveMap(
        _In_ const QosPortState &ps,
        _In_ int slot) const
{
    SWSS_LOG_ENTER();

    if (slot < 0 || slot >= QOS_SLOT_COUNT)
    {
        return SAI_NULL_OBJECT_ID;
    }

    if (ps.maps[slot] != SAI_NULL_OBJECT_ID)
    {
        return ps.maps[slot];
    }

    return m_qosSwitchMaps[slot];
}

bool SwitchBCM56860HW::qosMapEntries(
        _In_ sai_object_id_t map_id,
        _Out_ std::vector<sai_qos_map_t> &out)
{
    SWSS_LOG_ENTER();

    out.clear();

    if (map_id == SAI_NULL_OBJECT_ID)
    {
        return false;
    }

    sai_attribute_t attr;

    if (!getStoredAttr(SAI_OBJECT_TYPE_QOS_MAP, map_id, SAI_QOS_MAP_ATTR_MAP_TO_VALUE_LIST, attr))
    {
        return true; // exists without entries
    }

    if (attr.value.qosmap.list != NULL)
    {
        out.assign(attr.value.qosmap.list, attr.value.qosmap.list + attr.value.qosmap.count);
    }

    return true;
}

sai_status_t SwitchBCM56860HW::qosHwSetPort(
        _In_ sai_object_id_t port_id,
        _In_ const sai_attribute_t *attr)
{
    SWSS_LOG_ENTER();

    if (attr == NULL)
    {
        return SAI_STATUS_INVALID_PARAMETER;
    }

    if (bcmPortOf(port_id) < 0)
    {
        return SAI_STATUS_SUCCESS; // CPU port / unmapped: bookkeeping only
    }

    auto &ps = qosPortState(port_id);

    int slot = qosSlotOfPortAttr(attr->id);

    if (slot >= 0)
    {
        ps.maps[slot] = attr->value.oid;

        return qosApplyPortSlot(port_id, slot);
    }

    switch (attr->id)
    {
        case SAI_PORT_ATTR_QOS_DEFAULT_TC:

            ps.defaultTc = (int)attr->value.u8;
            return qosApplyPortDefaultTc(port_id);

        case SAI_PORT_ATTR_QOS_SCHEDULER_PROFILE_ID:

            ps.scheduler = attr->value.oid;
            return qosApplyPortShaper(port_id);

        case SAI_PORT_ATTR_PRIORITY_FLOW_CONTROL:

            ps.pfcCombined = attr->value.u8;
            ps.pfcTouched = true;
            return qosApplyPortPfc(port_id);

        case SAI_PORT_ATTR_PRIORITY_FLOW_CONTROL_MODE:

            ps.pfcSeparate = (attr->value.s32 == SAI_PORT_PRIORITY_FLOW_CONTROL_MODE_SEPARATE);
            return ps.pfcTouched ? qosApplyPortPfc(port_id) : SAI_STATUS_SUCCESS;

        case SAI_PORT_ATTR_PRIORITY_FLOW_CONTROL_RX:

            ps.pfcRx = attr->value.u8;
            ps.pfcTouched = true;
            return qosApplyPortPfc(port_id);

        case SAI_PORT_ATTR_PRIORITY_FLOW_CONTROL_TX:

            ps.pfcTx = attr->value.u8;
            ps.pfcTouched = true;
            return qosApplyPortPfc(port_id);

        case SAI_PORT_ATTR_QOS_INGRESS_BUFFER_PROFILE_LIST:
        case SAI_PORT_ATTR_QOS_EGRESS_BUFFER_PROFILE_LIST:

            if (attr->value.objlist.count != 0)
            {
                SWSS_LOG_WARN("qos: port %s buffer profile list (port-SP limits) not programmed to hardware",
                        sai_serialize_object_id(port_id).c_str());
            }
            return SAI_STATUS_SUCCESS;

        case SAI_PORT_ATTR_QOS_TC_AND_COLOR_TO_DOT1P_MAP:

            if (attr->value.oid != SAI_NULL_OBJECT_ID)
            {
                SWSS_LOG_WARN("qos: TC_AND_COLOR_TO_DOT1P remark not programmed to hardware (port %s)",
                        sai_serialize_object_id(port_id).c_str());
            }
            return SAI_STATUS_SUCCESS;

        default:
            return SAI_STATUS_SUCCESS;
    }
}

sai_status_t SwitchBCM56860HW::qosHwSetSwitch(
        _In_ const sai_attribute_t *attr)
{
    SWSS_LOG_ENTER();

    if (attr == NULL)
    {
        return SAI_STATUS_INVALID_PARAMETER;
    }

    std::vector<sai_object_id_t> ports;

    {
        std::lock_guard<std::mutex> lock(m_mapMutex);

        for (auto &kv: m_portToBcm)
        {
            ports.push_back(kv.first);
        }
    }

    sai_status_t status = SAI_STATUS_SUCCESS;

    if (attr->id == SAI_SWITCH_ATTR_QOS_DEFAULT_TC)
    {
        m_qosSwitchDefaultTc = (int)attr->value.u8;

        for (auto p: ports)
        {
            if (qosPortState(p).defaultTc < 0 && qosApplyPortDefaultTc(p) != SAI_STATUS_SUCCESS)
            {
                status = SAI_STATUS_FAILURE;
            }
        }

        return status;
    }

    int slot = qosSlotOfSwitchAttr(attr->id);

    if (slot < 0)
    {
        return SAI_STATUS_SUCCESS;
    }

    m_qosSwitchMaps[slot] = attr->value.oid;

    for (auto p: ports)
    {
        if (qosPortState(p).maps[slot] == SAI_NULL_OBJECT_ID &&
                qosApplyPortSlot(p, slot) != SAI_STATUS_SUCCESS)
        {
            status = SAI_STATUS_FAILURE;
        }
    }

    return status;
}

sai_status_t SwitchBCM56860HW::qosApplyPortSlot(
        _In_ sai_object_id_t port_id,
        _In_ int slot)
{
    SWSS_LOG_ENTER();

    switch (slot)
    {
        case QOS_SLOT_DSCP_TO_TC:
        case QOS_SLOT_DSCP_TO_COLOR:
        case QOS_SLOT_DOT1P_TO_TC:
        case QOS_SLOT_DOT1P_TO_COLOR:
            return qosApplyPortIngressMaps(port_id);

        case QOS_SLOT_TC_TO_QUEUE:
            return qosApplyPortTcToQueue(port_id);

        case QOS_SLOT_TC_TO_PG:
            return qosApplyPortTcToPg(port_id);

        case QOS_SLOT_PFC_TO_QUEUE:
            return qosPortState(port_id).pfcTouched ? qosApplyPortPfc(port_id) : SAI_STATUS_SUCCESS;

        case QOS_SLOT_PFC_TO_PG:
            {
                // TD2+ sends PFC for the priority, the PG only decides when
                // (xoff). A non-identity PFC priority -> PG map has no HW knob.
                std::vector<sai_qos_map_t> e;

                qosMapEntries(qosEffectiveMap(qosPortState(port_id), slot), e);

                for (auto &m: e)
                {
                    if (m.key.prio != m.value.pg)
                    {
                        SWSS_LOG_WARN("qos: PFC priority %u -> PG %u: only identity is supported on TD2+ (port %s)",
                                (unsigned)m.key.prio, (unsigned)m.value.pg, sai_serialize_object_id(port_id).c_str());
                    }
                }

                return SAI_STATUS_SUCCESS;
            }

        case QOS_SLOT_TC_COLOR_TO_DSCP:
            return qosApplyPortEgressMap(port_id);

        default:
            return SAI_STATUS_SUCCESS;
    }
}

sai_status_t SwitchBCM56860HW::qosApplyPortAll(
        _In_ sai_object_id_t port_id)
{
    SWSS_LOG_ENTER();

    auto &ps = qosPortState(port_id);

    sai_status_t status = SAI_STATUS_SUCCESS;

    auto upd = [&](sai_status_t s) { if (s != SAI_STATUS_SUCCESS) status = s; };

    if (qosEffectiveMap(ps, QOS_SLOT_DSCP_TO_TC) || qosEffectiveMap(ps, QOS_SLOT_DSCP_TO_COLOR) ||
            qosEffectiveMap(ps, QOS_SLOT_DOT1P_TO_TC) || qosEffectiveMap(ps, QOS_SLOT_DOT1P_TO_COLOR))
    {
        upd(qosApplyPortIngressMaps(port_id));
    }

    if (qosEffectiveMap(ps, QOS_SLOT_TC_TO_QUEUE))
    {
        upd(qosApplyPortTcToQueue(port_id));
    }

    if (qosEffectiveMap(ps, QOS_SLOT_TC_TO_PG))
    {
        upd(qosApplyPortTcToPg(port_id));
    }

    if (qosEffectiveMap(ps, QOS_SLOT_TC_COLOR_TO_DSCP))
    {
        upd(qosApplyPortEgressMap(port_id));
    }

    if (ps.defaultTc >= 0 || m_qosSwitchDefaultTc >= 0)
    {
        upd(qosApplyPortDefaultTc(port_id));
    }

    if (ps.pfcTouched)
    {
        upd(qosApplyPortPfc(port_id));
    }

    if (ps.scheduler != SAI_NULL_OBJECT_ID)
    {
        upd(qosApplyPortShaper(port_id));
    }

    return status;
}

// ---------------------------------------------------------------------------
// bcm qos map cache
// ---------------------------------------------------------------------------

int SwitchBCM56860HW::qosHwMapAcquire(
        _In_ const std::string &key)
{
    SWSS_LOG_ENTER();

    if (key.empty())
    {
        return -1;
    }

    auto it = m_qosHwMaps.find(key);

    if (it != m_qosHwMaps.end())
    {
        it->second.refs++;

        return it->second.id;
    }

    const uint8_t *p = reinterpret_cast<const uint8_t*>(key.data()) + 1;

    int id = -1;
    int rv = -1;

    switch (key[0])
    {
        case 'D':
            rv = (key.size() == 1 + 128) ? hwbcm_qos_dscp_map_create(p, p + 64, &id) : -1;
            break;
        case 'P':
            rv = (key.size() == 1 + 16) ? hwbcm_qos_dot1p_map_create(p, p + 8, &id) : -1;
            break;
        case 'E':
            rv = (key.size() == 1 + HWBCM_QOS_NUM_TC * HWBCM_QOS_NUM_COLOR) ? hwbcm_qos_egr_dscp_map_create(p, &id) : -1;
            break;
        default:
            break;
    }

    if (rv < 0)
    {
        SWSS_LOG_ERROR("qos: creating bcm qos map of kind %c failed: %d", key[0], rv);

        return -1;
    }

    QosHwMap m;

    m.id = id;
    m.refs = 1;

    m_qosHwMaps[key] = m;

    return id;
}

void SwitchBCM56860HW::qosHwMapRelease(
        _In_ const std::string &key)
{
    SWSS_LOG_ENTER();

    if (key.empty())
    {
        return;
    }

    auto it = m_qosHwMaps.find(key);

    if (it == m_qosHwMaps.end())
    {
        return;
    }

    if (--it->second.refs > 0)
    {
        return;
    }

    int rv = hwbcm_qos_map_destroy(it->second.id);

    if (rv < 0)
    {
        SWSS_LOG_WARN("qos: bcm qos map 0x%x destroy failed: %d", it->second.id, rv);
    }

    m_qosHwMaps.erase(it);
}

// ---------------------------------------------------------------------------
// per-port programming
// ---------------------------------------------------------------------------

sai_status_t SwitchBCM56860HW::qosApplyPortIngressMaps(
        _In_ sai_object_id_t port_id)
{
    SWSS_LOG_ENTER();

    int bcm = bcmPortOf(port_id);

    if (bcm < 0)
    {
        return SAI_STATUS_SUCCESS;
    }

    auto &ps = qosPortState(port_id);

    std::vector<sai_qos_map_t> e;

    // DSCP -> (tc, color)

    std::string dscpKey;

    sai_object_id_t dscpTc = qosEffectiveMap(ps, QOS_SLOT_DSCP_TO_TC);
    sai_object_id_t dscpColor = qosEffectiveMap(ps, QOS_SLOT_DSCP_TO_COLOR);

    if (dscpTc != SAI_NULL_OBJECT_ID || dscpColor != SAI_NULL_OBJECT_ID)
    {
        uint8_t tc[64] = {0};
        uint8_t color[64] = {0};

        qosMapEntries(dscpTc, e);

        for (auto &m: e)
        {
            if (m.key.dscp < 64)
            {
                tc[m.key.dscp] = (uint8_t)(m.value.tc & 0xf);
            }
        }

        qosMapEntries(dscpColor, e);

        for (auto &m: e)
        {
            if (m.key.dscp < 64)
            {
                color[m.key.dscp] = (uint8_t)m.value.color;
            }
        }

        dscpKey = "D";
        dscpKey.append(reinterpret_cast<const char*>(tc), sizeof(tc));
        dscpKey.append(reinterpret_cast<const char*>(color), sizeof(color));
    }

    // 802.1p -> (tc, color)

    std::string dot1pKey;

    sai_object_id_t dot1pTc = qosEffectiveMap(ps, QOS_SLOT_DOT1P_TO_TC);
    sai_object_id_t dot1pColor = qosEffectiveMap(ps, QOS_SLOT_DOT1P_TO_COLOR);

    if (dot1pTc != SAI_NULL_OBJECT_ID || dot1pColor != SAI_NULL_OBJECT_ID)
    {
        uint8_t tc[8] = {0};
        uint8_t color[8] = {0};

        qosMapEntries(dot1pTc, e);

        for (auto &m: e)
        {
            if (m.key.dot1p < 8)
            {
                tc[m.key.dot1p] = (uint8_t)(m.value.tc & 0xf);
            }
        }

        qosMapEntries(dot1pColor, e);

        for (auto &m: e)
        {
            if (m.key.dot1p < 8)
            {
                color[m.key.dot1p] = (uint8_t)m.value.color;
            }
        }

        dot1pKey = "P";
        dot1pKey.append(reinterpret_cast<const char*>(tc), sizeof(tc));
        dot1pKey.append(reinterpret_cast<const char*>(color), sizeof(color));
    }

    if (dscpKey == ps.hwIngDscpKey && dot1pKey == ps.hwIngDot1pKey)
    {
        return SAI_STATUS_SUCCESS;
    }

    int dscpId = qosHwMapAcquire(dscpKey);
    int dot1pId = qosHwMapAcquire(dot1pKey);

    if ((!dscpKey.empty() && dscpId < 0) || (!dot1pKey.empty() && dot1pId < 0))
    {
        if (dscpId >= 0) qosHwMapRelease(dscpKey);
        if (dot1pId >= 0) qosHwMapRelease(dot1pKey);

        return SAI_STATUS_INSUFFICIENT_RESOURCES;
    }

    int rv = hwbcm_qos_port_ingress_maps_set(bcm, dscpId, dot1pId);

    if (rv < 0)
    {
        SWSS_LOG_ERROR("qos: binding ingress maps to bcm port %d failed: %d", bcm, rv);

        qosHwMapRelease(dscpKey);
        qosHwMapRelease(dot1pKey);

        return SAI_STATUS_FAILURE;
    }

    qosHwMapRelease(ps.hwIngDscpKey);
    qosHwMapRelease(ps.hwIngDot1pKey);

    ps.hwIngDscpKey = dscpKey;
    ps.hwIngDot1pKey = dot1pKey;

    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchBCM56860HW::qosApplyPortEgressMap(
        _In_ sai_object_id_t port_id)
{
    SWSS_LOG_ENTER();

    int bcm = bcmPortOf(port_id);

    if (bcm < 0)
    {
        return SAI_STATUS_SUCCESS;
    }

    auto &ps = qosPortState(port_id);

    std::string key;

    sai_object_id_t mapId = qosEffectiveMap(ps, QOS_SLOT_TC_COLOR_TO_DSCP);

    if (mapId != SAI_NULL_OBJECT_ID)
    {
        const size_t n = HWBCM_QOS_NUM_TC * HWBCM_QOS_NUM_COLOR;

        uint8_t dscp[HWBCM_QOS_NUM_TC * HWBCM_QOS_NUM_COLOR] = {0};
        bool explicitSet[HWBCM_QOS_NUM_TC * HWBCM_QOS_NUM_COLOR] = {false};

        std::vector<sai_qos_map_t> e;

        qosMapEntries(mapId, e);

        for (auto &m: e)
        {
            size_t c = (size_t)m.key.color;

            if (m.key.tc < HWBCM_QOS_NUM_TC && c < HWBCM_QOS_NUM_COLOR)
            {
                size_t idx = (size_t)m.key.tc * HWBCM_QOS_NUM_COLOR + c;

                dscp[idx] = (uint8_t)(m.value.dscp & 0x3f);
                explicitSet[idx] = true;
            }
        }

        // SONiC TC_TO_DSCP maps key only the TC (color = green): let yellow
        // and red follow green unless given explicitly.
        for (size_t tc = 0; tc < HWBCM_QOS_NUM_TC; tc++)
        {
            size_t g = tc * HWBCM_QOS_NUM_COLOR;

            if (!explicitSet[g])
            {
                continue;
            }

            for (size_t c = 1; c < HWBCM_QOS_NUM_COLOR; c++)
            {
                if (!explicitSet[g + c])
                {
                    dscp[g + c] = dscp[g];
                }
            }
        }

        key = "E";
        key.append(reinterpret_cast<const char*>(dscp), n);
    }

    if (key == ps.hwEgrDscpKey)
    {
        return SAI_STATUS_SUCCESS;
    }

    int id = qosHwMapAcquire(key);

    if (!key.empty() && id < 0)
    {
        return SAI_STATUS_INSUFFICIENT_RESOURCES;
    }

    int rv = hwbcm_qos_port_egress_map_set(bcm, id);

    if (rv < 0)
    {
        SWSS_LOG_ERROR("qos: binding egress DSCP map to bcm port %d failed: %d", bcm, rv);

        qosHwMapRelease(key);

        return SAI_STATUS_FAILURE;
    }

    qosHwMapRelease(ps.hwEgrDscpKey);

    ps.hwEgrDscpKey = key;

    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchBCM56860HW::qosApplyPortTcToQueue(
        _In_ sai_object_id_t port_id)
{
    SWSS_LOG_ENTER();

    int bcm = bcmPortOf(port_id);

    if (bcm < 0)
    {
        return SAI_STATUS_SUCCESS;
    }

    uint8_t cos[HWBCM_QOS_NUM_TC];

    // unbound: identity for 0..7, the rest on the last reachable cos
    for (int p = 0; p < HWBCM_QOS_NUM_TC; p++)
    {
        cos[p] = (uint8_t)std::min(p, HWBCM_QOS_NUM_COS - 1);
    }

    std::vector<sai_qos_map_t> e;

    qosMapEntries(qosEffectiveMap(qosPortState(port_id), QOS_SLOT_TC_TO_QUEUE), e);

    for (auto &m: e)
    {
        if (m.key.tc < HWBCM_QOS_NUM_TC)
        {
            cos[m.key.tc] = m.value.queue_index;
        }
    }

    int rv = hwbcm_qos_port_tc_to_cos_set(bcm, cos);

    if (rv < 0)
    {
        SWSS_LOG_ERROR("qos: TC_TO_QUEUE on bcm port %d failed: %d", bcm, rv);

        return SAI_STATUS_FAILURE;
    }

    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchBCM56860HW::qosApplyPortTcToPg(
        _In_ sai_object_id_t port_id)
{
    SWSS_LOG_ENTER();

    int bcm = bcmPortOf(port_id);

    if (bcm < 0)
    {
        return SAI_STATUS_SUCCESS;
    }

    uint8_t pg[HWBCM_QOS_NUM_TC];

    // unbound: init-mmu default, every priority in PG7 (soc/esw/trident2.c:14180)
    for (int p = 0; p < HWBCM_QOS_NUM_TC; p++)
    {
        pg[p] = 7;
    }

    std::vector<sai_qos_map_t> e;

    qosMapEntries(qosEffectiveMap(qosPortState(port_id), QOS_SLOT_TC_TO_PG), e);

    for (auto &m: e)
    {
        if (m.key.tc < HWBCM_QOS_NUM_TC)
        {
            pg[m.key.tc] = m.value.pg;
        }
    }

    int rv = hwbcm_qos_port_tc_to_pg_set(bcm, pg);

    if (rv < 0)
    {
        SWSS_LOG_ERROR("qos: TC_TO_PRIORITY_GROUP on bcm port %d failed: %d", bcm, rv);

        return SAI_STATUS_FAILURE;
    }

    qosRetryPendingPgs(port_id);

    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchBCM56860HW::qosApplyPortPfc(
        _In_ sai_object_id_t port_id)
{
    SWSS_LOG_ENTER();

    int bcm = bcmPortOf(port_id);

    if (bcm < 0)
    {
        return SAI_STATUS_SUCCESS;
    }

    auto &ps = qosPortState(port_id);

    uint8_t tx = ps.pfcSeparate ? ps.pfcTx : ps.pfcCombined;
    uint8_t rx = ps.pfcSeparate ? ps.pfcRx : ps.pfcCombined;

    uint8_t p2c[HWBCM_QOS_NUM_PFC];

    for (int p = 0; p < HWBCM_QOS_NUM_PFC; p++)
    {
        p2c[p] = (uint8_t)p;
    }

    std::vector<sai_qos_map_t> e;

    qosMapEntries(qosEffectiveMap(ps, QOS_SLOT_PFC_TO_QUEUE), e);

    for (auto &m: e)
    {
        if (m.key.prio < HWBCM_QOS_NUM_PFC)
        {
            p2c[m.key.prio] = m.value.queue_index;
        }
    }

    int rv = hwbcm_qos_port_pfc_set(bcm, tx, rx, p2c);

    if (rv < 0)
    {
        SWSS_LOG_ERROR("qos: PFC tx 0x%02x rx 0x%02x on bcm port %d failed: %d", tx, rx, bcm, rv);

        return SAI_STATUS_FAILURE;
    }

    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchBCM56860HW::qosApplyPortDefaultTc(
        _In_ sai_object_id_t port_id)
{
    SWSS_LOG_ENTER();

    int bcm = bcmPortOf(port_id);

    if (bcm < 0)
    {
        return SAI_STATUS_SUCCESS;
    }

    auto &ps = qosPortState(port_id);

    int tc = ps.defaultTc >= 0 ? ps.defaultTc : m_qosSwitchDefaultTc;

    if (tc < 0)
    {
        return SAI_STATUS_SUCCESS;
    }

    int rv = hwbcm_qos_port_default_tc_set(bcm, tc);

    if (rv < 0)
    {
        SWSS_LOG_ERROR("qos: default TC %d on bcm port %d failed: %d", tc, bcm, rv);

        return SAI_STATUS_FAILURE;
    }

    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchBCM56860HW::qosApplyPortEffectiveShaper(
        _In_ sai_object_id_t port_id)
{
    SWSS_LOG_ENTER();

    int bcm = bcmPortOf(port_id);

    if (bcm < 0)
    {
        return SAI_STATUS_SUCCESS;
    }

    auto &ps = qosPortState(port_id);
    sai_object_id_t sched = ps.scheduler;

    if (sched == SAI_NULL_OBJECT_ID)
    {
        // Fall back to any scheduler bound on the port's root SG (level 0).
        for (const auto &kv: m_qosSgHw)
        {
            if (kv.second.port == port_id && kv.second.level == 0)
            {
                auto sit = m_qosSgSched.find(kv.first);
                if (sit != m_qosSgSched.end() && sit->second != SAI_NULL_OBJECT_ID)
                {
                    sched = sit->second;
                    break;
                }
            }
        }
    }

    QosSched s;

    hwbcm_qos_sched_t hw;

    memset(&hw, 0, sizeof(hw));

    bool have = sched != SAI_NULL_OBJECT_ID && qosDecodeScheduler(sched, s);

    if (have)
    {
        hw.pps = s.pps ? 1 : 0;
        hw.max_rate = s.maxRate;
        hw.max_burst = s.maxBurst;

        if (s.minRate != 0)
        {
            SWSS_LOG_WARN("qos: port %s scheduler min bandwidth has no port-level HW equivalent, ignored",
                    sai_serialize_object_id(port_id).c_str());
        }
    }

    int rv = hwbcm_qos_port_shaper_set(bcm, have ? &hw : NULL);

    if (rv < 0)
    {
        SWSS_LOG_ERROR("qos: port shaper on bcm port %d failed: %d", bcm, rv);

        return SAI_STATUS_FAILURE;
    }

    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchBCM56860HW::qosApplyPortShaper(
        _In_ sai_object_id_t port_id)
{
    SWSS_LOG_ENTER();

    return qosApplyPortEffectiveShaper(port_id);
}

// ---------------------------------------------------------------------------
// QOS_MAP
// ---------------------------------------------------------------------------

sai_status_t SwitchBCM56860HW::hwCreateQosMap(
        _In_ sai_object_id_t map_id,
        _In_ uint32_t attr_count,
        _In_ const sai_attribute_t *attr_list)
{
    SWSS_LOG_ENTER();

    auto *t = findAttr(attr_count, attr_list, SAI_QOS_MAP_ATTR_TYPE);

    m_qosMapType[map_id] = t ? t->value.s32 : -1;

    switch (m_qosMapType[map_id])
    {
        case SAI_QOS_MAP_TYPE_DOT1P_TO_TC:
        case SAI_QOS_MAP_TYPE_DOT1P_TO_COLOR:
        case SAI_QOS_MAP_TYPE_DSCP_TO_TC:
        case SAI_QOS_MAP_TYPE_DSCP_TO_COLOR:
        case SAI_QOS_MAP_TYPE_TC_TO_QUEUE:
        case SAI_QOS_MAP_TYPE_TC_AND_COLOR_TO_DSCP:
        case SAI_QOS_MAP_TYPE_TC_TO_PRIORITY_GROUP:
        case SAI_QOS_MAP_TYPE_PFC_PRIORITY_TO_PRIORITY_GROUP:
        case SAI_QOS_MAP_TYPE_PFC_PRIORITY_TO_QUEUE:
            break;

        default:
            SWSS_LOG_WARN("qos: map %s of type %d is not programmed to hardware (bookkeeping only)",
                    sai_serialize_object_id(map_id).c_str(), m_qosMapType[map_id]);
            break;
    }

    // Programmed when bound to a port (content is per port after the
    // switch-level fallback, so the bcm map is created on bind).
    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchBCM56860HW::hwSetQosMap(
        _In_ sai_object_id_t map_id,
        _In_ const sai_attribute_t *attr)
{
    SWSS_LOG_ENTER();

    if (attr == NULL || attr->id != SAI_QOS_MAP_ATTR_MAP_TO_VALUE_LIST)
    {
        return SAI_STATUS_SUCCESS;
    }

    sai_status_t status = SAI_STATUS_SUCCESS;

    std::vector<std::pair<sai_object_id_t, int>> todo;

    for (auto &kv: m_qosPort)
    {
        for (int slot = 0; slot < QOS_SLOT_COUNT; slot++)
        {
            if (qosEffectiveMap(kv.second, slot) == map_id)
            {
                todo.emplace_back(kv.first, slot);
            }
        }
    }

    for (auto &t: todo)
    {
        if (qosApplyPortSlot(t.first, t.second) != SAI_STATUS_SUCCESS)
        {
            status = SAI_STATUS_FAILURE;
        }
    }

    return status;
}

sai_status_t SwitchBCM56860HW::hwRemoveQosMap(
        _In_ sai_object_id_t map_id)
{
    SWSS_LOG_ENTER();

    for (auto &kv: m_qosPort)
    {
        for (int slot = 0; slot < QOS_SLOT_COUNT; slot++)
        {
            if (kv.second.maps[slot] == map_id)
            {
                SWSS_LOG_WARN("qos: map %s removed while still bound to port %s",
                        sai_serialize_object_id(map_id).c_str(),
                        sai_serialize_object_id(kv.first).c_str());
            }
        }
    }

    m_qosMapType.erase(map_id);

    return SAI_STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// SCHEDULER / SCHEDULER_GROUP
// ---------------------------------------------------------------------------

bool SwitchBCM56860HW::qosDecodeScheduler(
        _In_ sai_object_id_t sched_id,
        _Out_ QosSched &out)
{
    SWSS_LOG_ENTER();

    out = QosSched();

    if (sched_id == SAI_NULL_OBJECT_ID || m_qosSchedulers.find(sched_id) == m_qosSchedulers.end())
    {
        return false;
    }

    sai_attribute_t a;

    if (getStoredAttr(SAI_OBJECT_TYPE_SCHEDULER, sched_id, SAI_SCHEDULER_ATTR_SCHEDULING_TYPE, a))
    {
        switch (a.value.s32)
        {
            case SAI_SCHEDULING_TYPE_STRICT: out.mode = HWBCM_QOS_SCHED_STRICT; break;
            case SAI_SCHEDULING_TYPE_DWRR:   out.mode = HWBCM_QOS_SCHED_DWRR; break;
            default:                         out.mode = HWBCM_QOS_SCHED_WRR; break;
        }
    }

    if (getStoredAttr(SAI_OBJECT_TYPE_SCHEDULER, sched_id, SAI_SCHEDULER_ATTR_SCHEDULING_WEIGHT, a))
    {
        out.weight = (int)a.value.u8;
    }

    if (getStoredAttr(SAI_OBJECT_TYPE_SCHEDULER, sched_id, SAI_SCHEDULER_ATTR_METER_TYPE, a))
    {
        out.pps = (a.value.s32 == SAI_METER_TYPE_PACKETS);
    }

    if (getStoredAttr(SAI_OBJECT_TYPE_SCHEDULER, sched_id, SAI_SCHEDULER_ATTR_MIN_BANDWIDTH_RATE, a))
    {
        out.minRate = a.value.u64;
    }

    if (getStoredAttr(SAI_OBJECT_TYPE_SCHEDULER, sched_id, SAI_SCHEDULER_ATTR_MIN_BANDWIDTH_BURST_RATE, a))
    {
        out.minBurst = a.value.u64;
    }

    if (getStoredAttr(SAI_OBJECT_TYPE_SCHEDULER, sched_id, SAI_SCHEDULER_ATTR_MAX_BANDWIDTH_RATE, a))
    {
        out.maxRate = a.value.u64;
    }

    if (getStoredAttr(SAI_OBJECT_TYPE_SCHEDULER, sched_id, SAI_SCHEDULER_ATTR_MAX_BANDWIDTH_BURST_RATE, a))
    {
        out.maxBurst = a.value.u64;
    }

    return true;
}

sai_status_t SwitchBCM56860HW::hwCreateScheduler(
        _In_ sai_object_id_t sched_id,
        _In_ uint32_t attr_count,
        _In_ const sai_attribute_t *attr_list)
{
    SWSS_LOG_ENTER();

    m_qosSchedulers.insert(sched_id);

    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchBCM56860HW::hwSetScheduler(
        _In_ sai_object_id_t sched_id,
        _In_ const sai_attribute_t *attr)
{
    SWSS_LOG_ENTER();

    m_qosSchedulers.insert(sched_id);

    sai_status_t status = SAI_STATUS_SUCCESS;

    std::vector<sai_object_id_t> sgs, queues, ports;

    for (auto &kv: m_qosSgSched) if (kv.second == sched_id) sgs.push_back(kv.first);
    for (auto &kv: m_qosQueueSched) if (kv.second == sched_id) queues.push_back(kv.first);
    for (auto &kv: m_qosPort) if (kv.second.scheduler == sched_id) ports.push_back(kv.first);

    for (auto sg: sgs)
    {
        if (qosApplySgScheduler(sg) != SAI_STATUS_SUCCESS) status = SAI_STATUS_FAILURE;
    }

    for (auto q: queues)
    {
        if (qosApplyQueueScheduler(q) != SAI_STATUS_SUCCESS) status = SAI_STATUS_FAILURE;
    }

    for (auto p: ports)
    {
        if (qosApplyPortShaper(p) != SAI_STATUS_SUCCESS) status = SAI_STATUS_FAILURE;
    }

    return status;
}

sai_status_t SwitchBCM56860HW::hwRemoveScheduler(
        _In_ sai_object_id_t sched_id)
{
    SWSS_LOG_ENTER();

    std::vector<sai_object_id_t> sgs, queues, ports;

    for (auto it = m_qosSgSched.begin(); it != m_qosSgSched.end();)
    {
        if (it->second == sched_id)
        {
            sgs.push_back(it->first);
            it = m_qosSgSched.erase(it);
        }
        else
        {
            ++it;
        }
    }

    for (auto it = m_qosQueueSched.begin(); it != m_qosQueueSched.end();)
    {
        if (it->second == sched_id)
        {
            queues.push_back(it->first);
            it = m_qosQueueSched.erase(it);
        }
        else
        {
            ++it;
        }
    }

    for (auto &kv: m_qosPort)
    {
        if (kv.second.scheduler == sched_id)
        {
            kv.second.scheduler = SAI_NULL_OBJECT_ID;
            ports.push_back(kv.first);
        }
    }

    m_qosSchedulers.erase(sched_id);

    if (!sgs.empty() || !queues.empty() || !ports.empty())
    {
        SWSS_LOG_WARN("qos: scheduler %s removed while still bound (sgs=%zu queues=%zu ports=%zu); restoring defaults",
                sai_serialize_object_id(sched_id).c_str(), sgs.size(), queues.size(), ports.size());

        for (auto sg: sgs) (void)qosApplySgScheduler(sg);
        for (auto q: queues) (void)qosApplyQueueScheduler(q);
        for (auto p: ports) (void)qosApplyPortShaper(p);
    }

    return SAI_STATUS_SUCCESS;
}

/* Program L1 node `cos` of a port from a scheduler oid (NULL = SDK default). */
#define QOS_TO_HW_SCHED(q, hw)                     \
    do {                                           \
        memset(&(hw), 0, sizeof(hw));              \
        (hw).mode = (q).mode;                      \
        (hw).weight = (q).weight;                  \
        (hw).pps = (q).pps ? 1 : 0;                \
        (hw).min_rate = (q).minRate;               \
        (hw).max_rate = (q).maxRate;               \
        (hw).min_burst = (q).minBurst;             \
        (hw).max_burst = (q).maxBurst;             \
    } while (0)

sai_status_t SwitchBCM56860HW::qosApplyCosScheduler(
        _In_ sai_object_id_t port_id,
        _In_ int cos)
{
    SWSS_LOG_ENTER();

    int bcm = bcmPortOf(port_id);

    if (bcm < 0)
    {
        return SAI_STATUS_SUCCESS;
    }

    if (cos < 0 || cos >= HWBCM_QOS_NUM_COS)
    {
        SWSS_LOG_WARN("qos: scheduler on cos %d of bcm port %d not reachable in non-ETS mode, ignored",
                cos, bcm);
        return SAI_STATUS_SUCCESS;
    }

    // Effective scheduler for L1 node (port, cos): queue-level binding takes
    // precedence (UC over MC if both are set), else L1 SG binding, else NULL.
    sai_object_id_t sched = SAI_NULL_OBJECT_ID;
    bool haveUc = false;

    for (const auto &kv: m_qosQueueHw)
    {
        if (kv.second.port != port_id || kv.second.cpu || kv.second.cos != cos)
        {
            continue;
        }
        auto sit = m_qosQueueSched.find(kv.first);
        if (sit == m_qosQueueSched.end() || sit->second == SAI_NULL_OBJECT_ID)
        {
            continue;
        }
        if (!kv.second.mcast || !haveUc)
        {
            sched = sit->second;
            if (!kv.second.mcast)
            {
                haveUc = true;
            }
        }
    }

    if (sched == SAI_NULL_OBJECT_ID)
    {
        for (const auto &kv: m_qosSgHw)
        {
            if (kv.second.port == port_id && kv.second.level == 2 && kv.second.index == cos)
            {
                auto sit = m_qosSgSched.find(kv.first);
                if (sit != m_qosSgSched.end() && sit->second != SAI_NULL_OBJECT_ID)
                {
                    sched = sit->second;
                    break;
                }
            }
        }
    }

    QosSched s;

    bool have = qosDecodeScheduler(sched, s);

    hwbcm_qos_sched_t hw;

    QOS_TO_HW_SCHED(s, hw);

    int rv = hwbcm_qos_cos_sched_set(bcm, cos, have ? &hw : NULL);

    if (rv < 0)
    {
        SWSS_LOG_ERROR("qos: scheduler %s on bcm port %d cos %d failed: %d",
                sai_serialize_object_id(sched).c_str(), bcm, cos, rv);

        return SAI_STATUS_FAILURE;
    }

    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchBCM56860HW::qosApplySgScheduler(
        _In_ sai_object_id_t sg_id)
{
    SWSS_LOG_ENTER();

    auto it = m_qosSgHw.find(sg_id);

    if (it == m_qosSgHw.end() || it->second.bcmPort < 0)
    {
        return SAI_STATUS_SUCCESS;
    }

    const QosSgHw &h = it->second;

    switch (h.level)
    {
        case 2:
            return qosApplyCosScheduler(h.port, h.index);

        case 0:
            // root of the port tree: only a max shaper means something
            return qosApplyPortEffectiveShaper(h.port);

        default:
            if (m_qosSgSched.find(sg_id) != m_qosSgSched.end())
            {
                SWSS_LOG_WARN("qos: scheduler on L0 group %d of bcm port %d has no HW node in non-ETS mode, ignored",
                        h.index, h.bcmPort);
            }
            return SAI_STATUS_SUCCESS;
    }
}

sai_status_t SwitchBCM56860HW::qosApplyQueueScheduler(
        _In_ sai_object_id_t queue_id)
{
    SWSS_LOG_ENTER();

    auto it = m_qosQueueHw.find(queue_id);

    if (it == m_qosQueueHw.end() || it->second.cpu || it->second.bcmPort < 0)
    {
        return SAI_STATUS_SUCCESS;
    }

    // UC and MC queue of a cos share the L1 node: resolve effective scheduler.
    return qosApplyCosScheduler(it->second.port, it->second.cos);
}

sai_status_t SwitchBCM56860HW::hwSetSchedulerGroup(
        _In_ sai_object_id_t sg_id,
        _In_ const sai_attribute_t *attr)
{
    SWSS_LOG_ENTER();

    if (attr == NULL || attr->id != SAI_SCHEDULER_GROUP_ATTR_SCHEDULER_PROFILE_ID)
    {
        return SAI_STATUS_SUCCESS; // tree bookkeeping (CHILD_LIST etc.)
    }

    if (attr->value.oid == SAI_NULL_OBJECT_ID)
    {
        m_qosSgSched.erase(sg_id);
    }
    else
    {
        m_qosSgSched[sg_id] = attr->value.oid;
    }

    return qosApplySgScheduler(sg_id);
}

// ---------------------------------------------------------------------------
// WRED
// ---------------------------------------------------------------------------

sai_status_t SwitchBCM56860HW::hwCreateWred(
        _In_ sai_object_id_t wred_id,
        _In_ uint32_t attr_count,
        _In_ const sai_attribute_t *attr_list)
{
    SWSS_LOG_ENTER();

    m_qosWreds.insert(wred_id);

    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchBCM56860HW::hwSetWred(
        _In_ sai_object_id_t wred_id,
        _In_ const sai_attribute_t *attr)
{
    SWSS_LOG_ENTER();

    m_qosWreds.insert(wred_id);

    std::vector<sai_object_id_t> queues;

    for (auto &kv: m_qosQueueWred) if (kv.second == wred_id) queues.push_back(kv.first);

    sai_status_t status = SAI_STATUS_SUCCESS;

    for (auto q: queues)
    {
        if (qosApplyQueueWred(q) != SAI_STATUS_SUCCESS) status = SAI_STATUS_FAILURE;
    }

    return status;
}

sai_status_t SwitchBCM56860HW::hwRemoveWred(
        _In_ sai_object_id_t wred_id)
{
    SWSS_LOG_ENTER();

    std::vector<sai_object_id_t> queues;

    for (auto it = m_qosQueueWred.begin(); it != m_qosQueueWred.end();)
    {
        if (it->second == wred_id)
        {
            queues.push_back(it->first);
            it = m_qosQueueWred.erase(it);
        }
        else
        {
            ++it;
        }
    }

    m_qosWreds.erase(wred_id);

    if (!queues.empty())
    {
        SWSS_LOG_WARN("qos: WRED %s removed while still bound to %zu queue(s); disabling WRED",
                sai_serialize_object_id(wred_id).c_str(), queues.size());

        for (auto q: queues) (void)qosApplyQueueWred(q);
    }

    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchBCM56860HW::qosApplyQueueWred(
        _In_ sai_object_id_t queue_id)
{
    SWSS_LOG_ENTER();

    auto it = m_qosQueueHw.find(queue_id);

    if (it == m_qosQueueHw.end() || it->second.cpu || it->second.bcmPort < 0)
    {
        return SAI_STATUS_SUCCESS;
    }

    const QosQueueHw &h = it->second;

    auto wit = m_qosQueueWred.find(queue_id);

    sai_object_id_t wred = (wit == m_qosQueueWred.end()) ? SAI_NULL_OBJECT_ID : wit->second;

    if (h.mcast || h.cos < 0 || h.cos >= HWBCM_QOS_NUM_COS)
    {
        if (wred != SAI_NULL_OBJECT_ID)
        {
            SWSS_LOG_WARN("qos: WRED on %s queue %d of bcm port %d not supported (UC queues 0..%d only), ignored",
                    h.mcast ? "multicast" : "unicast", h.cos, h.bcmPort, HWBCM_QOS_NUM_COS - 1);
        }
        return SAI_STATUS_SUCCESS;
    }

    hwbcm_qos_wred_t w;

    memset(&w, 0, sizeof(w));

    bool have = wred != SAI_NULL_OBJECT_ID && m_qosWreds.find(wred) != m_qosWreds.end();

    if (have)
    {
        static const sai_attr_id_t en[3] = { SAI_WRED_ATTR_GREEN_ENABLE, SAI_WRED_ATTR_YELLOW_ENABLE, SAI_WRED_ATTR_RED_ENABLE };
        static const sai_attr_id_t mn[3] = { SAI_WRED_ATTR_GREEN_MIN_THRESHOLD, SAI_WRED_ATTR_YELLOW_MIN_THRESHOLD, SAI_WRED_ATTR_RED_MIN_THRESHOLD };
        static const sai_attr_id_t mx[3] = { SAI_WRED_ATTR_GREEN_MAX_THRESHOLD, SAI_WRED_ATTR_YELLOW_MAX_THRESHOLD, SAI_WRED_ATTR_RED_MAX_THRESHOLD };
        static const sai_attr_id_t pr[3] = { SAI_WRED_ATTR_GREEN_DROP_PROBABILITY, SAI_WRED_ATTR_YELLOW_DROP_PROBABILITY, SAI_WRED_ATTR_RED_DROP_PROBABILITY };
        static const sai_attr_id_t emn[3] = { SAI_WRED_ATTR_ECN_GREEN_MIN_THRESHOLD, SAI_WRED_ATTR_ECN_YELLOW_MIN_THRESHOLD, SAI_WRED_ATTR_ECN_RED_MIN_THRESHOLD };
        static const sai_attr_id_t emx[3] = { SAI_WRED_ATTR_ECN_GREEN_MAX_THRESHOLD, SAI_WRED_ATTR_ECN_YELLOW_MAX_THRESHOLD, SAI_WRED_ATTR_ECN_RED_MAX_THRESHOLD };
        static const sai_attr_id_t epr[3] = { SAI_WRED_ATTR_ECN_GREEN_MARK_PROBABILITY, SAI_WRED_ATTR_ECN_YELLOW_MARK_PROBABILITY, SAI_WRED_ATTR_ECN_RED_MARK_PROBABILITY };

        sai_attribute_t a;

        int ecnMode = SAI_ECN_MARK_MODE_NONE;

        if (getStoredAttr(SAI_OBJECT_TYPE_WRED, wred, SAI_WRED_ATTR_ECN_MARK_MODE, a))
        {
            ecnMode = a.value.s32;
        }

        bool ecnColor[3] = {
            ecnMode == SAI_ECN_MARK_MODE_GREEN || ecnMode == SAI_ECN_MARK_MODE_GREEN_YELLOW ||
                ecnMode == SAI_ECN_MARK_MODE_GREEN_RED || ecnMode == SAI_ECN_MARK_MODE_ALL,
            ecnMode == SAI_ECN_MARK_MODE_YELLOW || ecnMode == SAI_ECN_MARK_MODE_GREEN_YELLOW ||
                ecnMode == SAI_ECN_MARK_MODE_YELLOW_RED || ecnMode == SAI_ECN_MARK_MODE_ALL,
            ecnMode == SAI_ECN_MARK_MODE_RED || ecnMode == SAI_ECN_MARK_MODE_GREEN_RED ||
                ecnMode == SAI_ECN_MARK_MODE_YELLOW_RED || ecnMode == SAI_ECN_MARK_MODE_ALL,
        };

        for (int c = 0; c < 3; c++)
        {
            bool on = getStoredAttr(SAI_OBJECT_TYPE_WRED, wred, en[c], a) && a.value.booldata;

            // ECN marking on TD2+ rides on the WRED curve of the color, so a
            // color that only marks still needs its curve; fall back to the
            // ECN_* threshold/probability attributes when drop attributes are absent.
            w.enable[c] = (on || ecnColor[c]) ? 1 : 0;

            bool preferEcn = (!on && ecnColor[c]);
            sai_attr_id_t mn1 = preferEcn ? emn[c] : mn[c];
            sai_attr_id_t mn2 = preferEcn ? mn[c] : emn[c];
            sai_attr_id_t mx1 = preferEcn ? emx[c] : mx[c];
            sai_attr_id_t mx2 = preferEcn ? mx[c] : emx[c];
            sai_attr_id_t pr1 = preferEcn ? epr[c] : pr[c];
            sai_attr_id_t pr2 = preferEcn ? pr[c] : epr[c];

            if (getStoredAttr(SAI_OBJECT_TYPE_WRED, wred, mn1, a) ||
                    (ecnColor[c] && getStoredAttr(SAI_OBJECT_TYPE_WRED, wred, mn2, a)))
            {
                w.min_bytes[c] = a.value.u32;
            }
            if (getStoredAttr(SAI_OBJECT_TYPE_WRED, wred, mx1, a) ||
                    (ecnColor[c] && getStoredAttr(SAI_OBJECT_TYPE_WRED, wred, mx2, a)))
            {
                w.max_bytes[c] = a.value.u32;
            }
            if (getStoredAttr(SAI_OBJECT_TYPE_WRED, wred, pr1, a) ||
                    (ecnColor[c] && getStoredAttr(SAI_OBJECT_TYPE_WRED, wred, pr2, a)))
            {
                w.drop_pct[c] = (int)a.value.u32;
            }
            else
            {
                w.drop_pct[c] = 100;
            }
        }

        w.ecn = (ecnMode != SAI_ECN_MARK_MODE_NONE) ? 1 : 0;
        w.gain = getStoredAttr(SAI_OBJECT_TYPE_WRED, wred, SAI_WRED_ATTR_WEIGHT, a) ? (int)a.value.u8 : 0;

        if (w.ecn && !(ecnColor[0] && ecnColor[1] && ecnColor[2]))
        {
            SWSS_LOG_INFO("qos: ECN mark mode %d: TD2+ marks per queue, not per color (bcm port %d queue %d)",
                    ecnMode, h.bcmPort, h.cos);
        }
    }

    int rv = hwbcm_qos_cos_wred_set(h.bcmPort, h.cos, have ? &w : NULL);

    if (rv < 0)
    {
        SWSS_LOG_ERROR("qos: WRED %s on bcm port %d queue %d failed: %d",
                sai_serialize_object_id(wred).c_str(), h.bcmPort, h.cos, rv);

        return SAI_STATUS_FAILURE;
    }

    return SAI_STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// BUFFER_POOL / BUFFER_PROFILE
// ---------------------------------------------------------------------------

sai_status_t SwitchBCM56860HW::hwCreateBufferPool(
        _In_ sai_object_id_t pool_id,
        _In_ uint32_t attr_count,
        _In_ const sai_attribute_t *attr_list)
{
    SWSS_LOG_ENTER();

    QosPool p;

    sai_attribute_t a;

    // read from the store (create stored them already; also used on resync)
    if (getStoredAttr(SAI_OBJECT_TYPE_BUFFER_POOL, pool_id, SAI_BUFFER_POOL_ATTR_TYPE, a))
    {
        p.type = a.value.s32;
    }

    if (getStoredAttr(SAI_OBJECT_TYPE_BUFFER_POOL, pool_id, SAI_BUFFER_POOL_ATTR_SIZE, a))
    {
        p.size = a.value.u64;
    }

    if (getStoredAttr(SAI_OBJECT_TYPE_BUFFER_POOL, pool_id, SAI_BUFFER_POOL_ATTR_THRESHOLD_MODE, a))
    {
        p.dynamic = (a.value.s32 == SAI_BUFFER_POOL_THRESHOLD_MODE_DYNAMIC);
    }

    bool ingress = (p.type == SAI_BUFFER_POOL_TYPE_INGRESS || p.type == SAI_BUFFER_POOL_TYPE_BOTH);
    bool egress = (p.type == SAI_BUFFER_POOL_TYPE_EGRESS || p.type == SAI_BUFFER_POOL_TYPE_BOTH);

    // service pools in creation order per direction; the first one is SP0,
    // where init mmu put every PG and queue
    for (int i = 0; ingress && i < HWBCM_QOS_NUM_POOL; i++)
    {
        if (!m_qosIngressSpUsed[i])
        {
            m_qosIngressSpUsed[i] = true;
            p.ingressSp = i;
            break;
        }
    }

    for (int i = 0; egress && i < HWBCM_QOS_NUM_POOL; i++)
    {
        if (!m_qosEgressSpUsed[i])
        {
            m_qosEgressSpUsed[i] = true;
            p.egressSp = i;
            break;
        }
    }

    if ((ingress && p.ingressSp < 0) || (egress && p.egressSp < 0))
    {
        SWSS_LOG_ERROR("qos: no free %s service pool for %s (TD2+ has %d per direction)",
                ingress ? "ingress" : "egress", sai_serialize_object_id(pool_id).c_str(), HWBCM_QOS_NUM_POOL);

        if (p.ingressSp >= 0) m_qosIngressSpUsed[p.ingressSp] = false;
        if (p.egressSp >= 0) m_qosEgressSpUsed[p.egressSp] = false;

        return SAI_STATUS_INSUFFICIENT_RESOURCES;
    }

    m_qosPools[pool_id] = p;

    int sp = ingress ? p.ingressSp : p.egressSp;

    uint32_t ing = 0, egr = 0;

    if (sp >= 0 && hwbcm_qos_pool_shared_get(sp, &ing, &egr) == 0)
    {
        SWSS_LOG_NOTICE("qos: buffer pool %s -> %s SP%d, SAI size %" PRIu64 " (recorded only; HW shared now ingress %u / egress %u bytes)",
                sai_serialize_object_id(pool_id).c_str(),
                ingress && egress ? "ingress+egress" : (ingress ? "ingress" : "egress"),
                sp, p.size, ing, egr);
    }

    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchBCM56860HW::hwSetBufferPool(
        _In_ sai_object_id_t pool_id,
        _In_ const sai_attribute_t *attr)
{
    SWSS_LOG_ENTER();

    auto it = m_qosPools.find(pool_id);

    if (it == m_qosPools.end() || attr == NULL)
    {
        return SAI_STATUS_SUCCESS;
    }

    switch (attr->id)
    {
        case SAI_BUFFER_POOL_ATTR_SIZE:

            it->second.size = attr->value.u64;
            SWSS_LOG_NOTICE("qos: buffer pool %s size %" PRIu64 " recorded (pool sizes stay as init mmu set them)",
                    sai_serialize_object_id(pool_id).c_str(), it->second.size);
            return SAI_STATUS_SUCCESS;

        case SAI_BUFFER_POOL_ATTR_THRESHOLD_MODE:

            it->second.dynamic = (attr->value.s32 == SAI_BUFFER_POOL_THRESHOLD_MODE_DYNAMIC);
            break;

        case SAI_BUFFER_POOL_ATTR_XOFF_SIZE:

            if (attr->value.u64 != 0)
            {
                SWSS_LOG_WARN("qos: shared headroom pool (XOFF_SIZE) not programmed; PG headroom is per PG");
            }
            return SAI_STATUS_SUCCESS;

        default:
            return SAI_STATUS_SUCCESS;
    }

    // threshold mode is the default of profiles without their own: re-apply
    sai_status_t status = SAI_STATUS_SUCCESS;

    for (auto prof: m_qosProfiles)
    {
        sai_attribute_t a;

        if (getStoredAttr(SAI_OBJECT_TYPE_BUFFER_PROFILE, prof, SAI_BUFFER_PROFILE_ATTR_POOL_ID, a) &&
                a.value.oid == pool_id)
        {
            sai_attribute_t dummy;

            dummy.id = SAI_BUFFER_PROFILE_ATTR_THRESHOLD_MODE;

            if (hwSetBufferProfile(prof, &dummy) != SAI_STATUS_SUCCESS)
            {
                status = SAI_STATUS_FAILURE;
            }
        }
    }

    return status;
}

sai_status_t SwitchBCM56860HW::hwRemoveBufferPool(
        _In_ sai_object_id_t pool_id)
{
    SWSS_LOG_ENTER();

    auto it = m_qosPools.find(pool_id);

    if (it != m_qosPools.end())
    {
        if (it->second.ingressSp >= 0) m_qosIngressSpUsed[it->second.ingressSp] = false;
        if (it->second.egressSp >= 0) m_qosEgressSpUsed[it->second.egressSp] = false;

        m_qosPools.erase(it);
    }

    return SAI_STATUS_SUCCESS;
}

bool SwitchBCM56860HW::qosDecodeBufferProfile(
        _In_ sai_object_id_t profile_id,
        _In_ bool ingress,
        _Out_ QosBuffer &out)
{
    SWSS_LOG_ENTER();

    out = QosBuffer();

    if (profile_id == SAI_NULL_OBJECT_ID || m_qosProfiles.find(profile_id) == m_qosProfiles.end())
    {
        return false;
    }

    sai_attribute_t a;

    bool poolDynamic = true;

    if (getStoredAttr(SAI_OBJECT_TYPE_BUFFER_PROFILE, profile_id, SAI_BUFFER_PROFILE_ATTR_POOL_ID, a))
    {
        auto pit = m_qosPools.find(a.value.oid);

        if (pit != m_qosPools.end())
        {
            out.pool = ingress ? pit->second.ingressSp : pit->second.egressSp;
            poolDynamic = pit->second.dynamic;

            if (out.pool < 0)
            {
                SWSS_LOG_WARN("qos: profile %s uses an %s pool on an %s object, pool left unchanged",
                        sai_serialize_object_id(profile_id).c_str(),
                        ingress ? "egress" : "ingress", ingress ? "ingress" : "egress");
            }
        }
    }

    auto u32of = [](uint64_t v) -> uint32_t { return v > 0xffffffffull ? 0xffffffffu : (uint32_t)v; };

    if (getStoredAttr(SAI_OBJECT_TYPE_BUFFER_PROFILE, profile_id, SAI_BUFFER_PROFILE_ATTR_RESERVED_BUFFER_SIZE, a))
    {
        out.minBytes = u32of(a.value.u64);
    }

    out.dynamic = poolDynamic;

    if (getStoredAttr(SAI_OBJECT_TYPE_BUFFER_PROFILE, profile_id, SAI_BUFFER_PROFILE_ATTR_THRESHOLD_MODE, a))
    {
        out.dynamic = (a.value.s32 == SAI_BUFFER_PROFILE_THRESHOLD_MODE_DYNAMIC);
    }

    if (getStoredAttr(SAI_OBJECT_TYPE_BUFFER_PROFILE, profile_id, SAI_BUFFER_PROFILE_ATTR_SHARED_DYNAMIC_TH, a))
    {
        out.alphaExp = (int)a.value.s8;
    }

    if (getStoredAttr(SAI_OBJECT_TYPE_BUFFER_PROFILE, profile_id, SAI_BUFFER_PROFILE_ATTR_SHARED_STATIC_TH, a))
    {
        out.staticBytes = u32of(a.value.u64);
    }

    if (getStoredAttr(SAI_OBJECT_TYPE_BUFFER_PROFILE, profile_id, SAI_BUFFER_PROFILE_ATTR_XOFF_TH, a))
    {
        out.xoffBytes = u32of(a.value.u64);
    }

    if (getStoredAttr(SAI_OBJECT_TYPE_BUFFER_PROFILE, profile_id, SAI_BUFFER_PROFILE_ATTR_XON_TH, a))
    {
        out.xonBytes = u32of(a.value.u64);
    }

    if (getStoredAttr(SAI_OBJECT_TYPE_BUFFER_PROFILE, profile_id, SAI_BUFFER_PROFILE_ATTR_XON_OFFSET_TH, a))
    {
        out.xonOffsetBytes = u32of(a.value.u64);
    }

    return true;
}

sai_status_t SwitchBCM56860HW::hwCreateBufferProfile(
        _In_ sai_object_id_t profile_id,
        _In_ uint32_t attr_count,
        _In_ const sai_attribute_t *attr_list)
{
    SWSS_LOG_ENTER();

    m_qosProfiles.insert(profile_id);

    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchBCM56860HW::hwSetBufferProfile(
        _In_ sai_object_id_t profile_id,
        _In_ const sai_attribute_t *attr)
{
    SWSS_LOG_ENTER();

    m_qosProfiles.insert(profile_id);

    std::vector<sai_object_id_t> pgs, queues;

    for (auto &kv: m_qosPgProfile) if (kv.second == profile_id) pgs.push_back(kv.first);
    for (auto &kv: m_qosQueueProfile) if (kv.second == profile_id) queues.push_back(kv.first);

    sai_status_t status = SAI_STATUS_SUCCESS;

    for (auto pg: pgs)
    {
        if (qosApplyPgBuffer(pg) != SAI_STATUS_SUCCESS) status = SAI_STATUS_FAILURE;
    }

    for (auto q: queues)
    {
        if (qosApplyQueueBuffer(q) != SAI_STATUS_SUCCESS) status = SAI_STATUS_FAILURE;
    }

    return status;
}

sai_status_t SwitchBCM56860HW::hwRemoveBufferProfile(
        _In_ sai_object_id_t profile_id)
{
    SWSS_LOG_ENTER();

    std::vector<sai_object_id_t> pgs, queues;

    for (auto it = m_qosPgProfile.begin(); it != m_qosPgProfile.end();)
    {
        if (it->second == profile_id)
        {
            pgs.push_back(it->first);
            it = m_qosPgProfile.erase(it);
        }
        else
        {
            ++it;
        }
    }

    for (auto it = m_qosQueueProfile.begin(); it != m_qosQueueProfile.end();)
    {
        if (it->second == profile_id)
        {
            queues.push_back(it->first);
            it = m_qosQueueProfile.erase(it);
        }
        else
        {
            ++it;
        }
    }

    m_qosProfiles.erase(profile_id);

    if (!pgs.empty() || !queues.empty())
    {
        SWSS_LOG_WARN("qos: buffer profile %s removed while still bound (pgs=%zu queues=%zu); restoring defaults",
                sai_serialize_object_id(profile_id).c_str(), pgs.size(), queues.size());

        for (auto pg: pgs) (void)qosApplyPgBuffer(pg);
        for (auto q: queues) (void)qosApplyQueueBuffer(q);
    }

    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchBCM56860HW::qosApplyPgBuffer(
        _In_ sai_object_id_t pg_id)
{
    SWSS_LOG_ENTER();

    auto it = m_qosPgHw.find(pg_id);

    if (it == m_qosPgHw.end() || it->second.bcmPort < 0)
    {
        return SAI_STATUS_SUCCESS;
    }

    const QosPgHw &h = it->second;

    auto pit = m_qosPgProfile.find(pg_id);

    sai_object_id_t prof = (pit == m_qosPgProfile.end()) ? SAI_NULL_OBJECT_ID : pit->second;

    QosBuffer b;

    bool have = qosDecodeBufferProfile(prof, true, b);

    hwbcm_qos_buffer_t hw;

    memset(&hw, 0, sizeof(hw));

    hw.pool = b.pool;
    hw.min_bytes = b.minBytes;
    hw.dynamic = b.dynamic ? 1 : 0;
    hw.alpha_exp = b.alphaExp;
    hw.static_bytes = b.staticBytes;
    hw.xoff_bytes = b.xoffBytes;
    hw.xon_bytes = b.xonBytes;
    hw.xon_offset_bytes = b.xonOffsetBytes;

    int rv = hwbcm_qos_pg_buffer_set(h.bcmPort, h.pg, have ? &hw : NULL);

    if (rv == QOS_HWBCM_E_NOT_FOUND)
    {
        // min/headroom wait for a TC -> PG mapping (qosApplyPortTcToPg retries)
        m_qosPgPending.insert(pg_id);

        return SAI_STATUS_SUCCESS;
    }

    m_qosPgPending.erase(pg_id);

    if (rv < 0)
    {
        SWSS_LOG_ERROR("qos: buffer profile %s on bcm port %d PG %d failed: %d",
                sai_serialize_object_id(prof).c_str(), h.bcmPort, h.pg, rv);

        return SAI_STATUS_FAILURE;
    }

    return SAI_STATUS_SUCCESS;
}

void SwitchBCM56860HW::qosRetryPendingPgs(
        _In_ sai_object_id_t port_id)
{
    SWSS_LOG_ENTER();

    std::vector<sai_object_id_t> todo;

    for (auto pg: m_qosPgPending)
    {
        auto it = m_qosPgHw.find(pg);

        if (it != m_qosPgHw.end() && it->second.port == port_id)
        {
            todo.push_back(pg);
        }
    }

    for (auto pg: todo)
    {
        (void)qosApplyPgBuffer(pg);
    }
}

sai_status_t SwitchBCM56860HW::qosApplyQueueBuffer(
        _In_ sai_object_id_t queue_id)
{
    SWSS_LOG_ENTER();

    auto it = m_qosQueueHw.find(queue_id);

    if (it == m_qosQueueHw.end() || it->second.cpu || it->second.bcmPort < 0)
    {
        return SAI_STATUS_SUCCESS;
    }

    const QosQueueHw &h = it->second;

    auto pit = m_qosQueueProfile.find(queue_id);

    sai_object_id_t prof = (pit == m_qosQueueProfile.end()) ? SAI_NULL_OBJECT_ID : pit->second;

    if (h.cos < 0 || h.cos >= HWBCM_QOS_NUM_COS)
    {
        if (prof != SAI_NULL_OBJECT_ID)
        {
            SWSS_LOG_WARN("qos: buffer profile on queue %d of bcm port %d not reachable in non-ETS mode, ignored",
                    h.cos, h.bcmPort);
        }
        return SAI_STATUS_SUCCESS;
    }

    QosBuffer b;

    bool have = qosDecodeBufferProfile(prof, false, b);

    hwbcm_qos_buffer_t hw;

    memset(&hw, 0, sizeof(hw));

    hw.pool = b.pool;
    hw.min_bytes = b.minBytes;
    hw.dynamic = b.dynamic ? 1 : 0;
    hw.alpha_exp = b.alphaExp;
    hw.static_bytes = b.staticBytes;

    int rv = hwbcm_qos_queue_buffer_set(h.bcmPort, h.cos, h.mcast ? 1 : 0, have ? &hw : NULL);

    if (rv < 0)
    {
        SWSS_LOG_ERROR("qos: buffer profile %s on bcm port %d %s queue %d failed: %d",
                sai_serialize_object_id(prof).c_str(), h.bcmPort, h.mcast ? "mc" : "uc", h.cos, rv);

        return SAI_STATUS_FAILURE;
    }

    return SAI_STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// QUEUE / INGRESS_PRIORITY_GROUP attributes
// ---------------------------------------------------------------------------

sai_status_t SwitchBCM56860HW::hwSetQueue(
        _In_ sai_object_id_t queue_id,
        _In_ const sai_attribute_t *attr)
{
    SWSS_LOG_ENTER();

    if (attr == NULL)
    {
        return SAI_STATUS_SUCCESS;
    }

    auto bind = [&](std::map<sai_object_id_t, sai_object_id_t> &m)
    {
        if (attr->value.oid == SAI_NULL_OBJECT_ID)
        {
            m.erase(queue_id);
        }
        else
        {
            m[queue_id] = attr->value.oid;
        }
    };

    switch (attr->id)
    {
        case SAI_QUEUE_ATTR_SCHEDULER_PROFILE_ID:

            bind(m_qosQueueSched);
            return qosApplyQueueScheduler(queue_id);

        case SAI_QUEUE_ATTR_WRED_PROFILE_ID:

            bind(m_qosQueueWred);
            return qosApplyQueueWred(queue_id);

        case SAI_QUEUE_ATTR_BUFFER_PROFILE_ID:

            bind(m_qosQueueProfile);
            return qosApplyQueueBuffer(queue_id);

        case SAI_QUEUE_ATTR_PFC_DLR_INIT:
        case SAI_QUEUE_ATTR_ENABLE_PFC_DLDR:

            SWSS_LOG_WARN("qos: PFC deadlock detection/recovery attribute %d not programmed to hardware", attr->id);
            return SAI_STATUS_SUCCESS;

        default:

            // TYPE / INDEX / PORT / PARENT_SCHEDULER_NODE: tree bookkeeping
            return SAI_STATUS_SUCCESS;
    }
}

sai_status_t SwitchBCM56860HW::hwSetIngressPriorityGroup(
        _In_ sai_object_id_t pg_id,
        _In_ const sai_attribute_t *attr)
{
    SWSS_LOG_ENTER();

    if (attr == NULL || attr->id != SAI_INGRESS_PRIORITY_GROUP_ATTR_BUFFER_PROFILE)
    {
        return SAI_STATUS_SUCCESS;
    }

    if (attr->value.oid == SAI_NULL_OBJECT_ID)
    {
        m_qosPgProfile.erase(pg_id);
    }
    else
    {
        m_qosPgProfile[pg_id] = attr->value.oid;
    }

    return qosApplyPgBuffer(pg_id);
}
