# QoS Second Half Report: Scheduler / WRED / Buffer + SONiC Templates

Branch: `qos-sched` (base `a70b523` / `81de466`).
Build: `deploy/build/libsaivs.so.0.0.0` (warmboot variant, ALL CHECKS PASSED)
SHA256: `b3e52746c7d0eb7eafdf4865b00630121677638160b623d03d34279e495242bf` (150,570,136 bytes, 207 `hwbcm_*` / 32,524 `bcm_*` / 45 `_bcm_esw_*_sync` symbols).

## 1. Audit of Existing `SwitchBCM56860HW_qos.cpp` / `hwbcm_qos.c` (at `a70b523`)

| Area / SAI Object & Attribute | Status at `a70b523` | Gaps / Bugs Found |
|---|---|---|
| `SAI_OBJECT_TYPE_SCHEDULER` (`STRICT`/`DWRR`/`WRR`, `SCHEDULING_WEIGHT`, `MIN/MAX_BANDWIDTH_RATE/BURST`, `METER_TYPE`) | **Programs HW** via `hwbcm_qos_cos_sched_set` (`bcm_cosq_gport_sched_set`, `bcm_cosq_gport_bandwidth_set`, `bcm_cosq_control_set(BandwidthBurstMax/Min)`) & `hwbcm_qos_port_shaper_set` (`bcm_port_rate_egress[_pps]_set`) | 1. Signed int truncation if burst > `INT32_MAX` kbits.<br>2. `hwRemoveScheduler` only erased `m_qosSchedulers` without clearing still-bound SG/queue/port entries or restoring HW defaults.<br>3. L1 SG vs queue binding (and root SG vs port binding) overwrote each other on unbind instead of resolving the remaining effective scheduler. |
| `SAI_OBJECT_TYPE_WRED` (`GREEN/YELLOW/RED_ENABLE`, `MIN/MAX_THRESHOLD`, `DROP_PROBABILITY`, `ECN_MARK_MODE`, `WEIGHT`) via `SAI_QUEUE_ATTR_WRED_PROFILE_ID` | **Programs HW** via `hwbcm_qos_cos_wred_set` (`bcm_cosq_gport_discard_set` with `BCM_COSQ_DISCARD_BYTES\|TCP\|NONTCP\|ENABLE\|MARK_CONGESTION`) | 1. `SAI_WRED_ATTR_ECN_{GREEN,YELLOW,RED}_{MIN,MAX}_THRESHOLD` and `ECN_*_MARK_PROBABILITY` were ignored (only drop attrs read).<br>2. `hwRemoveWred` did not unbind/disable HW WRED on still-bound queues. |
| `SAI_OBJECT_TYPE_BUFFER_POOL` (`TYPE`, `SIZE`, `THRESHOLD_MODE`, `SHARED_SIZE`) | **Partial / Bookkeeping**: maps pools to SP0..SP3 in creation order; `SIZE` recorded only (init-mmu pool sizes preserved); `SHARED_SIZE` was **not** served from HW (`get()` fell through to vslib). | `SAI_SWITCH_ATTR_TOTAL_BUFFER_SIZE` and `SAI_BUFFER_POOL_ATTR_SHARED_SIZE` were not wired to `SwitchBCM56860HW::get()`, so `bufferorch` could not populate `BUFFER_MAX_PARAM_TABLE\|global:mmu_size` or query live HW shared pool bytes. |
| `SAI_OBJECT_TYPE_BUFFER_PROFILE` bound via `SAI_INGRESS_PRIORITY_GROUP_ATTR_BUFFER_PROFILE` & `SAI_QUEUE_ATTR_BUFFER_PROFILE_ID` | **Programs HW** via `hwbcm_qos_pg_buffer_set` & `hwbcm_qos_queue_buffer_set` (`bcm_cosq_control_set` + direct THDI/THDU/THDM alpha writes), **HOWEVER bypassed on bulk set!** | 1. **Critical (`bulkSet` bypass)**: SONiC 202505 `BufferOrch` uses `set_queues_attribute` and `set_ingress_priority_groups_attribute` (`bulkSet`). `SwitchStateBase::bulkSet()` called `set_internal()` directly, bypassing `SwitchBCM56860HW::set()` completely.<br>2. **Critical (TD2+ `DISABLE_QUEUING`)**: Setting `bcmCosqControlEgressUCQueueMinLimitBytes=0` when `Q_SHARED_LIMIT_CELL==0` causes SDK `_bcm_td2_cosq_egr_queue_set()` to set `MMU_THDU_{X,Y}PIPE_Q_TO_QGRP_MAP.DISABLE_QUEUING=1`, silently dropping traffic on dynamic `size=0` UC queues.<br>3. In `mmu_lossless=0`, `USE_QGROUP_MIN=1` prevents per-queue `Q_MIN_LIMIT_CELL` from rebalancing the egress SP unless `bcmCosqControlEgressUCQueueGroupMinEnable=0` is set first.<br>4. Moving a PG or queue with non-zero min/headroom to a different SP did not zero old min/headroom first, skewing old/new SP shared limits.<br>5. `hwRemoveBufferProfile` did not restore HW defaults on still-bound PGs/queues. |

