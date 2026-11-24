/*
 * SwitchBCM56860HW_stats.cpp — statistics of QUEUE, INGRESS_PRIORITY_GROUP,
 * BUFFER_POOL objects and the MMU-sourced port counters, plus
 * queryStatsCapability, for the Trident2+ hardware switch.
 *
 * Object -> hardware lookup (no dependency on the QoS code):
 *   QUEUE  : SAI_QUEUE_ATTR_PORT / INDEX / TYPE from the vslib object hash
 *            (written by SwitchBCM56850::create_qos_queues_per_port: UC queues
 *            index 0..9, MC queues index 10..19; CPU queues 0..31, MC).
 *            -> (bcm port, cos 0..9, mc). CPU port -> CMIC port, cos = index.
 *   PG     : SAI_INGRESS_PRIORITY_GROUP_ATTR_PORT / INDEX -> (bcm port, pg).
 *   POOL   : SAI_BUFFER_POOL_ATTR_TYPE, and one PG / queue whose
 *            BUFFER_PROFILE(_ID) has SAI_BUFFER_PROFILE_ATTR_POOL_ID == pool.
 *            The service pool number is then read back from the hardware
 *            (that PG's / queue's SPID), so whatever pool assignment the QoS
 *            code programmed is what gets reported.
 */

#include "SwitchBCM56860HW.h"

#include "swss/logger.h"
#include "meta/sai_serialize.h"

#include "hwbcm/hwbcm.h"
#include "hwbcm/hwbcm_stats.h"

#include <vector>

using namespace saivs;

#ifndef VS_COUNTERS_COUNT_MSB
#define VS_COUNTERS_COUNT_MSB (0x80000000)   /* vslib SwitchState.cpp:18 */
#endif

namespace
{
    struct HwStatMap
    {
        int32_t sai;    /* SAI_*_STAT_* */
        int hw;         /* HWBCM_*STAT_* */
        bool bytes;     /* hw value is in cells, report bytes */
    };

    const HwStatMap g_queueStats[] = {
        { SAI_QUEUE_STAT_PACKETS,                     HWBCM_QSTAT_PACKETS,           false },
        { SAI_QUEUE_STAT_BYTES,                       HWBCM_QSTAT_BYTES,             false },
        { SAI_QUEUE_STAT_DROPPED_PACKETS,             HWBCM_QSTAT_DROPPED_PACKETS,   false },
        { SAI_QUEUE_STAT_DROPPED_BYTES,               HWBCM_QSTAT_DROPPED_BYTES,     false },
        { SAI_QUEUE_STAT_CURR_OCCUPANCY_BYTES,        HWBCM_QSTAT_CURR_CELLS,        true  },
        { SAI_QUEUE_STAT_CURR_OCCUPANCY_CELLS,        HWBCM_QSTAT_CURR_CELLS,        false },
        { SAI_QUEUE_STAT_SHARED_CURR_OCCUPANCY_BYTES, HWBCM_QSTAT_SHARED_CURR_CELLS, true  },
        { SAI_QUEUE_STAT_SHARED_CURR_OCCUPANCY_CELLS, HWBCM_QSTAT_SHARED_CURR_CELLS, false },
        { SAI_QUEUE_STAT_WATERMARK_BYTES,             HWBCM_QSTAT_WATERMARK_CELLS,   true  },
        { SAI_QUEUE_STAT_WATERMARK_CELLS,             HWBCM_QSTAT_WATERMARK_CELLS,   false },
        { SAI_QUEUE_STAT_SHARED_WATERMARK_BYTES,      HWBCM_QSTAT_WATERMARK_CELLS,   true  },
        { SAI_QUEUE_STAT_SHARED_WATERMARK_CELLS,      HWBCM_QSTAT_WATERMARK_CELLS,   false },
    };

