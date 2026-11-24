# Integration notes: queue / PG / buffer pool / extra port statistics

Owner: stats agent, 2026-11-24. These files are all new; no existing file was edited.

| File | What it is |
|---|---|
| `sairedis/vslib/hwbcm/hwbcm_stats.h` | SDK-free C API: `hwbcm_queue_stat_multi_get`, `hwbcm_pg_stat_multi_get`, `hwbcm_pool_stat_multi_get`, `hwbcm_port_xstat_multi_get`, `hwbcm_stats_init` |
| `sairedis/vslib/hwbcm/hwbcm_stats.c` | The implementation (bcm_cosq_stat_get, BST, soc_counter_get for the UC/MC drop split) |
| `sairedis/vslib/SwitchBCM56860HW_stats.cpp` | `hwGetObjectStats`, `hwPortStatsOverlay`, the `queryStatsCapability` override, object-to-hardware lookups |
| `sairedis/vslib/SwitchBCM56860HW_stats.inc.h` | Member declarations to `#include` inside the class body |

Syntax checks (in `saibuild`) passed with no output:
- C: the command from the brief, plus `-Wall -Wextra -Werror`.
- C++: `vslib_cxx_cmd.txt` with `-fsyntax-only` (this includes `-Werror`). It ran against a copy of `SwitchBCM56860HW.h` that has the include as the last line of the class.

---

## 1. Root cause of "N/A" for the size buckets in `show interfaces counters detailed`

The hardware side was already fine. `hwPstatOf` maps `SAI_PORT_STAT_ETHER_IN_PKTS_*` and `ETHER_OUT_PKTS_*` to `snmpBcmReceivedPkts*` and `snmpBcmTransmittedPkts*` (`SwitchBCM56860HW.cpp:829-838`, `hwbcm_core.c` `pstat_map`). The problem is that syncd never asks for those counters:

1. orchagent puts the buckets in the PORT flex counter list (`sonic-swss/orchagent/portsorch.cpp:204-223`, `port_stat_ids`, installed at `portsorch.cpp:3806-3808`).
2. syncd builds the PORT context with `always_check_supported_counters = true`, and `use_sai_stats_capa_query` keeps its default `true` (`sonic-sairedis/syncd/FlexCounter.cpp:2746-2750`, `FlexCounter.h:85`).
3. `updateSupportedCounters` first calls `queryStatsCapability`. It falls back to trying each counter only if that call fails (`FlexCounter.cpp:1497-1501`). On success, the supported set is exactly the returned list (`FlexCounter.cpp:1504-1557`).
4. `addObject` keeps only the requested ids that are in the supported set (`FlexCounter.cpp:559-568`). Dropped ids are never polled, so the field is never written to `COUNTERS:oid:…`. The CLI prints a missing field as N/A.
5. Our switch does not override `queryStatsCapability`, so the vslib list is used (`VirtualSwitchSaiInterface.cpp:1024-1044` calls `SwitchStateBase::queryStatsCapability`, `SwitchStateBase.cpp:4328-4344`, which calls `queryPortStatsCapability`, `SwitchStateBase.cpp:4172-4286`). That hard-coded list has `ETHER_STATS_PKTS_*` but none of `ETHER_IN_PKTS_*` or `ETHER_OUT_PKTS_*`.

These ids from `port_stat_ids` are currently dropped by the vslib list (computed as orchagent's list minus vslib's list):
- All 20 `ETHER_IN_PKTS_*` / `ETHER_OUT_PKTS_*`
- `PAUSE_RX_PKTS`, `PAUSE_TX_PKTS` (mapped in hardware, never polled)
- `IP_IN_RECEIVES`, `IP_IN_UCAST_PKTS`
- All FEC counters

`IN_DROPPED_PKTS` and `OUT_DROPPED_PKTS` (`port_buffer_drop_stat_ids`, `portsorch.cpp:293-297`, `CounterType::PORT`, so the same capability path) are also missing from that list.

The vslib list also advertises counters we read as 0, for example `ETHER_STATS_PKTS_1519_TO_2047` and up, and `WRED_*`.