## 2. Changes Implemented

### Hardware Programming & SAI Wiring
- **`SwitchBCM56860HW::bulkSet`** (`SwitchBCM56860HW.h`, `SwitchBCM56860HW.cpp`): Overrides `SwitchStateBase::bulkSet` to dispatch each element through `SwitchBCM56860HW::set()`, fixing hardware programming for SONiC `BufferOrch` bulk `QUEUE` and `INGRESS_PRIORITY_GROUP` profile bindings.
- **Read-only MMU/pool getters** (`SwitchBCM56860HW.cpp`, `SwitchBCM56860HW_qos.cpp`, `hwbcm_qos.c/.h`):
  - `SwitchBCM56860HW::get()` intercepts `SAI_SWITCH_ATTR_TOTAL_BUFFER_SIZE` (`qosGetSwitchAttr` -> `hwbcm_qos_total_buffer_kb_get`, using `soc_td2p_mmu_params_arr_get` / `_MMU_TOTAL_CELLS` -> 16,384 KB on BCM56864) and `SAI_BUFFER_POOL_ATTR_SHARED_SIZE` (`qosGetBufferPoolAttr` -> `hwbcm_qos_pool_shared_get`, reading `THDI_BUFFER_CELL_LIMIT_SP` / `MMU_THDM_DB_POOL_SHARED_LIMIT`).
- **Scheduler & shaper fixes** (`SwitchBCM56860HW_qos.cpp`, `hwbcm_qos.c`):
  - `qosApplyCosScheduler()` resolves the effective L1 `(port, cos)` scheduler across queue bindings (UC > MC) and L1 SG bindings, restoring defaults (`WRR weight 1`, unshaped) only when all bindings clear.
  - `qosApplyPortEffectiveShaper()` resolves effective port shaper across `SAI_PORT_ATTR_QOS_SCHEDULER_PROFILE_ID` and root SG (level 0).
  - `hwRemoveScheduler()` unbinds and restores HW defaults on any remaining SG/queue/port references.
  - `hwbcm_qos_cos_sched_set()` clamps `BandwidthBurstMax`/`Min` arguments to `INT32_MAX`.
- **WRED/ECN fixes** (`SwitchBCM56860HW_qos.cpp`):
  - `qosApplyQueueWred()` supports `SAI_WRED_ATTR_ECN_{GREEN,YELLOW,RED}_{MIN,MAX}_THRESHOLD` and `ECN_*_MARK_PROBABILITY` (used when ECN is enabled without WRED drop on a color, or as fallback).
  - `hwRemoveWred()` unbinds and disables WRED on any remaining queues.
- **Buffer/MMU fixes** (`hwbcm_qos.c`, `SwitchBCM56860HW_qos.cpp`):
  - `qos_uc_queue_disable_queuing_set()` clears `MMU_THDU_{X,Y}PIPE_Q_TO_QGRP_MAP.DISABLE_QUEUINGf` after programming active UC queues (dynamic / non-zero min or static / limit disabled) and on restore.
  - `hwbcm_qos_queue_buffer_set()` clears `bcmCosqControlEgressUCQueueGroupMinEnable` (`USE_QGROUP_MINf`) when a UC queue profile sets `min_bytes > 0`, and snapshots/restores the boot value in `qos_q_snapshot()` / `qos_q_restore()`.
  - `hwbcm_qos_pg_buffer_set()`, `hwbcm_qos_queue_buffer_set()`, and `qos_q_restore()` zero old min (and PG headroom) before changing `bcmCosqControlIngressPool` / `bcmCosqControl{UC,MC}EgressPool` so service pool shared limits stay accurate.
  - `hwRemoveBufferProfile()` unbinds and restores boot snapshots on any remaining PGs/queues.

## 3. Warm-Boot Handling

- Zero hardware writes on warm boot preserved:
  - `qosBuildHwMap(/*warmBoot=*/true)` rebuilds `m_qosSgSched`, `m_qosQueueSched`, `m_qosQueueWred`, `m_qosQueueProfile`, `m_qosPgProfile`, and `m_qosPort` desired state solely from `m_objectHash`.
  - `qosGetSwitchAttr` / `qosGetBufferPoolAttr` are read-only queries.
  - Added optional TLV `HWSTATE_TAG_QOS_QGRP_MIN = 24` in `SwitchBCM56860HW_wb.inc.cpp` backed by `hwbcm_qos_wb_get_qgrp_snaps()` / `hwbcm_qos_warm_restore_qgrp()` to persist/restore UC boot-time `USE_QGROUP_MINf` snapshots alongside `HWSTATE_TAG_QOS_SNAPS` (backward-compatible with older `.hwstate` files).