    const HwStatMap g_pgStats[] = {
        { SAI_INGRESS_PRIORITY_GROUP_STAT_CURR_OCCUPANCY_BYTES,           HWBCM_PGSTAT_CURR_CELLS,               true  },
        { SAI_INGRESS_PRIORITY_GROUP_STAT_CURR_OCCUPANCY_CELLS,           HWBCM_PGSTAT_CURR_CELLS,               false },
        { SAI_INGRESS_PRIORITY_GROUP_STAT_SHARED_CURR_OCCUPANCY_BYTES,    HWBCM_PGSTAT_SHARED_CURR_CELLS,        true  },
        { SAI_INGRESS_PRIORITY_GROUP_STAT_SHARED_CURR_OCCUPANCY_CELLS,    HWBCM_PGSTAT_SHARED_CURR_CELLS,        false },
        { SAI_INGRESS_PRIORITY_GROUP_STAT_XOFF_ROOM_CURR_OCCUPANCY_BYTES, HWBCM_PGSTAT_HEADROOM_CURR_CELLS,      true  },
        { SAI_INGRESS_PRIORITY_GROUP_STAT_XOFF_ROOM_CURR_OCCUPANCY_CELLS, HWBCM_PGSTAT_HEADROOM_CURR_CELLS,      false },
        { SAI_INGRESS_PRIORITY_GROUP_STAT_SHARED_WATERMARK_BYTES,         HWBCM_PGSTAT_SHARED_WATERMARK_CELLS,   true  },
        { SAI_INGRESS_PRIORITY_GROUP_STAT_SHARED_WATERMARK_CELLS,         HWBCM_PGSTAT_SHARED_WATERMARK_CELLS,   false },
        { SAI_INGRESS_PRIORITY_GROUP_STAT_XOFF_ROOM_WATERMARK_BYTES,      HWBCM_PGSTAT_HEADROOM_WATERMARK_CELLS, true  },
        { SAI_INGRESS_PRIORITY_GROUP_STAT_XOFF_ROOM_WATERMARK_CELLS,      HWBCM_PGSTAT_HEADROOM_WATERMARK_CELLS, false },
    };

    const HwStatMap g_poolStats[] = {
        { SAI_BUFFER_POOL_STAT_WATERMARK_BYTES, HWBCM_POOLSTAT_WATERMARK_CELLS, true  },
        { SAI_BUFFER_POOL_STAT_WATERMARK_CELLS, HWBCM_POOLSTAT_WATERMARK_CELLS, false },
    };

    const HwStatMap g_portXStats[] = {
        { SAI_PORT_STAT_IN_DROPPED_PKTS,                    HWBCM_XPSTAT_IN_DROPPED_PKTS,          false },
        { SAI_PORT_STAT_OUT_DROPPED_PKTS,                   HWBCM_XPSTAT_OUT_DROPPED_PKTS,         false },
        { SAI_PORT_STAT_GREEN_WRED_DROPPED_PACKETS,         HWBCM_XPSTAT_GREEN_WRED_DROPPED_PKTS,  false },
        { SAI_PORT_STAT_YELLOW_WRED_DROPPED_PACKETS,        HWBCM_XPSTAT_YELLOW_WRED_DROPPED_PKTS, false },
        { SAI_PORT_STAT_RED_WRED_DROPPED_PACKETS,           HWBCM_XPSTAT_RED_WRED_DROPPED_PKTS,    false },
        { SAI_PORT_STAT_WRED_DROPPED_PACKETS,               HWBCM_XPSTAT_WRED_DROPPED_PKTS,        false },
        { SAI_PORT_STAT_ETHER_STATS_PKTS_1519_TO_2047_OCTETS,  HWBCM_XPSTAT_ETHER_PKTS_1519_TO_2047,  false },
        { SAI_PORT_STAT_ETHER_STATS_PKTS_2048_TO_4095_OCTETS,  HWBCM_XPSTAT_ETHER_PKTS_2048_TO_4095,  false },
        { SAI_PORT_STAT_ETHER_STATS_PKTS_4096_TO_9216_OCTETS,  HWBCM_XPSTAT_ETHER_PKTS_4096_TO_9216,  false },
        { SAI_PORT_STAT_ETHER_STATS_PKTS_9217_TO_16383_OCTETS, HWBCM_XPSTAT_ETHER_PKTS_9217_TO_16383, false },
        { SAI_PORT_STAT_IP_IN_RECEIVES,                     HWBCM_XPSTAT_IP_IN_RECEIVES,           false },
        { SAI_PORT_STAT_IP_IN_DISCARDS,                     HWBCM_XPSTAT_IP_IN_DISCARDS,           false },
        { SAI_PORT_STAT_IPV6_IN_RECEIVES,                   HWBCM_XPSTAT_IPV6_IN_RECEIVES,         false },
        { SAI_PORT_STAT_IPV6_IN_DISCARDS,                   HWBCM_XPSTAT_IPV6_IN_DISCARDS,         false },
    };

    template <size_t N>
    const HwStatMap* findStat(
            _In_ const HwStatMap (&table)[N],
            _In_ sai_stat_id_t id)
    {
        for (size_t i = 0; i < N; i++)
        {
            if (table[i].sai == (int32_t)id)
            {
                return &table[i];
            }
        }

        return nullptr;
    }

