# Integration notes: QoS and buffers (QOS_MAP, SCHEDULER, WRED, BUFFER_*)

Owner: QoS agent, 2026-11-24. All files are new; no existing file was edited.

| File | What it is |
|---|---|
| `sairedis/vslib/hwbcm/hwbcm_qos.h` | SDK-free C API (`hwbcm_qos_*`): classification maps, TC→cos, TC→PG, PFC, L1 scheduling/shaping, port shaper, WRED/ECN, PG and queue buffer thresholds |
| `sairedis/vslib/hwbcm/hwbcm_qos.c` | Implementation (bcm_qos_*, bcm_cosq_*, bcm_port_*; direct MMU access only where the SDK has no non-ETS API) |
| `sairedis/vslib/SwitchBCM56860HW_qos.cpp` | SAI handlers, oid→HW maps, desired-state bindings, re-apply on profile change |
| `sairedis/vslib/SwitchBCM56860HW_qos.inc.h` | Member declarations; `#include` inside the class body |

Syntax checks (in `saibuild`):
- C: the brief's command (`gcc -std=gnu99 -fsyntax-only` + sdk_cflags/sdk_incs, `-DBCM_HIDE_DISPATCHABLE` removed): no output.
  With `-Wall -Wextra` the only warning is in the SDK header `soc/trident2.h:836`.
- C++: `vslib_cxx_cmd.txt` + `-fsyntax-only` (includes `-Werror -Wconversion -Wshadow ...`), from
  `/w/sonic-sairedis/vslib`, scratch copy of `SwitchBCM56860HW.h` first on `-I`: clean. It is also clean
  with `_acl.inc.h`, `_copp.inc.h`, `_stats.inc.h` and `_qos.inc.h` all included, so the member names do not collide.

---

## 1. Wiring (lead)

### a) `SwitchBCM56860HW.h`
Last line inside the class body, after the stats include:
```cpp
#include "SwitchBCM56860HW_qos.inc.h"
```
It opens its own `public:`/`protected:` and leaves the class in `protected:`.

### b) `SwitchBCM56860HW.cpp`

1. `initialize_default_objects`: right after `CHECK_STATUS(SwitchBCM56850::initialize_default_objects(...));`
   ```cpp
   (void)qosBuildHwMap();
   ```
   Queues, scheduler groups and PGs are created by the base class after `create_ports()`, so the map
   cannot be built from `buildPortMaps`.
2. `warm_boot_initialize_objects`: replace `return buildPortMaps(true);` with
   ```cpp
   CHECK_STATUS(buildPortMaps(true));
   return qosBuildHwMap();
   ```
   `qosBuildHwMap` also reads the QoS bindings back from the object hash and re-programs them, because the SDK came up cold.
3. `hwSetPort`: right after the `attr == NULL` check, before `bcmPortOf`:
   ```cpp
   if (qosIsPortAttr(attr->id))
   {
       return qosHwSetPort(port_id, attr);
   }
   ```
   `qosHwSetPort` itself returns SUCCESS for the CPU port and unmapped ports. It owns the QoS map attributes,
   `QOS_DEFAULT_TC`, `QOS_SCHEDULER_PROFILE_ID`, `PRIORITY_FLOW_CONTROL(_MODE/_RX/_TX)`,
   `QOS_INGRESS/EGRESS_BUFFER_PROFILE_LIST` and `QOS_TC_AND_COLOR_TO_DOT1P_MAP`. It does not own `QOS_QUEUE_LIST`,
   `QOS_SCHEDULER_GROUP_LIST` or `INGRESS_PRIORITY_GROUP_LIST`; those stay with you.
4. `hwCreatePort`: right after `mapPort(port_id, bcm, lane);`
   ```cpp
   qosOnPortMapped(port_id);
   ```
   The base class already made that port's queues, SGs and PGs (`create_port_dependencies`) before the HW dispatch runs.
5. `hwRemovePort`: right before `unmapPort(port_id);`
   ```cpp
   qosOnPortRemoved(port_id);
   ```
