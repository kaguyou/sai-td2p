# Domain 4 Audit Report (Part 2 of 2): Gap Analysis & Robustness Bugs

---

## 2. Detailed Gap Analysis for Every `(b)` Silent No-Op & `(c)` Error / Unhandled Item

### 2.1 QoS Maps & Port/Switch QoS Attributes

1. **`SAI_QOS_MAP_TYPE_TC_AND_COLOR_TO_DOT1P` / `SAI_PORT_ATTR_QOS_TC_AND_COLOR_TO_DOT1P_MAP`** (`(b)` silent no-op at `SwitchBCM56860HW_qos.cpp:984-991, 1749-1752`)
   - **Vendor TD2+ SDK API(s)**: `bcm_qos_map_create(unit, BCM_QOS_MAP_L2 | BCM_QOS_MAP_EGRESS, &map_id)` + `bcm_qos_map_add(unit, BCM_QOS_MAP_L2 | BCM_QOS_MAP_EGRESS, &map, map_id)` (`int_pri`, `color` -> `pkt_pri`, `pkt_cfi`) + `bcm_qos_port_map_set(unit, port, -1, map_id)` (verified in `include/bcm/qos.h:19,30,218,252` and `src/bcm/esw/triumph2/qos.c:3737-3760, 7397-7435`). Note: TD2+ uses `soc_feature_egr_qos_combo_profile`, so egress DSCP + dot1p maps on the same port share `EGR_PORT.EGR_QOS_PROFILE_INDEX` or must be handled via separate L2/L3 egress `bcm_qos_port_map_set` calls (verify interaction in `triumph2/qos.c:5939-5955`).
   - **SONiC consequence**: `TC_TO_DOT1P_MAP` configured in `CONFIG_DB` is silently ignored; egress 802.1Q PCP bits are never remarked.
   - **Effort**: S (mirrors existing `TC_AND_COLOR_TO_DSCP` cache + port bind).
   - **Testable in 1-switch + 2x 1G copper PC lab?**: **Yes** — send tagged VLAN traffic on 1G port 1, capture on 1G port 2 (or KNET) with `tcpdump` and inspect 802.1p PCP bits.

2. **`SAI_QOS_MAP_TYPE_MPLS_EXP_TO_TC`, `MPLS_EXP_TO_COLOR`, `TC_AND_COLOR_TO_MPLS_EXP` / `SAI_PORT_ATTR_QOS_MPLS_EXP_TO_TC_MAP`** (`(b)` silent no-op at `SwitchBCM56860HW_qos.cpp:1749-1752`, port bind unhandled)
   - **Vendor TD2+ SDK API(s)**: `bcm_qos_map_create(unit, BCM_QOS_MAP_MPLS | BCM_QOS_MAP_INGRESS, &id)` / `BCM_QOS_MAP_MPLS | BCM_QOS_MAP_EGRESS` + `bcm_qos_map_add` (`exp` <-> `int_pri`/`color`), bound on MPLS tunnel/port (`triumph2/qos.c:4477, 7399, 7578`).
   - **SONiC consequence**: MPLS EXP classification/remarking silently ignored (only relevant if MPLS is used; MPLS itself is out of Domain 4).
   - **Effort**: S (for map create/bind), M if MPLS label/tunnel binding is also needed.
   - **Testable in 1-switch + 2x 1G PC lab?**: **Yes** (if MPLS data plane works; scapy MPLS packets on 1G copper links), **No** if MPLS forwarding is not implemented in SAI.

3. **`SAI_QOS_MAP_TYPE_DSCP_TO_FORWARDING_CLASS`, `MPLS_EXP_TO_FORWARDING_CLASS`** & port bindings (`qosorch.cpp:70-71, 1103, 1197`) (`(b)` silent no-op)
   - **Vendor TD2+ SDK API(s)**: **Not supported on TD2+ HW** (forwarding-class QoS maps are VOQ/DNX/TH4+ concepts; no TD2+ `bcm_qos` equivalent). Should return `SAI_STATUS_NOT_SUPPORTED` instead of silent success.
   - **SONiC consequence**: Silent acceptance of unsupported config if user configures `DSCP_TO_FC_MAP` / `EXP_TO_FC_MAP`.
   - **Effort**: S (reject with `SAI_STATUS_NOT_SUPPORTED`).
   - **Testable in 1-switch + 2x 1G PC lab?**: **Yes** (control-plane SAI status check).

4. **`SAI_QOS_MAP_TYPE_PFC_PRIORITY_TO_PRIORITY_GROUP` (non-identity entries)** (`(b)` silent no-op at `SwitchBCM56860HW_qos.cpp:1080-1098`)
   - **Vendor TD2+ SDK API(s)**: **HW limitation on TD2+** — TD2+ `THDI_INPUT_PORT_XON_ENABLES` maps PFC priority `i` directly to PG `i` (identity only; verified `src/bcm/esw/trident2/cosq.c:18866-18880`).
   - **SONiC consequence**: Non-identity `PFC_PRIORITY_TO_PG` maps silently behave as identity (WARN logged). Standard SONiC configs use identity (`3->3, 4->4`).
   - **Effort**: S (either keep WARN or reject non-identity with `SAI_STATUS_NOT_SUPPORTED`).
   - **Testable in 1-switch + 2x 1G PC lab?**: **Yes** (control-plane check only).