    template <size_t N>
    bool inTable(
            _In_ const HwStatMap (&table)[N],
            _In_ sai_stat_id_t id)
    {
        return findStat(table, id) != nullptr;
    }

    /* Run one hwbcm *_multi_get through a SAI <-> hw id table. */
    template <size_t N, typename F>
    sai_status_t readThrough(
            _In_ const HwStatMap (&table)[N],
            _In_ const char *what,
            _In_ uint32_t n,
            _In_ const sai_stat_id_t *counter_ids,
            _Out_ uint64_t *counters,
            _In_ F &&hwGet)
    {
        std::vector<int> hw(n);
        std::vector<bool> bytes(n);

        for (uint32_t i = 0; i < n; i++)
        {
            auto m = findStat(table, counter_ids[i]);

            if (m == nullptr)
            {
                SWSS_LOG_INFO("%s: counter %d not supported by the hardware", what, counter_ids[i]);
                return SAI_STATUS_NOT_SUPPORTED;
            }

            hw[i] = m->hw;
            bytes[i] = m->bytes;
        }

        std::vector<uint64_t> raw(n, 0);

        int rv = hwGet((int)n, hw.data(), raw.data());

        if (rv != 0)
        {
            SWSS_LOG_ERROR("%s: hardware read of %u counters failed: %d", what, n, rv);
            return SAI_STATUS_FAILURE;
        }

        uint64_t cell = hwbcm_stats_cell_bytes();

        for (uint32_t i = 0; i < n; i++)
        {
            counters[i] = bytes[i] ? raw[i] * cell : raw[i];
        }

        return SAI_STATUS_SUCCESS;
    }
}

// ---------------------------------------------------------------------------
// object -> hardware lookup
// ---------------------------------------------------------------------------

bool SwitchBCM56860HW::hwStatsQueueRef(
        _In_ sai_object_id_t queue_oid,
        _Out_ int &bcm_port,
        _Out_ int &cos,
        _Out_ bool &mc)
{
    SWSS_LOG_ENTER();

    sai_attribute_t port, index, type;

    if (!getStoredAttr(SAI_OBJECT_TYPE_QUEUE, queue_oid, SAI_QUEUE_ATTR_PORT, port) ||
            !getStoredAttr(SAI_OBJECT_TYPE_QUEUE, queue_oid, SAI_QUEUE_ATTR_INDEX, index))
    {
        return false;
    }

    mc = getStoredAttr(SAI_OBJECT_TYPE_QUEUE, queue_oid, SAI_QUEUE_ATTR_TYPE, type) &&
        type.value.s32 == SAI_QUEUE_TYPE_MULTICAST;

    int idx = index.value.u8;

    if (port.value.oid == m_cpu_port_id)
    {
        bcm_port = hwbcm_stats_cpu_port();
        cos = idx;
        mc = true;
        return true;
    }

    bcm_port = bcmPortOf(port.value.oid);

    if (bcm_port < 0)
    {
        return false;
    }

    cos = (mc && idx >= (int)m_unicastQueueNumber) ? idx - (int)m_unicastQueueNumber : idx;

    return true;
}

bool SwitchBCM56860HW::hwStatsPgRef(
        _In_ sai_object_id_t pg_oid,
        _Out_ int &bcm_port,
        _Out_ int &pg)
{
    SWSS_LOG_ENTER();

    sai_attribute_t port, index;

    if (!getStoredAttr(SAI_OBJECT_TYPE_INGRESS_PRIORITY_GROUP, pg_oid, SAI_INGRESS_PRIORITY_GROUP_ATTR_PORT, port) ||
            !getStoredAttr(SAI_OBJECT_TYPE_INGRESS_PRIORITY_GROUP, pg_oid, SAI_INGRESS_PRIORITY_GROUP_ATTR_INDEX, index))
    {
        return false;
    }

    bcm_port = bcmPortOf(port.value.oid);
    pg = index.value.u8;

    return bcm_port >= 0;
}