6. `hwSetSwitch`: before its `switch (attr->id)`:
   ```cpp
   if (qosIsSwitchAttr(attr->id))
   {
       return qosHwSetSwitch(attr);
   }
   ```
   This covers `SAI_SWITCH_ATTR_QOS_{DSCP,DOT1P}_TO_{TC,COLOR}_MAP`, `TC_TO_QUEUE_MAP`, `TC_AND_COLOR_TO_DSCP_MAP`
   and `QOS_DEFAULT_TC`. A switch-level map is the fallback for ports that have no port-level map of the same kind.
   qosorch uses `SAI_SWITCH_ATTR_QOS_DSCP_TO_TC_MAP` for the "global" DSCP_TO_TC map.
7. `create()` switch:
   ```cpp
   // QoS / buffers (A6)
   case SAI_OBJECT_TYPE_QOS_MAP:       hw = hwCreateQosMap(oid, attr_count, attr_list); break;
   case SAI_OBJECT_TYPE_SCHEDULER:     hw = hwCreateScheduler(oid, attr_count, attr_list); break;
   case SAI_OBJECT_TYPE_WRED:          hw = hwCreateWred(oid, attr_count, attr_list); break;
   case SAI_OBJECT_TYPE_BUFFER_POOL:   hw = hwCreateBufferPool(oid, attr_count, attr_list); break;
   case SAI_OBJECT_TYPE_BUFFER_PROFILE:hw = hwCreateBufferProfile(oid, attr_count, attr_list); break;
   ```
   These handlers read the attributes back from the object hash, because the base create has already run.
   `hwCreateBufferPool` returns `SAI_STATUS_INSUFFICIENT_RESOURCES` when all 4 service pools of a direction are used; your rollback then removes the object.
8. `remove()` switch:
   ```cpp
   case SAI_OBJECT_TYPE_QOS_MAP:        hw = hwRemoveQosMap(oid); break;
   case SAI_OBJECT_TYPE_SCHEDULER:      hw = hwRemoveScheduler(oid); break;
   case SAI_OBJECT_TYPE_WRED:           hw = hwRemoveWred(oid); break;
   case SAI_OBJECT_TYPE_BUFFER_POOL:    hw = hwRemoveBufferPool(oid); break;
   case SAI_OBJECT_TYPE_BUFFER_PROFILE: hw = hwRemoveBufferProfile(oid); break;
   ```
9. `set()` switch:
   ```cpp
   case SAI_OBJECT_TYPE_QOS_MAP:                hw = hwSetQosMap(oid, attr); break;
   case SAI_OBJECT_TYPE_SCHEDULER:              hw = hwSetScheduler(oid, attr); break;
   case SAI_OBJECT_TYPE_WRED:                   hw = hwSetWred(oid, attr); break;
   case SAI_OBJECT_TYPE_BUFFER_POOL:            hw = hwSetBufferPool(oid, attr); break;
   case SAI_OBJECT_TYPE_BUFFER_PROFILE:         hw = hwSetBufferProfile(oid, attr); break;
   case SAI_OBJECT_TYPE_QUEUE:                  hw = hwSetQueue(oid, attr); break;
   case SAI_OBJECT_TYPE_SCHEDULER_GROUP:        hw = hwSetSchedulerGroup(oid, attr); break;
   case SAI_OBJECT_TYPE_INGRESS_PRIORITY_GROUP: hw = hwSetIngressPriorityGroup(oid, attr); break;
   ```
   During init, the base class `set`s QUEUE TYPE/INDEX/PORT and SG CHILD_LIST/PORT_ID through this dispatch while
   `m_hwReady` is already true. The handlers ignore everything except the profile attributes.

### c) `vslib/Makefile.am` (and regenerate `sairedis/vslib.patch`)
Add these lines to `libSaiVS_a_SOURCES`:
```
					  SwitchBCM56860HW_qos.cpp \
					  hwbcm/hwbcm_qos.c \
```
`hwbcm_qos.c` needs nothing beyond the existing `libSaiVS_a_CFLAGS` (SDK flags + `-I$(srcdir)/hwbcm`). It includes
`soc/drv.h`, `soc/mem.h`, `soc/trident2.h` as well as the bcm headers.