**Fix:** `SwitchBCM56860HW::queryStatsCapability` advertises exactly what the hardware provides. For PORT that is every SAI port stat for which `hwPstatOf` returns something other than `HWBCM_PSTAT_NONE`, plus the ids in `g_portXStats`. The list is built from `sai_metadata_get_object_type_info()->statenum`, so nothing is hard-coded twice.

The sonic-utilities sources are not in `~/homelab`, so the "missing field prints N/A" step is from memory of `portstat`, not verified against a file:line.

---

## 2. Exact wiring (lead)

### a) `SwitchBCM56860HW.h`
Add this as the last line inside the class body, just before the closing `};`:
```cpp
#include "SwitchBCM56860HW_stats.inc.h"
```
It sets its own `public:` / `protected:` and leaves the class in `protected:`.

### b) `SwitchBCM56860HW.cpp`
Add this right after `static int hwPstatOf(...)`. It is the only link between your table and the capability list.
```cpp
int SwitchBCM56860HW::hwPortPstatOf(_In_ sai_stat_id_t id)
{
    return hwPstatOf(id);
}
```
Change `getStatsExt` like this (the new lines are marked `NEW`):
```cpp
    SWSS_LOG_ENTER();

    sai_status_t hwStatus;                                              // NEW
    if (hwGetObjectStats(object_type, object_id, number_of_counters,   // NEW
                counter_ids, mode, counters, hwStatus))                 // NEW
    {                                                                   // NEW
        return hwStatus;                                                // NEW
    }                                                                   // NEW

    int bcm = (object_type == SAI_OBJECT_TYPE_PORT && m_hwReady) ? bcmPortOf(object_id) : -1;
    ... unchanged up to and including the hwbcm_port_stat_multi_get error check ...

    sai_status_t xs = hwPortStatsOverlay(bcm, number_of_counters,      // NEW
            counter_ids, mode, counters);                               // NEW
    if (xs != SAI_STATUS_SUCCESS)                                       // NEW
    {                                                                   // NEW
        return xs;                                                      // NEW
    }                                                                   // NEW

    if (mode == SAI_STATS_MODE_READ_AND_CLEAR) { ... unchanged ... }
```
- `hwGetObjectStats` returns false for anything it does not own: HW not ready, the unit-test MSB "set counters" call, PORT and other object types, or an object without a HW port. In those cases the existing path runs unchanged.
- The overlay only overwrites ids your table reads as `HWBCM_PSTAT_NONE`. There is no overlap today.

Optional but recommended: in `hwInitialize()`, right after `hwbcm_init()` succeeds, add `hwbcm_stats_init();` and `#include "hwbcm/hwbcm_stats.h"`. Otherwise BST is enabled lazily on the first stats call. That works, but the first watermark poll then only covers the time since that call.

### c) `sonic-sairedis/vslib/Makefile.am` (`:56-58`, `:77-80`)
Add `SwitchBCM56860HW_stats.cpp` to the C++ list and `hwbcm/hwbcm_stats.c` to the hwbcm list (it picks up `libSaiVS_a_CFLAGS` = SDK flags). Then sync with `sairedis/sync_from_sairedis.sh` / `vslib.patch` the way the other files are synced.

### d) Extra port-stat mappings
No new lines are needed in your `hwPstatOf` / `pstat_map` tables. The extra port counters live in `g_portXStats` (C++) and `HWBCM_XPSTAT_*` (C), because half of them are cosq/MMU counters and not `bcm_stat_val_t`:

| SAI | Source |
|---|---|
| `IN_DROPPED_PKTS` | `bcm_cosq_stat_get(port, -1, bcmCosqStatSourcePortDroppedPackets)`: MMU_CTR_ING_DROP_MEM (`trident2/cosq.c:11415-11431`) |
| `OUT_DROPPED_PKTS` | `bcm_cosq_stat_get(port, -1, bcmCosqStatDroppedPackets)`: all UC+MC queue drops (`cosq.c:10364-10397`) |
| `GREEN/YELLOW/RED_WRED_DROPPED_PACKETS` | `bcmCosqStat{Green,Yellow,Red}DiscardDroppedPackets(port, -1)` |
| `WRED_DROPPED_PACKETS` | sum of the three above |
| `ETHER_STATS_PKTS_1519_TO_2047` | `snmpBcmEtherStatsPkts1519to1522Octets + snmpBcmEtherStatsPkts1522to2047Octets` |
| `ETHER_STATS_PKTS_2048_TO_4095` / `4096_TO_9216` / `9217_TO_16383` | `snmpBcmEtherStatsPkts2048to4095Octets` / `4095to9216Octets` / `9217to16383Octets` |
| `IP_IN_RECEIVES`, `IP_IN_DISCARDS` | `snmpIpInReceives`, `snmpIpInDiscards` |
| `IPV6_IN_RECEIVES`, `IPV6_IN_DISCARDS` | `snmpIpv6IfStatsInReceives`, `snmpIpv6IfStatsInDiscards` |

If you would rather keep every port counter in one table, the four EtherStats and four IP rows can move into `pstat_map` instead. `NON_UCAST`-style summing already exists there for the 1519 bucket. The cosq rows cannot move.

---

## 3. Design

### Object to hardware lookup
Built only from the vslib object hash. It does not depend on the QoS files.

- **QUEUE:** `SAI_QUEUE_ATTR_PORT`, `INDEX` and `TYPE` give `(bcm port via bcmPortOf, cos, mc)`.
  - UC index is 0..9. MC index is 10..19, mapped to cos = index − `m_unicastQueueNumber` (`SwitchBCM56850.cpp:31-75`, `SwitchBCM56850.h:59-60`).
  - A queue on `m_cpu_port_id` maps to `(CMIC_PORT, index, mc)`.
  - This matches what orchagent reads in `getQueueTypeAndIndex` (`portsorch.cpp:3377`).
- **PG:** `SAI_INGRESS_PRIORITY_GROUP_ATTR_PORT` and `INDEX` give `(bcm port, pg)`.
- **BUFFER_POOL:** `SAI_BUFFER_POOL_ATTR_TYPE` gives the direction. The code then finds one user of the pool: a PG (ingress) or a non-CPU queue (egress) whose `BUFFER_PROFILE(_ID)` has `SAI_BUFFER_PROFILE_ATTR_POOL_ID == pool`. The user is cached in `m_statPoolUser` and re-validated on every call.
  - The service pool number is read back from the hardware through that user: `bcmCosqControlIngressPool(port, pg)` (THDI_PORT_PG_SPID), or `bcmCosqControlUC/MCEgressPool(port, q)`.
  - So whatever service-pool assignment the QoS code programs is what gets reported. If the QoS agent ends up with an explicit `pool oid → SP` map, `hwStatsPoolRef` is the one function to replace.
  - Egress BST (`bcmBstStatIdEgrPool`) is indexed through a port's MC queue (STYLE_EGR_POOL, `cosq.c:2329-2359`, `7472-7478`). `hwbcm_stats.c:egr_pool_rep` finds a `(port, mc cos)` on that SP and caches it per SP.

### Counter sources (all under one mutex in C)

**Queue packets and bytes**
- Front panel: `bcmCosqStat{UC,MC}Out{Packets,Bytes}(port, cos)` (`cosq.c:10710-10833`).
- CPU: `bcmCosqStatOut*`.

**Queue drops**
- Front panel: the public API only gives the UC+MC sum per cos. The UC/MC split is read the way the SDK does it internally for a plain port: `soc_counter_get(port, SOC_COUNTER_NON_DMA_COSQ_DROP_{PKT,BYTE}[_UC], cos)` (`cosq.c:10340-10372`, index style `cosq.c:2383-2407`, counter base `common/counter.c:1125-1146`).
- CPU: `bcmCosqStatDropped*`.

**Queue occupancy**
- `CURR_OCCUPANCY` = `bcmCosqStatEgress{UC,MC}QueueMinBytesCurrent + …QueueBytesCurrent`. These are live MMU table reads (`cosq.c:11229-11414`).
- `SHARED_CURR_OCCUPANCY` = the shared part only.