5. **Non-ETS Mode Limitations: Queues 8 & 9 (UC/MC) and L0 Scheduler Groups (`level == 1`)** (`(b)` silent no-op at `SwitchBCM56860HW_qos.cpp:2011-2016, 2104-2110, 2242-2250, 2776-2784` and `hwbcm_stats.c:147-149, 317-323`)
   - **Vendor TD2+ SDK API(s)**: Switching to ETS mode via `bcm_cosq_gport_add(unit, port, numq, BCM_COSQ_GPORT_UCAST_QUEUE_GROUP / MCAST_QUEUE_GROUP / SCHEDULER, &gport)` + `bcm_cosq_gport_attach` (and rebuilding the CPU port LLS tree) exposes all 10 UC + 10 MC queues and L0 scheduler nodes (`include/bcm/cosq.h`, `src/bcm/esw/trident2/cosq.c:4341`).
   - **SONiC consequence**: `SwitchBCM56850` advertises 10 UC + 10 MC queues and 13 SGs (`2 L0 + 10 L1`), so SONiC can bind schedulers/WRED/buffers to queues 8–9 or L0 SGs, which silently do nothing and report 0 stats. Standard SONiC uses queues 0–7 (TC 0–7), so normal SONiC QoS works.
   - **Effort**: L (full ETS migration requires rebuilding per-port + CPU LLS trees and rewriting `hwbcm_qos.c` + `hwbcm_stats.c` to use queue/scheduler gports). *Alternative S*: advertise 8 UC + 8 MC queues in `SwitchBCM56860HW` to match non-ETS HW.
   - **Testable in 1-switch + 2x 1G PC lab?**: **Yes** — generate DSCP/TC traffic mapped to queues 0..9 and verify shaping/WRR/stats.

---

### 2.2 Buffers, Headroom & Port-SP Limits

6. **`SAI_BUFFER_POOL_ATTR_SIZE` (create & set)** (`(b)` silent no-op at `SwitchBCM56860HW_qos.cpp:2368-2371, 2422, 2446-2451`)
   - **Vendor TD2+ SDK API(s)**:
     - Ingress SP limit: `bcm_cosq_control_set(unit, -1, sp, bcmCosqControlIngressPoolLimitBytes, bytes)` (verified in `include/bcm/cosq.h:841`, `src/bcm/esw/trident2/cosq.c:14385-14402, 16871` — writes `THDI_BUFFER_CELL_LIMIT_SPr`).
     - Egress SP limit: `bcm_cosq_control_set(unit, -1, sp, bcmCosqControlEgressPoolSharedLimitBytes, bytes)` (verified in `include/bcm/cosq.h:852`, `src/bcm/esw/trident2/cosq.c:12231-12289, 16847` — writes `MMU_THDM_DB_POOL_SHARED_LIMITr` & THDU SP shared limit).
   - **SONiC consequence**: `BUFFER_POOL` sizes configured in `buffers.json` / `CONFIG_DB` (e.g., splitting 16MB MMU into `ingress_lossless_pool`, `egress_lossy_pool`, `egress_lossless_pool`) are NEVER programmed to ASIC! SP0 keeps its entire init-MMU size while SP1/SP2/SP3 keep whatever init-MMU gave them (often 0 cells!), so any PG/queue moved to SP1–SP3 can silently drop all packets or have wrong shared limits!
   - **Effort**: M (must handle cell rounding, total MMU budget vs reserved min/headroom already carved out by SDK, and resume limits `bcmCosqControlEgressPoolResumeLimitBytes`).
   - **Testable in 1-switch + 2x 1G PC lab?**: **Yes** — create a pool with a small size (e.g., 64 KB), bind a PG/queue to it, verify `SAI_BUFFER_POOL_ATTR_SHARED_SIZE` readback and burst drop behavior with 1G line-rate microbursts / port shaper bottleneck.

7. **`SAI_PORT_ATTR_QOS_INGRESS_BUFFER_PROFILE_LIST` & `SAI_PORT_ATTR_QOS_EGRESS_BUFFER_PROFILE_LIST`** (`(b)` silent no-op at `SwitchBCM56860HW_qos.cpp:974-982`)
   - **Vendor TD2+ SDK API(s)**:
     - Ingress port-SP: `bcm_cosq_control_set(unit, port, sp, bcmCosqControlIngressPortPoolMaxLimitBytes, bytes)` and `bcmCosqControlIngressPortPoolMinLimitBytes` (`include/bcm/cosq.h:783-784`, verified TD2+ `src/bcm/esw/trident2/cosq.c:13676-13715, 16864-16866`).
     - Egress port-SP: `bcm_cosq_control_set(unit, port, sp, bcmCosqControlEgressPoolLimitBytes / YellowLimitBytes / RedLimitBytes / LimitEnable, val)` (`src/bcm/esw/trident2/cosq.c:12455-12465, 16841-16844, 16885-16886`).
   - **SONiC consequence**: Per-port ingress/egress buffer pool profiles (`BUFFER_PORT_INGRESS_PROFILE_LIST`, `BUFFER_PORT_EGRESS_PROFILE_LIST` in `bufferorch.cpp:1626,1816`) are silently ignored (WARN logged).
   - **Effort**: M.
   - **Testable in 1-switch + 2x 1G PC lab?**: **Yes** — bind small static port-SP limit + egress shaper on 1G port and verify drops at configured threshold.

8. **`SAI_PORT_ATTR_QOS_MAXIMUM_HEADROOM_SIZE` (get)** (`(c)` error `SAI_STATUS_FAILURE` at `portsorch.cpp:6191-6197`)
   - **Vendor TD2+ SDK API(s)**: Compute from TD2+ per-PG `PG_HDRM_LIMITf` field width (`soc_mem_field_length(unit, THDI_PORT_PG_CONFIG_Xm, PG_HDRM_LIMITf)` × 208 bytes, or `bcmCosqControlIngressPortPGHeadroomLimitBytes` max / MMU total cells), exposed in `SwitchBCM56860HW::get()` for `SAI_OBJECT_TYPE_PORT`.
   - **SONiC consequence**: `portsorch` logs `"Unable to get the maximum headroom for port ... rv:-1, ignored"` for every port at boot and omits `max_headroom_size` from `STATE_DB|BUFFER_MAX_PARAM_TABLE`, which breaks SONiC dynamic buffer model (`buffermgrd`) headroom validation!
   - **Effort**: S.
   - **Testable in 1-switch + 2x 1G PC lab?**: **Yes** — check `redis-cli -n 6 HGETALL "BUFFER_MAX_PARAM_TABLE|Ethernet0"`.