### d) config.bcm
Nothing is required. Current state and one option to test later (each is a separate change, test one at a time):
- `bcm_num_cos=10` stays. `bcm_td2_cosq_init` clamps it to 8 (`src/bcm/esw/trident2/cosq.c:8046-8052`). That limit is why queues 8/9 are unreachable through the API (§3).
- `mmu_lossless` is **not set**, so it defaults to **1** (`src/soc/esw/trident2.c:10971`, `15971`).
  In that mode init mmu leaves egress queues with discard disabled (no per-queue tail drop) and makes PG7 (all
  priorities) lossless with headroom and flow control (`trident2.c:14203-14232` PG7, `14254-14262` queues). SONiC's
  buffer model assumes the opposite default: lossy, with lossless only where BUFFER_PG says so.
  This code works in both modes:
  - Queues that get a SONiC profile get `QueueLimitEnable=1`.
  - Priorities moved off PG7 inherit PG7's shared threshold (§2.6).
  - PFC TX is rewritten from the SAI bitmap.

  **Option to test:** `mmu_lossless=0`, which is closer to what SONiC expects. Do not change it together with this feature.

---

## 2. Design and why

1. **Non-ETS scheduler mode (the big decision).** The SDK default tree per port is root → L0.0 → L1.0..L1.9, and
   each L1 has one UC queue and one MC queue (`src/soc/esw/trident2/cosq.c:1228-1360`). With no queue gports, the bcm_cosq API takes
   `(local port, cos)` and resolves it to L1.cos (scheduling, MTRO shaper) or to UC queue cos (WRED, thresholds)
   (`src/bcm/esw/trident2/cosq.c:2076-2290` `_bcm_td2_cosq_index_resolve`, `6800-6860` `_bcm_td2_cosq_sched_set`).
   The first `bcm_cosq_gport_add` switches the whole chip to ETS mode. It runs `soc_td2_lls_reset()` (every port,
   **including the CPU port**) and leaves rebuilding every tree to the caller (`cosq.c:4341-4347`). That would put the
   working KNET/CoPP CPU path at risk. The README also notes an LLS L0 node limit on TD2+. So ETS is not used.
2. **oid → HW map.** `qosBuildHwMap` / `qosOnPortMapped` read `SAI_PORT_ATTR_QOS_QUEUE_LIST`, `QOS_SCHEDULER_GROUP_LIST`
   and `INGRESS_PRIORITY_GROUP_LIST` from the object hash:
   - **Queue.** UC queue index i maps to cos i. MC queue index 10+i maps to cos i (`SwitchBCM56850::create_qos_queues_per_port`). CPU queues 0..31 map to cos 0..31.
   - **Scheduler group.** The role is found from the tree, not by index:
     - a group with queue children is L1 and takes that queue's cos;
     - a group that is nobody's child is the root;
     - every other group is L0.
   - **PG.** `SAI_INGRESS_PRIORITY_GROUP_ATTR_INDEX` gives PG 0..7.
3. **Schedulers.** qosorch puts the queue's scheduler on its parent SG (`qosorch.cpp:1629-1691`), which is L1.cos here.
   That becomes `bcm_cosq_gport_sched_set(port, cos, STRICT|WRR|DWRR, weight)` plus
   `bcm_cosq_gport_bandwidth_set(port, cos, min_kbps, max_kbps)`, with bursts set through
   `bcm_cosq_control_set(BandwidthBurstMax/Min)` (`src/bcm/esw/cosq.c:3700-3710`). SAI rates are bytes/s and are converted to kbit/s,
   rounding up. `METER_TYPE_PACKETS` sets `BCM_COSQ_BW_PACKET_MODE`.
   - A scheduler set directly on a queue programs the same L1 node.
   - A scheduler on the root SG, or `SAI_PORT_ATTR_QOS_SCHEDULER_PROFILE_ID`, becomes the port shaper (`bcm_port_rate_egress_set`, or `_pps_set`).
   - A NULL scheduler restores WRR weight 1 with no shaping (the SDK default).