**PG occupancy**
- `IngressPortPG{Min,Shared,Headroom}BytesCurrent`, read by the counter thread.

**Watermarks**
- BST, with `bcmSwitchBstTrackingMode = peak` and `bcmSwitchBstEnable = 1` (`esw/bst.c:_bcm_td2_bst_control_set`). In peak mode the software cache keeps the maximum over syncs (`bst.c:3718-3724`).
- `bcm_cosq_bst_stat_sync(bid)` runs at most every 500 ms per resource.
- READ_AND_CLEAR uses `BCM_COSQ_BST_STAT_CLEAR_ON_READ` (`bst.c:3090-3104`).
- Resources:
  - Queues: Ucast / Mcast.
  - PGs: PriGroupShared / PriGroupHeadroom.
  - Pools: IngPool / EgrPool (`MMU_THDM_DB_POOL_MCUC_BST_STAT`, UC+MC, `bst.c:1546-1557`).
- BST values are cells. Bytes = cells × 208 (`trident2/cosq.c:49`).

**PG addressing**
- PG BST and PG stats take a *priority* as cosq and map it to a PG through THDI_PORT_PRI_GRP (`cosq.c:7388-7430`; BST resolve `cosq.c:7531-7539`). `hwbcm_pg_stat_multi_get` therefore looks up the lowest priority with `bcm_port_priority_group_mapping_get(...) == pg`.
- A PG that no priority maps to reads as 0. On default hardware every priority maps to PG 7.

**READ_AND_CLEAR of cumulative counters**
- Implemented with software baselines (`hw − base`, then `base = hw`). The hardware is never cleared, because the counters are shared: a queue drop is also part of the port's `OUT_DROPPED_PKTS`. orchagent uses READ mode for all of these groups anyway (`portsorch.cpp:572-583`). Clear only happens through `clearStats`.

**Per-call memo**
- Each hardware source is read once per call, so `WATERMARK` and `SHARED_WATERMARK` in one call do not clear each other.

**Unsupported counters**
- An id that is not in the table returns `SAI_STATUS_NOT_SUPPORTED`. A counter the SDK refuses for one specific object (UNAVAIL, PARAM, NOT_FOUND) reads as 0, with a warning logged at most 32 times.

### What `queryStatsCapability` reports
Both modes (READ and READ_AND_CLEAR) for every entry.

| Object | Advertised |
|---|---|
| PORT | every id in `hwPstatOf`, plus `g_portXStats` |
| QUEUE | `PACKETS`, `BYTES`, `DROPPED_PACKETS`, `DROPPED_BYTES`, `CURR_OCCUPANCY_{BYTES,CELLS}`, `SHARED_CURR_OCCUPANCY_{BYTES,CELLS}`, `WATERMARK_{BYTES,CELLS}`, `SHARED_WATERMARK_{BYTES,CELLS}` |
| INGRESS_PRIORITY_GROUP | `CURR_OCCUPANCY`, `SHARED_CURR_OCCUPANCY`, `XOFF_ROOM_CURR_OCCUPANCY`, `SHARED_WATERMARK`, `XOFF_ROOM_WATERMARK` (bytes and cells) |
| BUFFER_POOL | `WATERMARK_{BYTES,CELLS}` |

- Other object types go to the base class.
- QUEUE, PG and BUFFER_POOL contexts all use the capability query (`FlexCounter.cpp:2767-2787`, `2829-2834`). Before this change, PG and BUFFER_POOL fell back to trying each counter against vslib, which "succeeded" with zeros for everything.

---

## 4. Parity gaps vs Broadcom SAI (known, by design or by chip)