bool SwitchBCM56860HW::hwStatsPoolRef(
        _In_ sai_object_id_t pool_oid,
        _Out_ bool &egress,
        _Out_ int &bcm_port,
        _Out_ int &cos,
        _Out_ bool &mc)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;

    if (!getStoredAttr(SAI_OBJECT_TYPE_BUFFER_POOL, pool_oid, SAI_BUFFER_POOL_ATTR_TYPE, attr))
    {
        return false;
    }

    egress = (attr.value.s32 == SAI_BUFFER_POOL_TYPE_EGRESS);

    const sai_object_type_t userType = egress ? SAI_OBJECT_TYPE_QUEUE : SAI_OBJECT_TYPE_INGRESS_PRIORITY_GROUP;
    const sai_attr_id_t profileAttr = egress ? (sai_attr_id_t)SAI_QUEUE_ATTR_BUFFER_PROFILE_ID
                                             : (sai_attr_id_t)SAI_INGRESS_PRIORITY_GROUP_ATTR_BUFFER_PROFILE;

    /* user oid -> hardware ref if its profile is in this pool */
    auto tryUser = [&](sai_object_id_t user) -> bool
    {
        sai_attribute_t prof, pool;

        if (!getStoredAttr(userType, user, profileAttr, prof) || prof.value.oid == SAI_NULL_OBJECT_ID)
        {
            return false;
        }

        if (!getStoredAttr(SAI_OBJECT_TYPE_BUFFER_PROFILE, prof.value.oid, SAI_BUFFER_PROFILE_ATTR_POOL_ID, pool) ||
                pool.value.oid != pool_oid)
        {
            return false;
        }

        if (egress)
        {
            return hwStatsQueueRef(user, bcm_port, cos, mc) && bcm_port != hwbcm_stats_cpu_port();
        }

        mc = false;

        return hwStatsPgRef(user, bcm_port, cos);
    };

    auto cached = m_statPoolUser.find(pool_oid);

    if (cached != m_statPoolUser.end() && tryUser(cached->second))
    {
        return true;
    }

    m_statPoolUser.erase(pool_oid);

    auto oit = m_objectHash.find(userType);

    if (oit == m_objectHash.end())
    {
        return false;
    }

    for (auto &kv: oit->second)
    {
        sai_object_id_t user;

        sai_deserialize_object_id(kv.first, user);

        if (tryUser(user))
        {
            m_statPoolUser[pool_oid] = user;
            return true;
        }
    }

    return false;
}

// ---------------------------------------------------------------------------
// getStatsExt hooks
// ---------------------------------------------------------------------------

bool SwitchBCM56860HW::hwGetObjectStats(
        _In_ sai_object_type_t object_type,
        _In_ sai_object_id_t object_id,
        _In_ uint32_t number_of_counters,
        _In_ const sai_stat_id_t *counter_ids,
        _In_ sai_stats_mode_t mode,
        _Out_ uint64_t *counters,
        _Out_ sai_status_t &status)
{
    SWSS_LOG_ENTER();

    status = SAI_STATUS_SUCCESS;

    if (!m_hwReady || (number_of_counters & VS_COUNTERS_COUNT_MSB))
    {
        return false;
    }

    if (number_of_counters == 0)
    {
        return false;
    }

    const int clear = (mode == SAI_STATS_MODE_READ_AND_CLEAR) ? 1 : 0;
    const uint32_t n = number_of_counters;

    switch (object_type)
    {
        case SAI_OBJECT_TYPE_QUEUE:
        {
            int port, cos;
            bool mc;

            if (!hwStatsQueueRef(object_id, port, cos, mc))
            {
                return false;
            }

            status = readThrough(g_queueStats, "queue", n, counter_ids, counters,
                    [&](int cnt, const int *ids, uint64_t *out) {
                        return hwbcm_queue_stat_multi_get(port, cos, mc ? 1 : 0, cnt, ids, clear, out);
                    });

            return true;
        }

        case SAI_OBJECT_TYPE_INGRESS_PRIORITY_GROUP:
        {
            int port, pg;

            if (!hwStatsPgRef(object_id, port, pg))
            {
                return false;
            }

            status = readThrough(g_pgStats, "priority group", n, counter_ids, counters,
                    [&](int cnt, const int *ids, uint64_t *out) {
                        return hwbcm_pg_stat_multi_get(port, pg, cnt, ids, clear, out);
                    });

            return true;
        }

        case SAI_OBJECT_TYPE_BUFFER_POOL:
        {
            bool egress = false, mc = false;
            int port = -1, cos = -1;

            if (!hwStatsPoolRef(object_id, egress, port, cos, mc))
            {
                /* No PG / queue uses the pool (yet): it holds no buffers.
                 * Still validate the ids so unsupported ones are rejected. */
                status = readThrough(g_poolStats, "buffer pool", n, counter_ids, counters,
                        [&](int cnt, const int *, uint64_t *out) {
                            for (int i = 0; i < cnt; i++)
                            {
                                out[i] = 0;
                            }
                            return 0;
                        });

                return true;
            }

            status = readThrough(g_poolStats, "buffer pool", n, counter_ids, counters,
                    [&](int cnt, const int *ids, uint64_t *out) {
                        int sp = -1;
                        int rv = hwbcm_pool_stat_multi_get(egress ? 1 : 0, port, cos, mc ? 1 : 0,
                                cnt, ids, clear, out, &sp);
                        SWSS_LOG_DEBUG("pool %s: %s via port %d cos %d -> service pool %d",
                                sai_serialize_object_id(object_id).c_str(),
                                egress ? "egress" : "ingress", port, cos, sp);
                        return rv;
                    });

            return true;
        }

        default:
            return false;
    }
}