## 4. SONiC HWSKU Templates & Install Wiring

Added to `deploy/ref/hwsku-qos/` (for `Accton-AS5812-54T`, lossy-only, non-ETS 8 cos, dynamic port discovery from `PORT` with fallback `Ethernet0..71` minus `49-51, 61-63`):
- `qos.json.j2`: `DSCP_TO_TC_MAP|AZURE` (DSCP 46 -> TC 5, DSCP 10 -> TC 1, DSCP 3 -> TC 3, DSCP 4 -> TC 4, DSCP 8 -> TC 0, DSCP 48 -> TC 6), `TC_TO_QUEUE_MAP|AZURE` (0..7), `TC_TO_PRIORITY_GROUP_MAP|AZURE` (all -> PG 0), `SCHEDULER` (`scheduler.0` DWRR w=14, `scheduler.1` DWRR w=15), `WRED_PROFILE|AZURE_LOSSY` (ECN all, 1 MB..2 MB, 5%), `PORT_QOS_MAP`, `QUEUE` (0..7 per port).
- `buffers.json.j2`, `buffers_defaults_t0.j2` (`5m`), `buffers_defaults_t1.j2` (`40m`): `CABLE_LENGTH`, `BUFFER_POOL` (`ingress_lossy_pool`, `egress_lossy_pool`, 12,766,208 B dynamic), `BUFFER_PROFILE` (`ingress_lossy_profile`, `egress_lossy_profile`, `q_lossy_profile`, `size=0`, `dynamic_th=3`), `BUFFER_PG` (`|0-7`), `BUFFER_QUEUE` (`|0-7`).
- `pg_profile_lookup.ini`: 1G/10G/40G x 5m/40m/300m table (208 B cell multiples) for `buffermgrd`.
- Validated Jinja2 rendering to valid JSON across empty, T0 (`ToRRouter`), and T1 (`LeafRouter`) contexts.
- Updated `deploy/install.sh` and `deploy/remote_install.sh` (`--qos-dir`) to copy templates into `$HWSKU` only when absent or different (`cmp -s`).

## 5. Verification Steps (`deploy/test_qos_sched.sh`)

Offline plan: `deploy/test_qos_sched.sh plan [Ethernet0] [xe0] [1]`.
On switch (via lead):
1. Deploy updated `libsaivs.so` + templates (`deploy/install.sh`), then run `sudo config qos reload` and check `redis-cli -n 6 HGETALL "BUFFER_MAX_PARAM_TABLE|global"` (`mmu_size` ~16777216).
2. Apply test overlay: `SWITCH_PASS=... deploy/test_qos_sched.sh apply Ethernet0` (UC1 = DWRR weight 1 + 100 Mbps shaper, UC3 = WRED 104 KB..416 KB 10% + ECN all, UC5 = DWRR weight 4).
3. Verify HW state (read-only diag commands only — never run `cos port config`):
   `SWITCH_PASS=... deploy/test_qos_sched.sh verify Ethernet0 xe0 1`
   - `cos port show PortBitMap=xe0` (DWRR weights: COSQ 1 = 1, COSQ 5 = 4)
   - `cos bandwidth_show` (xe0 q=1 `KbpsMax=100000`, q=5 `KbpsMax=0`)
   - `cos discard_show Gport=1 Queue=3` + `d chg MMU_WRED_CONFIG_{X,Y}_PIPE` (Min 104000, Max 416000, DropProb 10, Ena 1, `ECN_MARKING_EN=1`)
   - `d chg THDI_PORT_PG_CONFIG_X`, `d chg MMU_THDU_XPIPE_CONFIG_QUEUE`, `d chg MMU_THDU_XPIPE_Q_TO_QGRP_MAP` (`DISABLE_QUEUING=0`)
4. Data-plane traffic + counters: `show queue counters Ethernet0` and `deploy/bsh "show c xe0"` with iperf3 TOS `0x28` (DSCP 10 -> UC1 100 Mbps cap), TOS `0xb8` (DSCP 46 -> UC5 4:1 DWRR share), and TOS `0x0e` (DSCP 3 + ECT(0) -> UC3 ECN/WRED).
5. Revert and verify default restoration: `SWITCH_PASS=... deploy/test_qos_sched.sh revert Ethernet0 && deploy/test_qos_sched.sh verify Ethernet0 xe0 1`.