- **PG `PACKETS` / `BYTES` / `DROPPED_PACKETS`:** TD2 has no per-PG packet or drop counter. There is nothing for it in `bcm_td2_cosq_stat_get`, `cosq.c:10316-11431`. So `show priority-group drop counters` stays N/A, and the PFC watchdog's PG "drop" counters (`pfcactionhandler.cpp:609-610`) are absent.
- **PG `WATERMARK_BYTES` (total):** not provided. Only shared and headroom watermarks exist in BST.
- **Queue `WATERMARK_BYTES` and `SHARED_WATERMARK_BYTES`:** both come from the same BST queue entry (THDU / THDM `*_BST_QUEUE`, `bst.c:1703-1750`), which tracks the queue's total use. I believe Broadcom SAI also uses this BST resource, but I have not verified that. SONiC only reads `SHARED_WATERMARK` (`watermarkorch.cpp:199-211`).
- **Per-queue WRED / ECN (`SAI_QUEUE_STAT_WRED_*`) and port WRED bytes / ECN marked:** the chip only has per-port, per-colour WRED drop *packets*.
- **Pool `XOFF_ROOM_WATERMARK_BYTES` and `CURR_OCCUPANCY`:** not provided. TD2 has no shared headroom pool, and there is no public egress-pool current API. `show headroom-pool watermark` is N/A.
- **PFC pause duration and `PFC_x_ON2OFF_RX_PKTS`, FEC counters, `IP_IN_UCAST_PKTS`:** not provided.
- **CPU queues:** packets, bytes and drops work. The BST Mcast watermark of CPU queues may come back UNAVAIL and then reads 0.
- **ETS hierarchy:** the drop split and BST indexes assume the plain-port queue layout (SAI queue i = hardware cos i). If the QoS code builds a scheduler hierarchy with `bcm_cosq_gport_add` (ets_mode) *and* attaches queue nodes out of order, the counts would land on the wrong SAI queue. In that case pass UC/MC queue-group gports instead of `(port, cos)`. The SDK supports gports in every path used here. Add a `hwbcm_stats_queue_gport_set()` registration and merge with the QoS oid→gport map.
- **PFC watchdog on the `vs` platform:** it polls `SAI_QUEUE_STAT_PACKETS`, `CURR_OCCUPANCY_BYTES` and PFC RX counters (`orchdaemon.cpp:601-627`). These are now real values, so the watchdog's storm detection behaves differently than it did on zeros.

---

## 5. Hardware test plan

Setup: `counterpoll show`, then enable what is off: `counterpoll queue enable`, `counterpoll watermark enable`, `counterpoll port-buffer-drop enable`. Commands below run on the switch unless marked `bsh` (`deploy/bsh "<diag cmd>"`).

1. **Capability is in effect.**
   - syncd log: `stats capability SAI_OBJECT_TYPE_PORT: N counters` (and QUEUE / PG / BUFFER_POOL), plus `stats: init num_cos 10 cpu_cos 48 bst on (peak)`.
   - `redis-cli -n 2 HKEYS COUNTERS:<Ethernet0 oid> | grep -c ETHER_IN_PKTS` should be 10 (was 0). Same check for `ETHER_OUT_PKTS`, `PAUSE_RX_PKTS` and `SAI_PORT_STAT_OUT_DROPPED_PKTS`.
2. **Size buckets.** From the PC, run `ping -s 1400 -c 100 <switch ip on Ethernet0>`, then `show interfaces counters detailed Ethernet0`.
   - "Packets Received 1024-1518 Octets" should go up by 100 and the transmitted bucket by 100 (echo replies).
   - `ping -s 3000` (needs MTU 9100 on both sides) should move the 2048-4095 bucket.
   - Cross-check with `bsh "show c xe0"` (the per-size MIB counters of that port).
3. **Queue counters.** `sonic-clear queuecounters`, send unicast traffic out of Ethernet0, then `show queue counters Ethernet0`.
   - Only the UC queue that DSCP/TC maps to should increase (UC0 when no QoS map is set).
   - Broadcast ARP or flooding should increase MC10.
   - Cross-check with `bsh "show c UC_PERQ_PKT"` / `MC_PERQ_PKT` (counter names: `common/counter.c:5321`, `5348`).
4. **Drops.** Force congestion: Ethernet1 + Ethernet2 at 10G in, Ethernet0 at 1G out (`config interface speed Ethernet0 1000`), iperf UDP 2 × 900M.
   - `show queue counters Ethernet0`: the Drop/pkts column increases.
   - `redis-cli -n 2 HGET COUNTERS:<oid> SAI_PORT_STAT_OUT_DROPPED_PKTS` should equal the sum of that port's queue drops.
   - `bsh "show c UCQ_DROP_PKT"` / `MCQ_DROP_PKT`.
   - Ingress-side drops (`IN_DROPPED_PKTS`, `DROP_PKT_ING`) only appear when the ingress PG / pool limit is hit.