sai_status_t SwitchBCM56860HW::hwPortStatsOverlay(
        _In_ int bcm_port,
        _In_ uint32_t number_of_counters,
        _In_ const sai_stat_id_t *counter_ids,
        _In_ sai_stats_mode_t mode,
        _Inout_ uint64_t *counters)
{
    SWSS_LOG_ENTER();

    std::vector<int> ids(number_of_counters, HWBCM_XPSTAT_NONE);

    bool any = false;

    for (uint32_t i = 0; i < number_of_counters; i++)
    {
        auto m = findStat(g_portXStats, counter_ids[i]);

        if (m != nullptr)
        {
            ids[i] = m->hw;
            any = true;
        }
    }

    if (!any)
    {
        return SAI_STATUS_SUCCESS;
    }

    int rv = hwbcm_port_xstat_multi_get(bcm_port, (int)number_of_counters, ids.data(),
            mode == SAI_STATS_MODE_READ_AND_CLEAR ? 1 : 0, counters);

    if (rv != 0)
    {
        SWSS_LOG_ERROR("hwbcm_port_xstat_multi_get(port %d) failed: %d", bcm_port, rv);
        return SAI_STATUS_FAILURE;
    }

    return SAI_STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// capability
// ---------------------------------------------------------------------------

sai_status_t SwitchBCM56860HW::queryStatsCapability(
        _In_ sai_object_id_t switchId,
        _In_ sai_object_type_t objectType,
        _Inout_ sai_stat_capability_list_t *stats_capability)
{
    SWSS_LOG_ENTER();

    bool (*supported)(sai_stat_id_t) = nullptr;

    switch (objectType)
    {
        case SAI_OBJECT_TYPE_PORT:
            supported = [](sai_stat_id_t id) {
                return hwPortPstatOf(id) != 0 || inTable(g_portXStats, id);
            };
            break;

        case SAI_OBJECT_TYPE_QUEUE:
            supported = [](sai_stat_id_t id) { return inTable(g_queueStats, id); };
            break;

        case SAI_OBJECT_TYPE_INGRESS_PRIORITY_GROUP:
            supported = [](sai_stat_id_t id) { return inTable(g_pgStats, id); };
            break;

        case SAI_OBJECT_TYPE_BUFFER_POOL:
            supported = [](sai_stat_id_t id) { return inTable(g_poolStats, id); };
            break;

        default:
            return SwitchBCM56850::queryStatsCapability(switchId, objectType, stats_capability);
    }

    if (stats_capability == nullptr)
    {
        return SAI_STATUS_INVALID_PARAMETER;
    }

    auto info = sai_metadata_get_object_type_info(objectType);

    if (info == nullptr || info->statenum == nullptr)
    {
        return SAI_STATUS_NOT_SUPPORTED;
    }

    std::vector<sai_stat_id_t> list;

    for (size_t i = 0; i < info->statenum->valuescount; i++)
    {
        sai_stat_id_t id = (sai_stat_id_t)info->statenum->values[i];

        if (supported(id))
        {
            list.push_back(id);
        }
    }

    if (stats_capability->list == nullptr || stats_capability->count < list.size())
    {
        stats_capability->count = (uint32_t)list.size();
        return SAI_STATUS_BUFFER_OVERFLOW;
    }

    stats_capability->count = (uint32_t)list.size();

    for (size_t i = 0; i < list.size(); i++)
    {
        stats_capability->list[i].stat_enum = list[i];
        stats_capability->list[i].stat_modes = SAI_STATS_MODE_READ | SAI_STATS_MODE_READ_AND_CLEAR;
    }

    SWSS_LOG_NOTICE("stats capability %s: %zu counters",
            sai_serialize_object_type(objectType).c_str(), list.size());

    return SAI_STATUS_SUCCESS;
}