4. **QoS maps.** Map content is resolved per port: the port map if there is one, otherwise the switch map. One bcm qos
   map exists per distinct content, refcounted (`m_qosHwMaps`). DSCP_TABLE / ING_PRI_CNG_MAP are shared profiles with few
   ids, so 72 ports with the same map use one id.
   - DSCP_TO_TC + DSCP_TO_COLOR become one L3 ingress map (PORT_TAB.TRUST_DSCP_PTR).
   - DOT1P_TO_TC + DOT1P_TO_COLOR become one L2 ingress map (TRUST_DOT1P_PTR).
   - TC_AND_COLOR_TO_DSCP becomes an L3 egress map (EGR_PORT.EGR_QOS_PROFILE_INDEX on TD2+, `triumph2/qos.c:5939-5950`).
     A TC entry given only for green also fills yellow/red.
   - Rebinding always clears first, because `bcm_qos_port_map_set(port, 0, -1)` clears both trust pointers (`triumph2/qos.c:5623-5666`).
   - TC_TO_QUEUE uses `bcm_cosq_port_mapping_set(port, tc, cos)` (PORT_COS_MAP, both UC and MC).
   - TC_TO_PG uses `bcm_port_priority_group_mapping_set` (THDI_PORT_PRI_GRP).
   - DEFAULT_TC uses `bcm_port_untagged_priority_set`.
5. **PFC.**
   - **TX:** the SAI priority bitmap is written to THDI_INPUT_PORT_XON_ENABLES.PORT_PRI_XON_ENABLE, and the MAC
     transmitter is turned on or off, through `bcm_port_priority_group_config_set(pfc_transmit_enable)` for each bit
     (`trident2/cosq.c:18866-18880`, `port.c:38645-38750`).
   - **RX:** `bcm_cosq_pfc_class_mapping_set` maps class p to cos `PFC_PRIORITY_TO_QUEUE[p]` (identity if no map is bound),
     only for priorities enabled for RX. Other classes pause nothing. Then `bcmPortControlPFCReceive` is set.
   - `PRIORITY_FLOW_CONTROL_MODE=SEPARATE` with `_TX`/`_RX` is honoured.