9. **`SAI_BUFFER_POOL_ATTR_XOFF_SIZE` (Shared Headroom Pool)** (`(b)` silent no-op at `SwitchBCM56860HW_qos.cpp:2458-2464`)
   - **Vendor TD2+ SDK API(s)**: **Not supported in TD2+ HW** (`bcmCosqControlIngressHeadroomPoolLimitBytes` is NOT implemented in `src/bcm/esw/trident2/cosq.c`; TD2+ only has per-PG headroom + global headroom pool, not per-SP shared headroom pool — SHP starts on Tomahawk/TD3).
   - **SONiC consequence**: Dynamic buffer calculation with shared headroom pool (`xoff` on `BUFFER_POOL`) is ignored; lossless PFC relies on per-PG `XOFF_TH` (which IS implemented).
   - **Effort**: S (if keeping WARN no-op, or reject non-zero with `SAI_STATUS_NOT_SUPPORTED` so `buffermgrd` doesn't assume SHP exists).
   - **Testable in 1-switch + 2x 1G PC lab?**: **Yes** (control-plane check only).

10. **`SAI_BUFFER_PROFILE_ATTR_PACKET_ADMISSION_FAIL_ACTION` (`DROP_AND_TRIM`)** (`(b)` silent no-op + false capability advertisement at `SwitchStateBase.cpp:4100-4101`)
    - **Vendor TD2+ SDK API(s)**: **Not supported on TD2+ HW** (packet trimming is TH5/modern ASIC only). Should override `queryAttrEnumValuesCapability` to advertise only `DROP` (or `NOT_SUPPORTED`).
    - **SONiC consequence**: SONiC thinks TD2+ supports packet trimming and silently accepts `trim` profiles without trimming packets.
    - **Effort**: S.
    - **Testable in 1-switch + 2x 1G PC lab?**: **Yes** (control-plane capability check).

---

### 2.3 PFC Watchdog (PFCWD) & Queue Pause Status (CRITICAL CLUSTER)

11. **`SAI_QUEUE_ATTR_PAUSE_STATUS` (get)** (`(b)` silent no-op — hardcoded `false` in `SwitchStateBase.cpp:2386-2402`)
    - **Vendor TD2+ SDK API(s)**: Verified in `src/bcm/esw/trident2/cosq.c:22677-22797` (`bcm_td2_cosq_pause_queue_status_get`): reads `READ_XPORT_TO_MMU_BKPr(unit, local_port, &rval_bkp)` (`PRI15_0_BKPf`) and maps active RX PFC priorities through `PORT_PFC_CFG0..3r` / `PRIO2COS_PROFILEr` (or `bcm_cosq_pfc_class_mapping_get`) to check whether `cos` is currently paused! (Note: `bcm_td2_cosq_pause_queue_status_get` requires a queue gport, so in non-ETS mode read `XPORT_TO_MMU_BKPr` + `bcm_cosq_pfc_class_mapping_get` directly in `hwbcm_qos.c`).
    - **SONiC consequence**: `pfc_detect_broadcom.lua:50,74` requires `SAI_QUEUE_ATTR_PAUSE_STATUS == 'true'` for consecutive polls. Hardcoded `false` makes **PFC deadlock detection completely non-functional**!
    - **Effort**: S.
    - **Testable in 1-switch + 2x 1G PC lab?**: **Yes** — enable PFC RX on a 1G port, send synthetic 802.1Qbb PFC pause frames (EtherType `0x8808`, opcode `0x0101`, non-zero quanta for priority 3) from PC using `scapy`, and poll `SAI_QUEUE_ATTR_PAUSE_STATUS` in `COUNTERS_DB`.

12. **`SAI_PORT_STAT_PFC_0..7_ON2OFF_RX_PKTS`** (`(b)` missing from `hwPstatOf` & `queryStatsCapability`)
    - **Vendor TD2+ SDK API(s)**: `bcm_stat_get(unit, port, snmpBcmRxPFCFrameXonPriority0..7, &val)` (verified in `include/bcm/stat.h:240-254`, `src/bcm/esw/stat_generic.c:811-841`, AND register `RPFCOFF0r` verified non-NULL in `src/soc/mcm/bcm56860_a0.c:85845`!).
    - **SONiC consequence**: `pfc_detect_broadcom.lua:43,49,52` checks `if ... and pfc_on2off and ...`. Because it is omitted from `queryStatsCapability`, FlexCounter never populates `SAI_PORT_STAT_PFC_*_ON2OFF_RX_PKTS`, so the Lua detection script **never runs**!
    - **Effort**: S (8 entries in `hwPstatOf` + `hwbcm_core.c:pstat_map`).
    - **Testable in 1-switch + 2x 1G PC lab?**: **Yes** — send PFC XOFF then PFC XON (quanta=0) frames via `scapy` on 1G link and observe `SAI_PORT_STAT_PFC_*_ON2OFF_RX_PKTS` increment.

13. **`SAI_QUEUE_ATTR_PFC_DLR_INIT`, `SAI_QUEUE_ATTR_ENABLE_PFC_DLDR`, `SAI_SWITCH_ATTR_PFC_DLR_PACKET_ACTION`** (`(b)` silent no-ops at `SwitchBCM56860HW_qos.cpp:2857-2861` and unhandled in `hwSetSwitch`, compounded by `SwitchStateBase::queryAttributeCapability:4165`!)
    - **Vendor TD2+ SDK API(s)**: **TD2+ HW has NO hardware PFC deadlock engine** — verified in `src/bcm/esw/pfc_deadlock.c:2250-2278` (only TH/HR4/FB6/HX5/TD3) and `src/soc/common/feature.c:6487-6819` (`soc_feature_pfc_deadlock` is NOT enabled on `bcm56860_a0`). On vendor Broadcom TD2+ SAI, `queryAttributeCapability(SAI_OBJECT_TYPE_QUEUE, SAI_QUEUE_ATTR_PFC_DLR_INIT)` returns `false`!
    - **SONiC consequence**: Because `SwitchStateBase::queryAttributeCapability` blindly returns `true` for every attribute, `switchorch.cpp:103` -> `orchdaemon.cpp:730-757` selects `PfcWdDlrHandler` (which relies on unimplemented `SAI_QUEUE_ATTR_PFC_DLR_INIT` + `SAI_SWITCH_ATTR_PFC_DLR_PACKET_ACTION`) **instead of** the software fallback `PfcWdAclHandler` / `PfcWdLossyHandler` (which works on TD2+ using ACL drop + disabling PFC on that priority!). Returning `false` for `PFC_DLR_INIT` capability immediately activates the working TD2+ SW recovery path (`PfcWdAclHandler`/`PfcWdLossyHandler`)!
    - **Effort**: S (override `queryAttributeCapability` in `SwitchBCM56860HW` to return `false` for `SAI_QUEUE_ATTR_PFC_DLR_INIT` / `SAI_QUEUE_ATTR_ENABLE_PFC_DLDR` / `SAI_SWITCH_ATTR_PFC_DLR_PACKET_ACTION`).
    - **Testable in 1-switch + 2x 1G PC lab?**: **Yes** — enable `pfcwd`, send continuous PFC XOFF storm from PC with `scapy`, verify `pfcwd` detects storm (`PFC_WD_STATUS=stormed`) and triggers `PfcWdAclHandler` (drop) / `PfcWdLossyHandler` (forward) and restores after restoration timer!

14. **`SAI_INGRESS_PRIORITY_GROUP_STAT_PACKETS` & `DROPPED_PACKETS`** (`(c)` `SAI_STATUS_NOT_SUPPORTED` at `SwitchBCM56860HW_stats.cpp:138`, called directly by `pfcactionhandler.cpp:609-610,640` and FlexCounter `PG_DROP` at `portsorch.cpp:326`)
    - **Vendor TD2+ SDK API(s)**: **Not supported in TD2+ MMU HW** — `src/bcm/esw/trident2/cosq.c` has no per-PG received/dropped packet counter (only per-source-port MMU ingress drop `bcmCosqStatSourcePortDroppedPackets`, not per-PG).
    - **SONiC consequence**: `PG_DROP` flex counter group is cleanly filtered out by `queryStatsCapability`, **but** `PfcWdLossyHandler::getHwCounters()` (`pfcactionhandler.cpp:640-650`) calls `get_ingress_priority_group_stats` unconditionally and aborts updating PFCWD TX/RX drop counters in `COUNTERS_DB` whenever it returns an error! (Note: `pfcactionhandler.cpp:628` comment notes even Mellanox lacked PG counters; vendor Broadcom TD2+ SAI either returns 0 or `NOT_SUPPORTED`.)
    - **Effort**: S (if returning 0 for `PG_STAT_PACKETS/DROPPED_PACKETS` only on direct non-capa calls, or accept `NOT_SUPPORTED` as HW limitation).
    - **Testable in 1-switch + 2x 1G PC lab?**: **Yes** (trigger PFCWD storm and check `show pfcwd stats`).

---

### 2.4 Counters, Watermarks & Debug Counters

15. **`SAI_OBJECT_TYPE_ROUTER_INTERFACE` stats (`SAI_ROUTER_INTERFACE_STAT_IN/OUT_PACKETS/OCTETS`, `*_ERROR_*`)** (`(b)` silent no-op — always 0)
    - **Vendor TD2+ SDK API(s)**:
      - Ingress RIF: `bcm_stat_group_create(unit, bcmStatObjectIngL3Intf, bcmStatGroupModeSingle, &stat_id, &num)` + `bcm_l3_ingress_stat_attach(unit, ing_intf_id, stat_id)` + `bcm_l3_ingress_stat_counter_get/set` (`include/bcm/stat.h:1741,2175`, `include/bcm/l3.h:2548-2589`).
      - Egress RIF: `bcm_stat_group_create(unit, bcmStatObjectEgrL3Intf, bcmStatGroupModeSingle, &stat_id, &num)` + `bcm_l3_intf_stat_attach(unit, &l3_intf, stat_id)` + `bcm_stat_flex_counter_get/set` (`include/bcm/stat.h:1751`, `include/bcm/l3.h:1359-1375`).
      - Plus advertise in `SwitchBCM56860HW::queryStatsCapability(SAI_OBJECT_TYPE_ROUTER_INTERFACE)`.
    - **SONiC consequence**: `show interfaces counters rif` always displays 0 packets / 0 bytes on all L3 interfaces and SVIs!
    - **Effort**: M (manage flex counter pool lifecycle on RIF create/remove + warm boot persistence of `stat_id`).
    - **Testable in 1-switch + 2x 1G PC lab?**: **Yes** — route packets between the two 1G copper ports (or PC -> SVI/RIF -> PC) and verify `show interfaces counters rif` increments accurately.

16. **`SAI_OBJECT_TYPE_TUNNEL` stats (`SAI_TUNNEL_STAT_IN/OUT_OCTETS/PACKETS`)** (`(b)` silent no-op — always 0)
    - **Vendor TD2+ SDK API(s)**: `bcm_stat_group_create(unit, bcmStatObjectIngVxlan / bcmStatObjectEgrVxlan, ...)` + `bcm_vxlan_stat_attach(unit, vxlan_port_id, stat_id)` + `bcm_vxlan_stat_counter_get/set` (`include/bcm/stat.h:1764,1768`, `include/bcm/vxlan.h:433-478`).
    - **SONiC consequence**: `TUNNEL` flex counters (`vxlanorch.cpp:71-74`) always show 0.
    - **Effort**: M (aggregate across VXLAN VPs belonging to tunnel + warm boot).
    - **Testable in 1-switch + 2x 1G PC lab?**: **Yes** — send VXLAN-encapsulated frames from PC on 1G port 1 into VXLAN decap/encap and capture on 1G port 2.

17. **`SAI_OBJECT_TYPE_POLICER` stats (`PACKETS`, `BYTES`, `GREEN/YELLOW/RED_PACKETS/BYTES`)** (`(b)` silent no-op — always 0)
    - **Vendor TD2+ SDK API(s)**: On TD2+, CoPP/ACL policers are FP meters (`bcm_policer_create` attached via `bcm_field_entry_policer_attach`). Per-color counts come from FP flex stats attached to the entry (`bcm_field_stat_get(unit, stat_id, bcmFieldStatGreenPackets/YellowPackets/RedPackets/Bytes, &val)`, verified in `include/bcm/field.h:17220` — `hwbcm_copp_trap_stats_get` ALREADY reads `pkts, bytes, red`!). Standalone `bcm_policer_stat_get` (`include/bcm/policer.h:759`) is not supported for IFP meters on TD2+.
    - **SONiC consequence**: `POLICER` flex counters / P4 ACL meter stats always read 0.
    - **Effort**: M (aggregate `bcm_field_stat_get` across all FP entries bound to the policer, or return `NOT_SUPPORTED` in `queryStatsCapability`).
    - **Testable in 1-switch + 2x 1G PC lab?**: **Yes** — flood ARP/ICMP to CPU or ACL policed traffic on 1G link and check policer stats.

18. **`SAI_OBJECT_TYPE_COUNTER` for ROUTE (`SAI_ROUTE_ENTRY_ATTR_COUNTER_ID`) & SRv6** (`(b)` silent no-op — always 0; only CoPP TRAP counters work)
    - **Vendor TD2+ SDK API(s)**: `bcm_stat_group_create(unit, bcmStatObjectIngL3Route, ...)` + `bcm_l3_route_stat_attach(unit, &route, stat_id)` + `bcm_l3_route_stat_counter_get/set` (`include/bcm/l3.h:2710-2749`; note TD2+ ALPM requires `soc_feature_alpm_flex_stat` / `alpm_flex_stat_support=1` in `config.bcm`, verified in `feature.c:6800+`).
    - **SONiC consequence**: `FLOW_CNT_ROUTE` flex counters (`flexcounterorch.cpp:265-277`) always read 0. (SRv6 is not supported on TD2+ HW.)
    - **Effort**: M (attach flex counter on route create/set + warm boot; verify `alpm_flex_stat_support` in `config.bcm`).
    - **Testable in 1-switch + 2x 1G PC lab?**: **Yes** — configure a static route with flow counter and send routed traffic between the two 1G ports.

19. **`SAI_OBJECT_TYPE_DEBUG_COUNTER` & `SAI_PORT_STAT_IN/OUT_DROP_REASON_RANGE_BASE`, `SAI_SWITCH_STAT_IN/OUT_DROP_REASON_RANGE_BASE`** (`(c)` capability query fails -> 0 drop reasons advertised; `(b)` silent 0 if polled)
    - **Vendor TD2+ SDK API(s)**:
      - TD2+ has 9 ingress custom debug counters (`RDBGC0..8` -> `snmpBcmCustomReceive0..8`) and 12 egress (`TDBGC0..11` -> `snmpBcmCustomTransmit0..11`), verified in `src/soc/mcm/bcm56860_a0.c:85362,94963` and `src/bcm/esw/stat.c:2347-2525, 3379-3381`. Note SDK reserves `RDBGC0..2,6` and `TDBGC0..8` at init (`src/bcm/esw/stat.c:1014-1163`) unless reassigned or using free slots (`RDBGC3,4,5,7,8` and `TDBGC9,10,11`).
      - APIs: `bcm_stat_custom_add(unit, port, snmpBcmCustomReceiveN, trigger)` / `bcm_stat_custom_delete_all` (`include/bcm/stat.h:1467-1481`) + `bcm_stat_get(unit, port, snmpBcmCustomReceiveN, &val)` (per-port; sum across ports for `SWITCH` debug counters).
      - Must also override `queryAttrEnumValuesCapability(SAI_OBJECT_TYPE_DEBUG_COUNTER, SAI_DEBUG_COUNTER_ATTR_TYPE / IN_DROP_REASON_LIST / OUT_DROP_REASON_LIST)` and `objectTypeGetAvailability(SAI_OBJECT_TYPE_DEBUG_COUNTER)` (`drop_counter.cpp:305,376,439`).
    - **SONiC consequence**: `show dropcounters capabilities` is empty (`[]`), `config dropcounters add` fails with unsupported drop reason, and `show dropcounters counts` cannot monitor L2/L3/ACL/VLAN/TTL/MTU drop reasons!
    - **Effort**: L (trigger mapping table for ~35 `SAI_IN/OUT_DROP_REASON_*` -> `bcm_custom_stat_trigger_t`, RDBGC/TDBGC allocator avoiding SDK-reserved slots, `queryAttrEnumValuesCapability`, `objectTypeGetAvailability`, port & switch stat polling, warm boot).
    - **Testable in 1-switch + 2x 1G PC lab?**: **Yes** — send packets with TTL=0, bad IP checksum, unknown VLAN tag, or SMAC=DMAC from PC on 1G link and verify `show dropcounters counts` increments!

20. **`SAI_PORT_STAT_IP_IN_UCAST_PKTS`, `SAI_PORT_STAT_IF_IN_VLAN_DISCARDS`, `SAI_PORT_STAT_IF_IN_FEC_*`, `SAI_PORT_STAT_ECN_MARKED_PACKETS`, `SAI_PORT_STAT_*_WRED_DROPPED_BYTES`, `SAI_QUEUE_STAT_WRED_*`, `SAI_PORT_STAT_PFC_*_PAUSE_DURATION_US`, `SAI_BUFFER_POOL_STAT_XOFF_ROOM_WATERMARK_BYTES` / `CURR_OCCUPANCY_BYTES`**
    - **Vendor TD2+ SDK API(s)**:
      - `SAI_PORT_STAT_IP_IN_UCAST_PKTS` (`portsorch.cpp:243`): no direct SNMP enum in `stat.h`; TD2+ uses `snmpIpInReceives` (already mapped to `SAI_PORT_STAT_IP_IN_RECEIVES`).
      - `SAI_PORT_STAT_ECN_MARKED_PACKETS` & `SAI_QUEUE_STAT_WRED_ECN_MARKED_*` & `SAI_QUEUE_STAT_WRED_DROPPED_*` & `SAI_PORT_STAT_*_WRED_DROPPED_BYTES`: **Not supported in TD2+ HW** (`EGR_ECN_COUNTER` is NULL in `bcm56860_a0.c:28487`, `bcmCosqStat*DiscardDroppedBytes` / per-queue WRED / ECN stats are absent from `src/bcm/esw/trident2/cosq.c`).
      - `SAI_PORT_STAT_PFC_*_PAUSE_DURATION_US`: **Not supported in TD2+ HW** (`PFC_XOFF_TIMER` is NULL in `bcm56860_a0.c:82341`).
      - `SAI_BUFFER_POOL_STAT_XOFF_ROOM_*`: **Not supported in TD2+ HW** (no shared headroom pool).
      - `SAI_BUFFER_POOL_STAT_CURR_OCCUPANCY_BYTES`: **No single pool-wide current counter in TD2+ `trident2/cosq.c`** (only per-port pool current `bcmCosqStatIngressPortPoolSharedBytesCurrent` / `bcmCosqStatEgressPortPoolSharedBytesCurrent` which would require summing all ports).
      - `SAI_PORT_STAT_IF_IN_FEC_*`: 1G/10G copper ports on AS5812-54T have no RS-FEC; 40G QSFP ports only have CL74 FC-FEC (`bcm_port_control_get(bcmPortControlFecErrorDetect)`), not RS-FEC symbol/codeword S0..S15 counters.
    - **SONiC consequence**: Cleanly filtered out by `queryStatsCapability` (except `XOFF_ROOM_WATERMARK_BYTES` in `bufferorch.cpp:316` — see Bug #R1!).
    - **Effort**: N/A (ASIC HW limitations, already accurately omitted from `queryStatsCapability`).
    - **Testable in 1-switch + 2x 1G PC lab?**: **No** for FEC (no 40G) / N/A for unsupported ASIC features.

21. **Bulk Stats (`bulkGetStats` / `bulkClearStats`)** (`(c)` `SAI_STATUS_NOT_IMPLEMENTED` in `Sai.cpp:589,606` & `VirtualSwitchSaiInterface.cpp:1173,1188`)
    - **Vendor TD2+ SDK API(s)**: Implement loop over `getStatsExt` inside `SwitchBCM56860HW` / `VirtualSwitchSaiInterface` (and advertise `SAI_STATS_MODE_BULK_READ | SAI_STATS_MODE_BULK_READ_AND_CLEAR` in `queryStatsCapability`).
    - **SONiC consequence**: Functional parity is preserved (FlexCounter falls back to per-object `getStatsExt`), with slightly higher per-poll mutex/IPC overhead across 54 ports × 20 queues = ~1080 queues.
    - **Effort**: S.
    - **Testable in 1-switch + 2x 1G PC lab?**: **Yes** — verify syncd FlexCounter uses bulk polling without fallback messages.

---

## 3. Robustness Bugs in `SwitchBCM56860HW_qos.cpp`, `SwitchBCM56860HW_stats.cpp`, `hwbcm_qos.c`, `hwbcm_stats.c` (+ stats dispatch)

### Critical / High Severity

- **#R1 [High] — Buffer pool watermarks NEVER clear on read in SONiC (`SwitchBCM56860HW_stats.cpp:135-139, 388, 400` + `bufferorch.cpp:30-31, 316-350`)**
  - `bufferorch.cpp:316-324` probes watermark clear capability by calling `clear_buffer_pool_stats(pool, 2, {SAI_BUFFER_POOL_STAT_WATERMARK_BYTES, SAI_BUFFER_POOL_STAT_XOFF_ROOM_WATERMARK_BYTES})`.
  - Because `readThrough()` (`SwitchBCM56860HW_stats.cpp:135-139`) aborts the entire call with `SAI_STATUS_NOT_SUPPORTED` on the unsupported `XOFF_ROOM_WATERMARK_BYTES`, `BufferOrch` marks EVERY pool as `noWmClrCapability` and forces `STATS_MODE_READ` instead of `STATS_MODE_READ_AND_CLEAR` (`bufferorch.cpp:329-354`).
  - **Impact**: `show watermark` / telemetry buffer pool watermarks latch at their all-time boot peak and never reset on periodic poll or `sonic-clear watermark`.

- **#R2 [High] — Egress buffer pool watermarks for UC-only service pools (SP1..SP3) ALWAYS read 0 and burn ~432 SDK reads per poll (`hwbcm_stats.c:477-533, 551-557`)**
  - `hwbcm_pool_stat_multi_get` resolves an egress pool's SP via `bcmCosqControlUCEgressPool` or `bcmCosqControlMCEgressPool` (line 553), then calls `egr_pool_rep(sp, ...)` which calls `bcm_cosq_control_get(..., bcmCosqControlEgressPool, &got)` (line 479).
  - Verified in SDK `src/bcm/esw/trident2/cosq.c:2343-2358`: in non-ETS mode (plain port), `bcmCosqControlEgressPool` ONLY reads `MMU_THDM_MCQE_QUEUE_CONFIG` (multicast queue SPID)!
  - **Impact**: If an egress pool (`egress_lossless_pool` / `egress_lossy_pool`, SP1..SP3) only has **unicast** queues bound (standard SONiC config), `egr_pool_rep` fails to find `sp` on any port/cos, loops across all 54 ports × 8 cos on every poll, returns `BCM_E_NOT_FOUND` -> `soft_rv` -> silently reports `0` forever!

- **#R3 [High] — False `SAI_QUEUE_ATTR_PFC_DLR_INIT` capability breaks PFCWD recovery path (`SwitchStateBase.cpp:4165` not overridden by `SwitchBCM56860HW`, `SwitchBCM56860HW_qos.cpp:2857-2861`)**
  - `SwitchStateBase::queryAttributeCapability` returns `true` for all attributes, causing `switchorch.cpp:103` -> `orchdaemon.cpp:748` to select `PfcWdDlrHandler` (no-op on TD2+) instead of the working TD2+ SW handlers (`PfcWdAclHandler` / `PfcWdLossyHandler`). Combined with missing `ON2OFF_RX_PKTS` and hardcoded `PAUSE_STATUS = false`, PFCWD is completely dead.

- **#R4 [High] — `qos_cells()` 32-bit unsigned overflow & signed `int` overflow in `hwbcm_qos.c:88-91, 613, 618, 867, 878` + `SwitchBCM56860HW_qos.cpp:2548`**
  - `u32of(v)` (`SwitchBCM56860HW_qos.cpp:2548`) clamps large 64-bit buffer values to `0xffffffffu`.
  - `qos_cells(0xffffffffu)` (`hwbcm_qos.c:90`): `(0xffffffffu + 207u) / 208u` overflows `uint32` to `206 / 208 == 0` cells!
  - Furthermore, `(int)(min * 208)` (`hwbcm_qos.c:613, 618, 867, 878`) overflows signed `int` whenever `bytes > INT_MAX` (~2 GB), passing a negative byte count to `bcm_cosq_control_set`.
  - Similarly `qos_kbits()` (`hwbcm_qos.c:1017`): `(bytes_or_pkts * 8 + 999) / 1000` overflows `uint64_t` if `bytes_or_pkts > UINT64_MAX / 8`.

- **#R5 [High] — DB/SW-cache/HW desync on error in QoS `set()` & `qosHwSet*()` (`SwitchBCM56860HW.cpp:3135`, `SwitchBCM56860HW_qos.cpp:934-972, 1023, 1043, 2141-2148, 2844-2854, 2881-2888`)**
  - Both the vslib object hash (`SwitchBCM56850::set`) AND the C++ desired-state maps (`ps.maps[slot]`, `ps.defaultTc`, `ps.scheduler`, `ps.pfc*`, `m_qosSwitchMaps`, `m_qosSgSched`, `m_qosQueue*`, `m_qosPgProfile`) are mutated **before** calling `hwbcm_qos_*()`, with **no rollback** when the HW call fails.
  - Worse, in `hwbcm_qos_pg_buffer_set` (`hwbcm_qos.c:561-588`), if moving a PG to a different service pool fails partway (e.g., `old_min` is zeroed at line 573, then `PGHeadroomLimitBytes` or `IngressPool` fails at 578/587), `old_min` stays 0 in HW. And if `prio < 0` (no TC mapped to PG yet) while `old_min != 0 || old_hdrm != 0`, line 570 skips zeroing old min/hdrm and moves `THDI_PORT_PG_SPID` anyway, **corrupting both service pools' `THDI_BUFFER_CELL_LIMIT_SP` accounting**!

- **#R6 [High] — ECN-only WRED profiles inadvertently enable WRED dropping for non-ECT packets (`SwitchBCM56860HW_qos.cpp:2293`, `hwbcm_qos.c:1156-1185`)**
  - `SwitchBCM56860HW_qos.cpp:2293` sets `w.enable[c] = (on || ecnColor[c]) ? 1 : 0`. When a SONiC WRED profile configures `ecn=ecn_all` with `wred_green_enable=false` (ECN marking only, no dropping), `hwbcm_qos.c:1158-1161` sets `BCM_COSQ_DISCARD_ENABLE | BCM_COSQ_DISCARD_MARK_CONGESTION` with the curve thresholds active, which causes TD2+ to **WRED-drop non-ECT packets** on that queue even though drop was disabled!

### Medium Severity

- **#R7 [Medium] — `hwRemoveQosMap()` leaks port/switch bindings and `m_qosHwMaps` refcounts (`SwitchBCM56860HW_qos.cpp:1797-1818`)**
  - Unlike `hwRemoveScheduler`/`hwRemoveWred`/`hwRemoveBufferProfile`, `hwRemoveQosMap` only logs a warning if still bound to a port, does not check `m_qosSwitchMaps`, does not clear `kv.second.maps[slot]` / `m_qosSwitchMaps[slot]`, and does not unbind/release the `bcm_qos_map` in HW.
  - Related: `qosUnmapPort` (`SwitchBCM56860HW_qos.cpp:345-347`) calls `qosHwMapRelease()` **without** first detaching the maps from the port via `hwbcm_qos_port_ingress_maps_set(bcm, -1, -1)` / `hwbcm_qos_port_egress_map_set(bcm, -1)`. Destroying a `bcm_qos_map` still referenced by a HW port can fail (`BCM_E_BUSY`) or leave a dangling HW profile index.

- **#R8 [Medium] — `hwRemoveBufferPool()` frees service pool slot without checking in-use profiles/PGs/queues (`SwitchBCM56860HW_qos.cpp:2494-2510`)**
  - Immediately clears `m_qosIngressSpUsed`/`m_qosEgressSpUsed` even if `BUFFER_PROFILE`s, PGs, or queues still use that SP in HW. A subsequently created `BUFFER_POOL` reuses the same SP index, causing two pools' profiles to collide in HW.

- **#R9 [Medium] — `hwbcm_qos_pg_buffer_set()` makes it impossible to set `XON_OFFSET_TH = 0` (`hwbcm_qos.c:548-555`), and `hwbcm_qos_cos_sched_set()` cannot clear burst back to 0 (`hwbcm_qos.c:1075-1090`), and `hwbcm_qos_queue_buffer_set()` never restores `USE_QGROUP_MIN` when `min_bytes` changes back to 0 (`hwbcm_qos.c:851-862`)**
  - In `hwbcm_qos.c:548`: `if (offset == 0)` overwrites an explicit `0` with `s->offset` or PG7's offset.
  - In `hwbcm_qos.c:1075,1083`: `if (s != NULL && s->max_burst != 0 && kmax != 0)` skips updating `bcmCosqControlBandwidthBurstMax/Min` when a scheduler's burst is modified back to `0`, leaving the old HW burst sticky.
  - In `hwbcm_qos.c:851`: clearing `bcmCosqControlEgressUCQueueGroupMinEnable = 0` when `min_bytes > 0` is never reversed if `min_bytes` is later set back to `0`.

- **#R10 [Medium] — BST sync 500 ms throttle loses watermark peaks on `READ_AND_CLEAR` (`hwbcm_stats.c:199-236`)**
  - `bst_sync(bid)` skips `bcm_cosq_bst_stat_sync()` if `< 500 ms` elapsed since the last sync, even when `clear == 1` (`BCM_COSQ_BST_STAT_CLEAR_ON_READ`). Any peak occurring within that 500 ms window is discarded from HW before being synced to the SDK cache, so `READ_AND_CLEAR` returns stale data and clears the real peak.
  - Also line 214 sets `g_bst_last_ms[bid] = t` unconditionally even when `bcm_cosq_bst_stat_sync` failed, suppressing retries for 500 ms.

- **#R11 [Medium] — `SwitchBCM56860HW::getStatsExt` for `SAI_OBJECT_TYPE_PORT` silently returns `0` + `SAI_STATUS_SUCCESS` for unsupported stat IDs, AND `hwbcm_port_stat_multi_clear` clears shared HW MAC counters (`SwitchBCM56860HW.cpp:1252-1288`, `hwbcm_core.c:3100-3109, 3152`)**
  - Any unmapped `SAI_PORT_STAT_*` (e.g., `IN_DROP_REASON_RANGE_BASE`, `ECN_MARKED_PACKETS`) returns `0` with `SAI_STATUS_SUCCESS` instead of `SAI_STATUS_NOT_SUPPORTED`. Because `FlexCounter.cpp:2762` disables `queryStatsCapability` for `PORT_DEBUG`, FlexCounter's legacy probe accepts invalid port debug stats and polls zeros forever.
  - Clearing `IF_IN_NON_UCAST_PKTS` / `IF_OUT_NON_UCAST_PKTS` via `bcm_stat_clear_single` (`hwbcm_core.c:3152`) zeroes HW `snmpIfHCIn/OutMulticastPkts` and `snmpIfHCIn/OutBroadcastPkts`, corrupting `IF_IN/OUT_MULTICAST_PKTS` and `IF_IN/OUT_BROADCAST_PKTS`.
  - In `SwitchBCM56860HW.cpp:1270-1286`, if `hwPortStatsOverlay` updates `g_xbase` on `READ_AND_CLEAR` and `hwbcm_port_stat_multi_clear` then fails, `g_xbase` is already cleared while `SAI_STATUS_FAILURE` is returned.

- **#R12 [Medium] — `SAI_BUFFER_POOL_TYPE_BOTH` mishandled in both `qosGetBufferPoolAttr` (`SwitchBCM56860HW_qos.cpp:838-854`) and `hwStatsPoolRef` (`SwitchBCM56860HW_stats.cpp:248`)**
  - Both treat `TYPE_BOTH` as ingress-only, ignoring egress shared size and egress watermark. Also `hwCreateBufferPool` (`_qos.cpp:2405-2406`) logs `"no free ingress service pool"` even when ingress succeeded and egress exhausted.

- **#R13 [Medium] — Binding `DSCP_TO_COLOR` or `DOT1P_TO_COLOR` without `*_TO_TC` overwrites all TCs with 0 (`SwitchBCM56860HW_qos.cpp:1277-1342`)**
  - `uint8_t tc[64] = {0}` / `tc[8] = {0}`: if only a color map is bound (without a TC map), every DSCP/PCP is mapped to internal priority `0` in HW.

### Low Severity

- **#R14 [Low] — Unchecked return values during cold-fallback replay & port map (`SwitchBCM56860HW_qos.cpp:363, 596, 614, 642, 660, 676, 688, 711, 2754`)**
  - `qosBuildHwMap(false)`, `qosOnPortMapped`, and `qosRetryPendingPgs` cast every HW apply call to `(void)`, silently swallowing failures and returning `SAI_STATUS_SUCCESS`.

- **#R15 [Low] — Thread-safety observations (`Sai.cpp:494-607`, `hwbcm_stats.c:179-183, 317`, `hwbcm_qos.c`)**
  - Good news: `Sai.cpp` serializes ALL SAI API entry points (`create`, `set`, `remove`, `get`, `getStats`, `getStatsExt`, `clearStats`, `queryStatsCapability`) under a single global `MUTEX()`, so syncd's `FlexCounter` polling thread cannot race with the SAI API thread inside `SwitchBCM56860HW` or `hwbcm_qos.c`!
  - Minor lock-hygiene gaps in `hwbcm_stats.c`: `hwbcm_stats_bst_enabled()` (lines 179–183) reads `g_bst_on` outside `g_lock`, and `hwbcm_queue_stat_multi_get()` (line 317) reads `g_num_cpu_cos`/`g_num_cos` outside `g_lock`. `hwbcm_qos.c` has no internal mutex around `s_qos_ready`/`s_pg_snap`/`s_q_snap` (safe only because `Sai.cpp` holds the global mutex).

- **#R16 [Low] — `VirtualSwitchSaiInterface::clearStats` fixed 128-element stack buffer (`sonic-sairedis/vslib/VirtualSwitchSaiInterface.cpp:28, 1149`)**
  - Upstream vslib declares `uint64_t counters[VS_MAX_COUNTERS]` (`128`) without bounds-checking `number_of_counters`! Passing >128 counter IDs to `clearStats` causes a stack buffer overflow. (Currently max port stats polled is ~83, so safe today, but fragile.)

- **#R17 [Low] — CPU port queue watermarks/occupancy pass `bcmBstStatIdMcast` instead of `bcmBstStatIdCpuQueue` (`hwbcm_stats.c:367`)**
  - For CPU queues (`cpu == 1`, `port == CMIC_PORT`, `queue == 0..31`), `hwbcm_stats.c:367` queries `bcmBstStatIdMcast` rather than `bcmBstStatIdCpuQueue`, which is rejected as `BCM_E_PARAM` by `trident2/cosq.c` (`soft_rv` turns it into 0).

---

*Verification note: Every file:line reference, SAI enum, orchagent/syncd caller, and OpenBCM SDK 6.5.27 TD2+ API/register (`bcm56860_a0.c`, `trident2/cosq.c`, `triumph2/qos.c`, `stat_generic.c`, `pfc_deadlock.c`, `feature.c`) above was directly verified from source.*