5. **Watermarks**, during step 4 and after ≥ 60 s (the watermark poll interval):
   - `show queue watermark unicast`: the congested queue is non-zero and a multiple of 208.
   - `show queue persistent-watermark unicast` keeps the peak; `sonic-clear queue persistent-watermark unicast` resets it.
   - `show priority-group watermark shared` / `headroom`: non-zero for the ingress ports' PG (PG 7 unless a PFC pri→PG map is configured).
   - `show buffer_pool watermark`: the pools that BUFFER_PROFILE ties to queues / PGs are non-zero. syncd DEBUG shows `pool …: egress via port X cos Y -> service pool N`.
   - Cross-check the SP mapping with `bsh "g THDI_PORT_PG_SPID xe1"`.
6. **Occupancy**, while traffic is running: `redis-cli -n 2 HGET COUNTERS:<queue oid> SAI_QUEUE_STAT_CURR_OCCUPANCY_BYTES`. This is only present if a group polls it, for example PFC watchdog: `pfcwd start --action drop Ethernet0 200`. It should be non-zero under congestion and 0 when idle.
7. **Negative checks.**
   - `show priority-group drop counters` → N/A (expected gap).
   - With `HWBCM_STATS_NO_BST=1` in syncd's environment, all watermarks read 0 and nothing else changes. This is the escape hatch if BST misbehaves.

---

## 6. Open risks

- **Not run on hardware.** In particular:
  - That `bcmSwitchBstEnable` with the external thread operation (it starts the BST sync thread and maxes the thresholds) is benign next to KNET and linkscan.
  - That TD2+ fills the `PG_*_CURRENT` counter-thread values with the counter interval in our `config.bcm`.
- **Representative-user pool lookup:** a pool that no profile references reads 0, which is correct, but a pool used only by a profile that is not yet attached also reads 0 until it is attached.
- **SDK internals:** the C layer uses `soc_counter_get` with `SOC_COUNTER_NON_DMA_*` ids (exported, but not `bcm_*` public API), only for the UC/MC drop split.

---

## 7. Merge with the QoS agent (sai-qos, 2026-11-24)

- **Queue and PG handles agree.** The QoS agent stays in SDK non-ETS mode: no cosq gports are created, and `(bcm port, cos)` is the handle. That matches the plain-port paths used here, so the ETS-hierarchy risk in §4 does not apply as long as they stay in that mode.
  - Their `qosQueueHwInfo(queue_oid, QosQueueHw&)` and `qosPgHwInfo(pg_oid, QosPgHw&)` (`SwitchBCM56860HW_qos.inc.h`) use the same convention as `hwStatsQueueRef` / `hwStatsPgRef`.
  - After the merge, both stats lookups can become thin wrappers over those accessors. No behaviour changes.
- **Only cos 0..7 exist.** `bcm_num_cos=10` in `config.bcm` is clamped to 8 at `trident2/cosq.c:8046-8054` (`NUM_COS(unit) = min(numq, 8)`).
  - `hwbcm_stats_init` takes `g_num_cos` from `bcm_cosq_config_get`, so it is 8.
  - SAI queues UC8/UC9 and MC18/MC19 exist in vslib but have no hardware queue, and read as 0 for every counter. This is expected, not a bug.
- **Buffer pool service pools.** QoS assigns SPs in creation order per direction and keeps them in `m_qosPools[pool].ingressSp` / `.egressSp`.
  - `hwStatsPoolRef` instead reads the SP back from the hardware through a PG or queue that uses the pool. Both methods should give the same number. The syncd DEBUG line `-> service pool N` lets you check this on hardware.
  - BST still has to be addressed through a `(port, cos)` (`bst.c` resolve), so the representative lookup stays even if the SP is taken from `m_qosPools`.