6. **Buffers.**
   - **Service pools.** BUFFER_POOL gets an SP in creation order per direction; the first ingress and the first egress pool both get SP0.
   - **PG profile:**
     - `RESERVED_BUFFER_SIZE` → PG_MIN; `XOFF_TH` → PG_HDRM (headroom).
     - `XON_TH` → PG_RESET_FLOOR; `XON_OFFSET_TH` → PG_RESET_OFFSET. If the offset is 0, the PG keeps its own resume offset, or PG7's if it has none.
     - Dynamic `SHARED_DYNAMIC_TH` n → PG_SHARED_DYNAMIC with alpha 2^n, clamped to 1/128..8. Static → PG_SHARED_LIMIT cells.
     - `POOL_ID` → THDI_PORT_PG_SPID.
     - **Min and headroom go through `bcm_cosq_control_set`.** Only that path moves the delta into or out of the SP shared
       limit (`trident2/cosq.c:13741-13800`), so the pool stays consistent with no headroom maths of our own. That API is keyed by
       *priority*, not PG (`cosq.c:13651-13660`). If no priority maps to the PG yet, the C layer returns
       `BCM_E_NOT_FOUND`. The C++ side then marks the PG pending and retries after every TC_TO_PG change.
     - The admission fields (dynamic / limit / floor / offset) are written directly, the same fields that init mmu writes (`trident2.c:15130-15160`).
     - **Inherit PG7.** Init mmu leaves PG0..6 with min 0 and static shared 0, so they admit nothing (`trident2.c:14203-14215`).
       Before a priority is moved to such a PG, the PG gets PG7's shared threshold. Otherwise traffic would drop in the window
       between qosorch binding TC_TO_PG and bufferorch binding the PG profile.
   - **Queue profile:**
     - min → `Egress{UC,MC}QueueMinLimitBytes` (the SDK rebalances the pool).
     - static → `...SharedLimitBytes`. If the value is larger than the field (`egress_lossless_profile`'s static_th = pool size), the queue limit is disabled instead.
     - dynamic → `...SharedDynamicEnable` plus the alpha written straight to MMU_THDU/THDM (`cosq.c:14666-14717` needs a queue gport, so its memory writes are mirrored).
     - pool → `{UC,MC}EgressPool`.
     - `...QueueLimitEnable=1`.
   - **NULL profile.** The PG or queue goes back to the values snapshotted at first touch, which is the init-mmu config.
7. **WRED.** `bcm_cosq_gport_discard_set(port, cos)` runs once per color, with TCP and non-TCP curves equal.
   - A color whose curve is off gets the 0x1ffff-cell "unused" sentinel (`cosq.c:6100-6117`).
   - `ECN_MARK_MODE != NONE` sets `BCM_COSQ_DISCARD_MARK_CONGESTION`, and a color that only marks gets its curve anyway.
   - `WEIGHT` → gain.
   - NULL disables WRED and ECN on the queue.
8. **Desired state.** Every binding (SG/queue → scheduler, queue → WRED, PG/queue → buffer profile, port → maps / PFC / shaper)
   is kept, and a set on a profile or map re-applies it to all of its users.

### For the statistics agent
- `qosQueueHwInfo(queue_oid, QosQueueHw&)` gives `{port oid, bcmPort, cpu, mcast, cos}` (front port: cos 0..9; CPU: cos 0..31).
- `qosPgHwInfo(pg_oid, QosPgHw&)` gives `{bcmPort, pg}`.
- `qosSgHwInfo` gives `{level, index}`.
- There are **no cosq gports**. The handle is `(bcmPort, cos)`, and the bcm_cosq API reaches cos 0..7 only.
- `m_qosPools[pool_oid].ingressSp/egressSp` is the SP of a pool.

This was sent to sai-counters; their `hwStatsQueueRef` uses the same cos convention.

---

## 3. Not at parity with Broadcom SAI (explicit)

| Area | State | Why / what it would take |
|---|---|---|
| Scheduler hierarchy | vslib reports 13 SGs (root, 2×L0, 10×L1). HW has root → **one** L0 → 10 L1. Scheduler on an L0 SG: ignored (WARN). | Non-ETS mode (§2.1). Parity needs an ETS tree for every port + CPU + LB rebuilt with `bcm_cosq_gport_add/attach`. |
| Queues 8 and 9 | Scheduler, WRED and buffer profiles on them are ignored (WARN). TC→queue ≥8 is clamped to 7. | bcm_num_cos is clamped to 8 (`cosq.c:8046`). SONiC's stock qos.json only uses 0..7 (6). |
| SP + WRR mixing | Passed through to the SDK. Not checked against LLS strict-priority contiguity rules. | Test it (§4). |
| Buffer pool size | `SAI_BUFFER_POOL_ATTR_SIZE` is **recorded, not programmed**. The SP shared limits stay as init mmu set them, minus or plus the SDK's own carve-outs for min/headroom. The pool create logs the SAI size next to the HW shared limit. | Programming sizes needs a full MMU budget: total cells, port-SP / global headroom, the 10-cell lossless overshoot (`trident2.c:13873-13889`). A wrong value black-holes traffic. |
| `SAI_BUFFER_POOL_ATTR_SHARED_SIZE` (read-only) | Returns the vslib value, not HW. | Could read `hwbcm_qos_pool_shared_get()`; needs a `get` hook in A2's dispatch. |
| Dynamic buffer model (buffermgrd `dynamic`) | Not supported. Headroom is SONiC's static `XOFF_TH` (pg_profile_lookup.ini). There is no own headroom calculation and no `SAI_PORT_ATTR_QOS_MAXIMUM_HEADROOM_SIZE`. | The traditional (static) model is what Broadcom platforms run. |
| Shared headroom pool (`XOFF_SIZE`) | WARN, not programmed. PG headroom is per PG (`PG_GBL_HDRM_EN` left as init set it). | TD2+ global headroom (`THDI_GLOBAL_HDRM_LIMIT`) is not modelled. |
| `QOS_INGRESS/EGRESS_BUFFER_PROFILE_LIST` (port-SP min/max) | WARN, not programmed. | THDI_PORT_SP_CONFIG via `IngressPortPool{Min,Max}LimitBytes`, also keyed by priority. |
| PFC watchdog | `SAI_QUEUE_ATTR_PAUSE_STATUS` comes from vslib (always false). `PFC_DLR_INIT` / `ENABLE_PFC_DLDR` are logged only. | pfcwd detection reads the queue pause state (`bcm_cosq_pause_queue_status_get` exists, `trident2/cosq.c:22677`; better done in the stats/get path). Recovery needs ACL drop (pfcwd ACL handler) or DLR. |
| PFC priority → PG | Identity only (WARN otherwise). | TD2+ has no separate PFC-priority→PG table. The PG only decides when to send xoff. |
| ECN | Per queue (ECN_MARKING_EN), not per color. The separate `ECN_*_THRESHOLD` attributes are ignored and the color thresholds are used. WRED applies to UC queues only. | HW / SDK limits (`cosq.c:6132-6141`). |
| `TC_AND_COLOR_TO_DOT1P`, MPLS EXP, forwarding-class maps | Not programmed (WARN). | On TD2+ the combo egress profile only takes the DSCP map in the port path (`triumph2/qos.c:5939-5950`). |
| Egress DSCP remark | Bound through EGR_QOS_PROFILE_INDEX. Whether routed packets are rewritten may also depend on the L3 intf's DSCP selection. | Not verified, see test 4.6. |
| TC→queue scope | Programmed per **ingress** port (COS_MAP_SEL), which is how TD2 works. Identical to SAI semantics when every port has the same map, which SONiC does. | — |
| CPU queues | Not touched; that is the CoPP agent's area. CPU queue oids are only in the oid→HW map. | — |
| Warm boot | Cold SDK + `qosBuildHwMap` re-applies the stored bindings. No real warm boot. | Same as the rest of the project. |

---

## 4. Hardware test plan

Run on the box through `deploy/bsh "<cmd>"` (the diag socket). **Never** run `cos port config`, `cos clear`, `cos config`,
`cos strict|simple|weight|drr` or `cos bandwidth` there. `cos port config` creates a gport and flips the chip to ETS mode (CPU LLS reset).
The other commands reprogram what SONiC set.
Use only read-only commands: `cos show`, `cos bandwidth_show`, `cos discard_show`, `g`, `d chg`, `show c`.

0. **Before** loading the new libsai, capture a baseline (pick ports to match, e.g. xe0 / Ethernet0):
   `d chg THDI_PORT_PG_CONFIG_X`, `g THDI_PORT_PRI_GRP0.xe0`, `g THDI_PORT_PG_SPID.xe0`, `g THDI_INPUT_PORT_XON_ENABLES.xe0`,
   `g THDI_BUFFER_CELL_LIMIT_SP`, `g MMU_THDM_DB_POOL_SHARED_LIMIT`, `d chg MMU_THDU_XPIPE_CONFIG_QUEUE`,
   `d chg MMU_WRED_CONFIG_X_PIPE`, `d chg PORT_COS_MAP`, `d chg DSCP_TABLE`, `cos show`, `cos bandwidth_show`.
1. **Boot / no QoS config.** syncd starts. The log shows `qos: hardware map built: N queues ...`: 72×20 + 32 CPU queues,
   72×13 SGs, 72×8 PGs. `show interfaces status` and traffic are unchanged, and the dumps match the baseline.
2. **Apply SONiC qos/buffers.** First check whether the hwsku dir has `qos.json.j2` / `buffers.json.j2` (`ls /usr/share/sonic/device/x86_64-accton_as5812_54t-r0/Accton-AS5812-54T/`).
   If not, load a minimal `config load` JSON with DSCP_TO_TC_MAP, TC_TO_QUEUE_MAP, TC_TO_PRIORITY_GROUP_MAP, SCHEDULER,
   WRED_PROFILE, QUEUE, PORT_QOS_MAP, BUFFER_POOL/PROFILE/PG/QUEUE for two ports. Check:
   - `d chg DSCP_TABLE` (one 64-entry chunk), `g PORT_TAB` TRUST_DSCP_PTR / TRUST_DSCP_V4=1 on those ports, `d chg PORT_COS_MAP`.
   - `g THDI_PORT_PRI_GRP0.xe0` shows the TC→PG map. `d chg THDI_PORT_PG_CONFIG_X` shows PG3/4 with HDRM = xoff/208, MIN = size/208,
     and PG0 dynamic with alpha.
   - `g THDI_BUFFER_CELL_LIMIT_SP` has gone down by the carved headroom/min.
   - `cos discard_show Queue=3` shows WRED on, with thresholds in cells. `cos bandwidth_show` / `cos show` show the weights.
   - syslog has no `qos: ... failed` lines. Check `pending` debug lines for PGs (should resolve after TC_TO_PG).
3. **Classification + queueing (two hosts, A on Ethernet0, B on Ethernet4, 10G).** Send UDP with DSCP 0, 8, 46 and 3 (A→B).
   Use `show queue counters Ethernet4` if the stats work is in, otherwise `show c` (UCQ/… per-cos counters) to see the right queue grow.
   Change one DSCP in DSCP_TO_TC_MAP live and check that the counters move. That exercises hwSetQosMap re-apply.
4. **Scheduling.** Two senders (A plus the CPU-side pseudo traffic, or two flows from A at line rate in different DSCPs)
   oversubscribe B's port:
   - DWRR weights 1:3 between queue 1 and queue 3 should give ~25/75 throughput on the receiver.
   - Then make queue 3 STRICT: queue 1 should starve.
   - Then set a scheduler with `max_bandwidth_rate` = 125000000 (1 Gbit/s) on queue 1: its rate caps at ~1G. `cos bandwidth_show` shows ~1000000 kbps.
   - A port shaper via PORT_QOS_MAP `scheduler` caps the whole port.
5. **WRED/ECN.** With an ECN-capable (ECT) iperf3 flow into queue 3 at oversubscription, the receiver sees CE marks
   (`tcpdump -v 'ip[1]&3=3'`). With ECN off, drops appear instead of marks.
6. **Egress DSCP remark** (only if TC_AND_COLOR_TO_DSCP is used): send DSCP 10 routed A→B, capture on B, compare.
7. **PFC (needs a PFC-capable peer NIC, or the second switch port in MAC loopback cannot do it — use a real NIC).**
   - `pfc_enable 3,4`: `g THDI_INPUT_PORT_XON_ENABLES.xe0` gives PORT_PRI_XON_ENABLE=0x18, PORT_PAUSE_ENABLE=0. `g XLMAC_PFC_CTRL.xe0` (or `CLMAC`) has TX/RX enable set.
   - Oversubscribe queue 3 toward a slow port: the sender port's PFC TX counter rises (`show pfc counters`, or `show c` RPFC/TPFC). Queue 3 shows no drops, while queue 1 drops.
   - Send PFC frames for priority 3 from the NIC (e.g. `ethtool --set-priv-flags`, or a crafted 0x8808 frame): queue 3 on that port stops. `PRIO2COS_PROFILE` = cos bitmap 0x8.
8. **Unbind / restore.** Remove PORT_QOS_MAP and BUFFER_PG for one port. The dumps of that port go back to the baseline from step 0, except PG SPID if a non-zero pool was used.
9. **Port add/remove (DPB)** if A2 supports it: the new port's queues appear in the map log line and inherit switch-level maps.
10. **syncd restart** (`systemctl restart syncd`): QoS dumps after the replay equal those after step 2.

---

## 5. Open risks

- None of this has run on hardware. These SDK behaviours are taken from source reading and need confirming in step 2:
  - the non-ETS `(port, cos)` path for discard, sched, bandwidth and pfc_class_mapping;
  - whether `bcm_cosq_control_get` supports every type that `qos_q_snapshot` asks for (a failing get only disables restore of that field);
  - the MC queue `- 1480` index mirror in `qos_queue_alpha_rw` (taken from `cosq.c:14706`).
- If the SONiC buffer templates for this SKU are missing or wrong for TD2+ (12 MB MMU, 208-byte cells), the PG/queue profiles come straight from
  them. A huge xoff on 72 ports can push the SP shared limit to 0. The SDK API refuses a decrease that would go negative, so
  that shows up as `PGHeadroomLimitBytes failed` errors, not silent loss.
- The PG7-inherit heuristic only triggers on "min 0, static 0, not dynamic" PGs.
- `mmu_lossless` default=1 interplay (§1d). The SDK default makes PG7 lossless, which matters for every priority still mapped to PG7.
