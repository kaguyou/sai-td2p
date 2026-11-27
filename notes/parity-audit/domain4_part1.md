# Domain 4 Audit Report (Part 1 of 2): Complete Attribute / API / Stat Coverage Table

Scope: `SwitchBCM56860HW` (`sai-td2p-merge/sairedis/vslib/SwitchBCM56860HW_{qos,stats}.*`, `hwbcm_{qos,stats}.*`, `SwitchBCM56860HW.cpp`, `hwbcm_core.c`) vs SONiC 202505 orchagent/syncd & Broadcom TD2+ (BCM56864, OpenBCM sdk-6.5.27).

Status legend:
- `(a) HW` — programmed to / read from TD2+ ASIC hardware
- `(b) silent no-op` — returns `SAI_STATUS_SUCCESS` (vslib DB updated only, HW untouched; some emit `SWSS_LOG_WARN`)
- `(c) error` — returns SAI error (`SAI_STATUS_NOT_SUPPORTED` / `SAI_STATUS_FAILURE` / `SAI_STATUS_NOT_IMPLEMENTED`)
- `(d) never reached` — no caller in SONiC 202505 `sonic-swss/orchagent` or gated off by capability query

---

## 1. Complete Coverage Table — QoS, Buffers, PFC/PFCWD, Headroom

### 1.1 `SAI_OBJECT_TYPE_QOS_MAP`
| Object / Feature | API / Attribute / Enum | Caller (`file:line`) | Handler (`file:line`) | Status | Notes |
|---|---|---|---|---|---|
| QOS_MAP | `create`/`set`/`remove` (`MAP_TO_VALUE_LIST`) | `qosorch.cpp:206,219,272,288` | `SwitchBCM56860HW.cpp:2992,3101,3177` -> `SwitchBCM56860HW_qos.cpp:1725-1818` | (a) HW (deferred to port bind) | Refcounted content-keyed `bcm_qos_map` cache (`m_qosHwMaps`). See Bug #R2 on `hwRemoveQosMap`. |
| QOS_MAP type | `DOT1P_TO_TC` | `qosorch.cpp:405` | `SwitchBCM56860HW_qos.cpp:1738,1311-1378` -> `hwbcm_qos.c:201-233` | (a) HW | `bcm_qos_map_create(INGRESS\|L2)` + `bcm_qos_port_map_set`. |
| QOS_MAP type | `DSCP_TO_TC` | `qosorch.cpp:264` | `SwitchBCM56860HW_qos.cpp:1740,1274-1378` -> `hwbcm_qos.c:170-199` | (a) HW | `bcm_qos_map_create(INGRESS\|L3)` + `bcm_qos_port_map_set`. |
| QOS_MAP type | `DSCP_TO_COLOR` | None in 202505 orchagent | `SwitchBCM56860HW_qos.cpp:1741,1275-1305` -> `hwbcm_qos.c:170-199` | (a) HW / (d) | Supported in HW when bound; no orchagent caller. Caution: without `DSCP_TO_TC`, `tc[64]` defaults to all 0s (`_qos.cpp:1279`). |
| QOS_MAP type | `DOT1P_TO_COLOR` | None in 202505 orchagent | `SwitchBCM56860HW_qos.cpp:1739,1312-1342` -> `hwbcm_qos.c:201-233` | (a) HW / (d) | Supported in HW when bound; same all-zero `tc[8]` caveat if bound alone. |
| QOS_MAP type | `TC_TO_QUEUE` | `qosorch.cpp:457` | `SwitchBCM56860HW_qos.cpp:1742,1477-1519` -> `hwbcm_qos.c:319-343` | (a) HW | `bcm_cosq_port_mapping_set`; cos >= 8 clamped to 7 (non-ETS `NUM_COS=8`). Applies to both UC & MC. |
| QOS_MAP type | `TC_TO_PRIORITY_GROUP` | `qosorch.cpp:912` | `SwitchBCM56860HW_qos.cpp:1744,1521-1565` -> `hwbcm_qos.c:487-514` | (a) HW | `bcm_port_priority_group_mapping_set`; inherits PG7 admission thresholds if target PG is unusable. |
| QOS_MAP type | `PFC_PRIORITY_TO_QUEUE` | `qosorch.cpp:1020` | `SwitchBCM56860HW_qos.cpp:1746,1584-1603` -> `hwbcm_qos.c:983-998` | (a) HW | `bcm_cosq_pfc_class_mapping_set` (applied when `pfcTouched == true`). |
| QOS_MAP type | `PFC_PRIORITY_TO_PRIORITY_GROUP` | `qosorch.cpp:965` | `SwitchBCM56860HW_qos.cpp:1745,1080-1098` | (b) silent no-op (for non-identity) | Identity is TD2+ native; non-identity logs WARN and returns `SAI_STATUS_SUCCESS` (no HW knob on TD2+). |
| QOS_MAP type | `TC_AND_COLOR_TO_DSCP` | `qosorch.cpp:1270` | `SwitchBCM56860HW_qos.cpp:1743,1381-1475` -> `hwbcm_qos.c:235-267` | (a) HW | `bcm_qos_map_create(EGRESS\|L3)` + `bcm_qos_port_map_set`. |
| QOS_MAP type | `TC_AND_COLOR_TO_DOT1P` | `qosorch.cpp:511` | `SwitchBCM56860HW_qos.cpp:1749-1752,984-991` | (b) silent no-op | Logs WARN at create and port bind; never programmed to HW (TD2+ supports `BCM_QOS_MAP_L2 \| BCM_QOS_MAP_EGRESS`). |
| QOS_MAP type | `MPLS_EXP_TO_TC` | `qosorch.cpp:334` | `SwitchBCM56860HW_qos.cpp:1749-1752`; port bind falls through `hwSetPort` default | (b) silent no-op | Logs WARN at create; `SAI_PORT_ATTR_QOS_MPLS_EXP_TO_TC_MAP` not in `qosIsPortAttr()` -> silent no-op. |
| QOS_MAP type | `MPLS_EXP_TO_COLOR` | None in 202505 orchagent | `SwitchBCM56860HW_qos.cpp:1749-1752` | (b) / (d) | Bookkeeping only, no caller in orchagent. |
| QOS_MAP type | `TC_AND_COLOR_TO_MPLS_EXP` | None in 202505 orchagent | `SwitchBCM56860HW_qos.cpp:1749-1752` | (b) / (d) | Bookkeeping only, no caller in orchagent. |
| QOS_MAP type | `DSCP_TO_FORWARDING_CLASS` | `qosorch.cpp:1103` | `SwitchBCM56860HW_qos.cpp:1749-1752`; port bind falls through `hwSetPort` | (b) silent no-op | Bookkeeping only (`SAI_PORT_ATTR_QOS_DSCP_TO_FORWARDING_CLASS_MAP` unhandled). |
| QOS_MAP type | `MPLS_EXP_TO_FORWARDING_CLASS` | `qosorch.cpp:1197` | `SwitchBCM56860HW_qos.cpp:1749-1752`; port bind falls through `hwSetPort` | (b) silent no-op | Bookkeeping only (`SAI_PORT_ATTR_QOS_MPLS_EXP_TO_FORWARDING_CLASS_MAP` unhandled). |

### 1.2 Port QoS / Buffer / PFC Attributes (`SAI_OBJECT_TYPE_PORT`)
| Object / Feature | API / Attribute | Caller (`file:line`) | Handler (`file:line`) | Status | Notes |
|---|---|---|---|---|---|
| PORT | `QOS_DSCP_TO_TC_MAP`, `QOS_DSCP_TO_COLOR_MAP`, `QOS_DOT1P_TO_TC_MAP`, `QOS_DOT1P_TO_COLOR_MAP`, `QOS_TC_TO_QUEUE_MAP`, `QOS_TC_TO_PRIORITY_GROUP_MAP`, `QOS_PFC_PRIORITY_TO_QUEUE_MAP`, `QOS_TC_AND_COLOR_TO_DSCP_MAP` | `qosorch.cpp:60-68,2080,2187` | `SwitchBCM56860HW.cpp:909` -> `SwitchBCM56860HW_qos.cpp:731-743,930-937` | (a) HW | All 8 map bindings wired to HW (with switch-level fallback for ingress/egress/tc-to-q). |
| PORT | `QOS_PFC_PRIORITY_TO_PRIORITY_GROUP_MAP` | `qosorch.cpp:67,2080` | `SwitchBCM56860HW_qos.cpp:740,1080-1098` | (b) silent no-op (non-identity) | TD2+ HW is identity-only; warns if `prio != pg`. |
| PORT | `QOS_TC_AND_COLOR_TO_DOT1P_MAP` | `qosorch.cpp:64,2080` | `SwitchBCM56860HW_qos.cpp:783,984-991` | (b) silent no-op | Logs WARN, returns `SAI_STATUS_SUCCESS`. |
| PORT | `QOS_MPLS_EXP_TO_TC_MAP`, `QOS_DSCP_TO_FORWARDING_CLASS_MAP`, `QOS_MPLS_EXP_TO_FORWARDING_CLASS_MAP` | `qosorch.cpp:61,70,71,2080` | Not in `qosIsPortAttr()` (`_qos.cpp:763-788`), falls through `hwSetPort` (`SwitchBCM56860HW.cpp:931`) | (b) silent no-op | No warning even logged on port bind! |
| PORT | `QOS_DEFAULT_TC` | None in 202505 orchagent | `SwitchBCM56860HW_qos.cpp:775,941-944,1615-1646` -> `hwbcm_qos.c:307-317` | (a) HW | `bcm_port_untagged_priority_set`. |
| PORT | `QOS_SCHEDULER_PROFILE_ID` | `qosorch.cpp:69,2080` | `SwitchBCM56860HW_qos.cpp:776,946-949,1648-1719` -> `hwbcm_qos.c:1097-1123` | (a) HW | `bcm_port_rate_egress_[pps_]set`. Min rate ignored with WARN (no port-level CIR in HW). |
| PORT | `QOS_NUMBER_OF_QUEUES`, `QOS_QUEUE_LIST` (get) | `portsorch.cpp:6061,6082` | `SwitchBCM56850.cpp:67-76,117-126`; `SwitchStateBase.cpp:2638-2639,3075` | (a) HW (vslib populated) | Exposes 20 queues/port (10 UC + 10 MC) & 32 CPU queues; note cos 8..9 unreachable in non-ETS HW (`hwbcm_qos.h:17`). |
| PORT | `QOS_NUMBER_OF_SCHEDULER_GROUPS`, `QOS_SCHEDULER_GROUP_LIST` (get) | `portsorch.cpp:6106,6126`; `qosorch.cpp:1521,1537` | `SwitchBCM56850.cpp:420-442`; `SwitchStateBase.cpp:2646-2647,3096` | (a) HW (vslib populated) | 13-node tree (root + 2 L0 + 10 L1); only root and L1 0..7 map to HW. |
| PORT | `NUMBER_OF_INGRESS_PRIORITY_GROUPS`, `INGRESS_PRIORITY_GROUP_LIST` (get) | `portsorch.cpp:6170` | `SwitchStateBase.cpp:1545-1554,2642-2643,3084` | (a) HW (vslib populated) | 8 PGs per port, matches TD2+ `HWBCM_QOS_NUM_PG = 8`. |
| PORT | `QOS_MAXIMUM_HEADROOM_SIZE` (get) | `portsorch.cpp:6191-6202` | Not stored in `SwitchStateBase`, not handled in `SwitchBCM56860HW::get()` (`SwitchBCM56860HW.cpp:3253-3335`) | (c) error | Returns `SAI_STATUS_FAILURE` ("failed to find attribute"); `portsorch` logs NOTICE and omits `max_headroom_size` from `STATE_DB|BUFFER_MAX_PARAM_TABLE`. |
| PORT | `PRIORITY_FLOW_CONTROL_MODE`, `PRIORITY_FLOW_CONTROL`, `PRIORITY_FLOW_CONTROL_RX`, `PRIORITY_FLOW_CONTROL_TX` | `portsorch.cpp:2226,2230,2311,2336` | `SwitchBCM56860HW_qos.cpp:777-780,951-972,1567-1613` -> `hwbcm_qos.c:957-1004` | (a) HW | `bcm_port_priority_group_config_set` (TX), `bcm_cosq_pfc_class_mapping_set` + `bcm_port_control_set(bcmPortControlPFCReceive)` (RX). |
| PORT | `QOS_INGRESS_BUFFER_PROFILE_LIST`, `QOS_EGRESS_BUFFER_PROFILE_LIST` | `bufferorch.cpp:1626,1774,1816,1964` | `SwitchBCM56860HW_qos.cpp:781-782,974-982` | (b) silent no-op | Logs WARN if count != 0; port-SP limits never programmed to HW. |

### 1.3 `SAI_OBJECT_TYPE_SCHEDULER` & `SAI_OBJECT_TYPE_SCHEDULER_GROUP`
| Object / Feature | API / Attribute | Caller (`file:line`) | Handler (`file:line`) | Status | Notes |
|---|---|---|---|---|---|
| SCHEDULER | `create`/`set`/`remove` | `qosorch.cpp:1445,1459,1489` | `SwitchBCM56860HW.cpp:2993,3102,3178` -> `SwitchBCM56860HW_qos.cpp:1882-1983` | (a) HW | Re-applies on `set()` to all bound SGs, queues, and ports. |
| SCHEDULER | `SCHEDULING_TYPE` (`STRICT`/`DWRR`/`WRR`), `SCHEDULING_WEIGHT` | `qosorch.cpp:1379,1400` | `SwitchBCM56860HW_qos.cpp:1839-1852,1998-2079` -> `hwbcm_qos.c:1039-1071` | (a) HW | `bcm_cosq_gport_sched_set(U, port, cos, mode, weight)` (weight clamped 1..127). |
| SCHEDULER | `METER_TYPE` (`BYTES`/`PACKETS`), `MIN_BANDWIDTH_RATE`, `MAX_BANDWIDTH_RATE` | `qosorch.cpp:1407,1413,1425` | `SwitchBCM56860HW_qos.cpp:1854-1873` -> `hwbcm_qos.c:1060-1073,1104-1120` | (a) HW | `bcm_cosq_gport_bandwidth_set` (cos 0..7) / `bcm_port_rate_egress_[pps_]set` (port). |
| SCHEDULER | `MIN_BANDWIDTH_BURST_RATE`, `MAX_BANDWIDTH_BURST_RATE` | `qosorch.cpp:1419,1431` | `SwitchBCM56860HW_qos.cpp:1864-1877` -> `hwbcm_qos.c:1075-1090,1106-1120` | (a) HW | `bcmCosqControlBandwidthBurstMax/Min`. See Bug #R9: clearing burst back to 0 is not applied to HW! |
| SCHEDULER_GROUP | `PORT_ID`, `CHILD_COUNT`, `CHILD_LIST` (get) | `qosorch.cpp:1576,1600` | `SwitchBCM56850.cpp:219-403`; `SwitchStateBase.cpp:2691-2692` | (a) HW (vslib populated) | Static 13-node tree. |
| SCHEDULER_GROUP | `LEVEL`, `MAX_CHILDS`, `PARENT_NODE` | Not queried by `qosorch.cpp` (only `CHILD_COUNT`/`CHILD_LIST`) | `SwitchBCM56850.cpp:251-400` does NOT set `LEVEL`/`MAX_CHILDS`/`PARENT_NODE`! | (c) error if queried / (d) | `SwitchBCM56850` omits `LEVEL`, `MAX_CHILDS`, `PARENT_NODE` on created SGs (reconstructed by `qosMapPort` via `CHILD_LIST` traversal). |
| SCHEDULER_GROUP | `SCHEDULER_PROFILE_ID` (set on L1 cos 0..7 / root L0) | `qosorch.cpp:1688-1691` | `SwitchBCM56860HW_qos.cpp:2081-2151` -> `hwbcm_qos.c:1022-1123` | (a) HW (root & L1 0..7); (b) silent no-op (L0 & L1 8..9) | L0 groups (`level==1`) and L1 cos 8..9 log WARN and return `SAI_STATUS_SUCCESS` (non-ETS mode). |

### 1.4 `SAI_OBJECT_TYPE_QUEUE`
| Object / Feature | API / Attribute | Caller (`file:line`) | Handler (`file:line`) | Status | Notes |
|---|---|---|---|---|---|
| QUEUE | `TYPE`, `PORT`, `INDEX` (get) | `portsorch.cpp:3386-3387` | `SwitchBCM56850.cpp:51-65,101-115` | (a) HW (vslib populated) | Front ports: UC 0..9, MC 10..19; CPU: MC 0..31. |
| QUEUE | `PARENT_SCHEDULER_NODE` | Not set/get by orchagent | `SwitchBCM56850.cpp` does not set; `hwSetQueue:2865` no-op | (b) / (d) | Tree is fixed in non-ETS mode. |
| QUEUE | `WRED_PROFILE_ID` | `qosorch.cpp:1734-1736` | `SwitchBCM56860HW_qos.cpp:2847-2850,2224-2345` -> `hwbcm_qos.c:1129-1189` | (a) HW (UC 0..7); (b) (MC & UC 8..9) | `bcm_cosq_gport_discard_set` on UC 0..7; MC/8..9 WARN no-op. |
| QUEUE | `BUFFER_PROFILE_ID` | `bufferorch.cpp:982,1223`; `pfcactionhandler.cpp:676,688,708` | `SwitchBCM56860HW.cpp:3205` (`bulkSet`) -> `SwitchBCM56860HW_qos.cpp:2852-2855,2758-2811` -> `hwbcm_qos.c:816-901` | (a) HW (cos 0..7); (b) (cos 8..9 & CPU) | Programs pool, min, dyn/alpha or static, limit_en, clears `DISABLE_QUEUING`. |
| QUEUE | `SCHEDULER_PROFILE_ID` | Not directly set by `qosorch.cpp` (uses SG) | `SwitchBCM56860HW_qos.cpp:2842-2845,2114-2128` -> `hwbcm_qos.c:1022-1095` | (a) HW (cos 0..7) | UC/MC share L1 node; UC takes precedence over MC. |
| QUEUE | `PFC_DLR_INIT` | `switchorch.cpp:103` (cap query); `pfcactionhandler.cpp:230,253,273,296` | `SwitchBCM56860HW_qos.cpp:2857-2861` | (b) silent no-op | Logs WARN and returns `SAI_STATUS_SUCCESS`! **Worse**: `SwitchStateBase::queryAttributeCapability` (`SwitchStateBase.cpp:4165`) falsely reports it as implemented, forcing `PfcWdDlrHandler`! |
| QUEUE | `ENABLE_PFC_DLDR` | None in 202505 BRCM path (Mellanox/HW-PFCWD path) | `SwitchBCM56860HW_qos.cpp:2858-2861` | (b) silent no-op / (d) | Logs WARN, returns `SAI_STATUS_SUCCESS`. |
| QUEUE | `PAUSE_STATUS` (get, polled by FlexCounter `QUEUE_ATTR`) | `orchdaemon.cpp:727` -> `syncd/FlexCounter.cpp:1678` -> `pfc_detect_broadcom.lua:50` | `SwitchStateBase.cpp:2386-2402,2717` (NOT overridden in `SwitchBCM56860HW`) | (b) silent no-op (hardcoded `false`) | Always returns `false`! Never reads `XPORT_TO_MMU_BKPr` / `bcm_td2_cosq_pause_queue_status_get`! Completely breaks BRCM PFCWD detection. |
| QUEUE | `TAM_OBJECT` | None in 202505 orchagent | `hwSetQueue:2863` default | (b) / (d) | Not implemented in HW; no orchagent caller. |

### 1.5 `SAI_OBJECT_TYPE_WRED`
| Object / Feature | API / Attribute | Caller (`file:line`) | Handler (`file:line`) | Status | Notes |
|---|---|---|---|---|---|
| WRED | `create`/`set`/`remove` | `qosorch.cpp:773,854,867` | `SwitchBCM56860HW.cpp:2994,3103,3179` -> `SwitchBCM56860HW_qos.cpp:2157-2222` | (a) HW | Re-applies on `set()` to all bound queues. |
| WRED | `GREEN/YELLOW/RED_ENABLE`, `*_MIN_THRESHOLD`, `*_MAX_THRESHOLD`, `*_DROP_PROBABILITY`, `WEIGHT` | `qosorch.cpp:638-738,793,837-851` | `SwitchBCM56860HW_qos.cpp:2260-2335` -> `hwbcm_qos.c:1129-1189` | (a) HW | `bcm_cosq_gport_discard_set` (TCP + non-TCP). |
| WRED | `ECN_MARK_MODE` (`NONE`/`GREEN`/`YELLOW`/`RED`/`ALL`/combos) | `qosorch.cpp:742-745` | `SwitchBCM56860HW_qos.cpp:2270-2332` -> `hwbcm_qos.c:1159-1161` | (a) HW (with caveat) | TD2+ `ECN_MARKING_EN` is per-queue (not per-color). See Bug #R6: enabling ECN with `*_ENABLE=false` also enables WRED drops for non-ECT packets! |
| WRED | `ECN_GREEN/YELLOW/RED_MIN/MAX_THRESHOLD`, `ECN_*_MARK_PROBABILITY` | None in 202505 `qosorch.cpp` (only used when SAI callers pass them directly) | `SwitchBCM56860HW_qos.cpp:2264-2322` | (a) HW (fallback only) / (b) if drop also enabled | Only used when `*_ENABLE=false && ecnColor[c]`; silently ignored if drop is also enabled on that color (TD2+ shares one curve per color). |

### 1.6 `SAI_OBJECT_TYPE_BUFFER_POOL`, `BUFFER_PROFILE`, `INGRESS_PRIORITY_GROUP`
| Object / Feature | API / Attribute | Caller (`file:line`) | Handler (`file:line`) | Status | Notes |
|---|---|---|---|---|---|
| BUFFER_POOL | `create`/`remove`, `TYPE` (`INGRESS`/`EGRESS`/`BOTH`) | `bufferorch.cpp:443-458,525,570`; `pfcactionhandler.cpp:789-797,850` | `SwitchBCM56860HW_qos.cpp:2351-2429,2494-2510` | (a) HW (SP allocation) | Maps to TD2+ service pool 0..3 per direction (`INSUFFICIENT_RESOURCES` if >4). See Bug #R3 on `remove`. |
| BUFFER_POOL | `SIZE` (create / set) | `bufferorch.cpp:425,505,525`; `pfcactionhandler.cpp:785` | `SwitchBCM56860HW_qos.cpp:2368-2371,2422,2446-2451` | (b) silent no-op | **Recorded in SW only!** Never calls `bcm_cosq_control_set(bcmCosqControlIngressPoolLimitBytes / bcmCosqControlEgressPoolSharedLimitBytes)`. Init-MMU defaults remain in HW! |
| BUFFER_POOL | `THRESHOLD_MODE` (`DYNAMIC`/`STATIC`) | `bufferorch.cpp:474-485,505` | `SwitchBCM56860HW_qos.cpp:2373-2376,2453-2491` | (a) HW | Used as default threshold mode for profiles in this pool; re-applies profiles on `set()`. |
| BUFFER_POOL | `XOFF_SIZE` (shared headroom pool) | `bufferorch.cpp:491,505` | `SwitchBCM56860HW_qos.cpp:2458-2464` | (b) silent no-op | Logs WARN if != 0. TD2+ has **no** shared headroom pool in HW (only per-PG headroom + global headroom pool; SHP is TH/TD3+). |
| BUFFER_POOL | `SHARED_SIZE` (get) | Not directly called by 202505 `bufferorch.cpp` | `SwitchBCM56860HW.cpp:3294-3332` -> `SwitchBCM56860HW_qos.cpp:819-855` -> `hwbcm_qos.c:903-923` | (a) HW | Reads `THDI_BUFFER_CELL_LIMIT_SPr` / `MMU_THDM_DB_POOL_SHARED_LIMITr`. (For `TYPE_BOTH`, only returns ingress). |
| BUFFER_POOL | `WRED_PROFILE_ID`, `TAM` | None in 202505 orchagent | `hwSetBufferPool:2466` default | (b) / (d) | No HW programming; no orchagent caller. |
| BUFFER_PROFILE | `create`/`set`/`remove` | `bufferorch.cpp:768,783,798,832`; `pfcactionhandler.cpp:827,843` | `SwitchBCM56860HW_qos.cpp:2590-2675` | (a) HW | Re-applies on `set()` to all bound PGs & queues. |
| BUFFER_PROFILE | `POOL_ID`, `RESERVED_BUFFER_SIZE` (= `BUFFER_SIZE`), `THRESHOLD_MODE`, `SHARED_DYNAMIC_TH`, `SHARED_STATIC_TH` | `bufferorch.cpp:658-718`; `pfcactionhandler.cpp:811-823` | `SwitchBCM56860HW_qos.cpp:2512-2588,2677-2811` -> `hwbcm_qos.c:516-623,816-901` | (a) HW | `BUFFER_SIZE` is an enum alias of `RESERVED_BUFFER_SIZE` (`saibuffer.h:683`). Alpha clamped to -7..3 (1/128..8). |
| BUFFER_PROFILE | `XOFF_TH`, `XON_TH`, `XON_OFFSET_TH` | `bufferorch.cpp:665-677` | `SwitchBCM56860HW_qos.cpp:2572-2585,2708-2710` -> `hwbcm_qos.c:543-555,590-619` | (a) HW | PG headroom (`bcmCosqControlIngressPortPGHeadroomLimitBytes`) & resume floor/offset (`THDI_PORT_PG_CONFIG`). Note Bug #R7 when `XON_OFFSET_TH == 0`! |
| BUFFER_PROFILE | `PACKET_ADMISSION_FAIL_ACTION` (`DROP` / `DROP_AND_TRIM`; note `PACKET_DISCARD_ACTION` is not in SAI 1.15 `saibuffer.h`) | `bufferorch.cpp:724-732` | Not decoded in `qosDecodeBufferProfile()` (`_qos.cpp:2512-2588`) | (b) silent no-op | `SwitchStateBase.cpp:4100-4101` falsely advertises `DROP_AND_TRIM` as supported! TD2+ has no packet trimming HW. |
| INGRESS_PRIORITY_GROUP | `PORT`, `INDEX` (get) | `portsorch.cpp:6170` | `SwitchStateBase.cpp:1529-1538` | (a) HW (vslib populated) | PGs 0..7 per port. |
| INGRESS_PRIORITY_GROUP | `BUFFER_PROFILE` (set / bulkSet) | `bufferorch.cpp:1379,1577` | `SwitchBCM56860HW.cpp:3205` (`bulkSet`) -> `SwitchBCM56860HW_qos.cpp:2870-2891,2677-2733` -> `hwbcm_qos.c:516-623` | (a) HW | Deferred via `m_qosPgPending` if no TC maps to PG yet (`BCM_E_NOT_FOUND`), retried on `TC_TO_PG`. |
| INGRESS_PRIORITY_GROUP | `TAM` | None in 202505 orchagent | `hwSetIngressPriorityGroup:2876` | (b) / (d) | Ignored; no orchagent caller. |

### 1.7 Switch QoS / Buffer / PFC Attributes (`SAI_OBJECT_TYPE_SWITCH`)
| Object / Feature | API / Attribute | Caller (`file:line`) | Handler (`file:line`) | Status | Notes |
|---|---|---|---|---|---|
| SWITCH | `QOS_DSCP_TO_TC_MAP`, `QOS_DSCP_TO_COLOR_MAP`, `QOS_DOT1P_TO_TC_MAP`, `QOS_DOT1P_TO_COLOR_MAP`, `QOS_TC_TO_QUEUE_MAP`, `QOS_TC_AND_COLOR_TO_DSCP_MAP`, `QOS_DEFAULT_TC` | `qosorch.cpp:1950-2024` (`DSCP_TO_TC`) | `SwitchBCM56860HW.cpp:1305` -> `SwitchBCM56860HW_qos.cpp:746-761,790-796,998-1055` | (a) HW | Applied as switch-wide fallback to all ports without port-level override. |
| SWITCH | `TOTAL_BUFFER_SIZE` (get) | `bufferorch.cpp:213-227` | `SwitchBCM56860HW.cpp:3294-3332` -> `SwitchBCM56860HW_qos.cpp:798-817` -> `hwbcm_qos.c:925-951` | (a) HW | Returns MMU total buffer in KB from `soc_td2p_mmu_params_arr_get` (16384 KB on TD2+). |
| SWITCH | `QOS_MAX_NUMBER_OF_TRAFFIC_CLASSES`, `QOS_MAX_NUMBER_OF_CHILDS_PER_SCHEDULER_GROUP`, `NUMBER_OF_UNICAST_QUEUES`, `NUMBER_OF_MULTICAST_QUEUES`, `NUMBER_OF_QUEUES`, `NUMBER_OF_ECMP_GROUPS` | `routeorch.cpp:60` (`ECMP_GROUPS`); vslib init | `SwitchStateBase.cpp:1794,1817`; `SwitchBCM56850.cpp:157-174,461` | (a) vslib constants | Note `NUMBER_OF_UNICAST/MULTICAST_QUEUES` = 10 while non-ETS HW only accesses 0..7. |
| SWITCH | `PFC_DLR_PACKET_ACTION` | `pfcwdorch.cpp:242-245` | NOT in `qosIsSwitchAttr()` (`_qos.cpp:790`) or `hwSetSwitch()` (`SwitchBCM56860HW.cpp:1312`) | (b) silent no-op | Triggered whenever PFCWD is configured on BRCM because `queryAttributeCapability(QUEUE, PFC_DLR_INIT)` falsely returns true! |
| SWITCH | `ECN_ECT_THRESHOLD_ENABLE`, `QUEUE_PFC_DEADLOCK_NOTIFY`, `PFC_TC_DLD_INTERVAL_RANGE`, `PFC_TC_DLR_INTERVAL_RANGE`, `PFC_TC_DLD_INTERVAL`, `PFC_TC_DLR_INTERVAL`, `INGRESS_BUFFER_POOL_NUM`, `EGRESS_BUFFER_POOL_NUM`, `DEFAULT_INGRESS_BUFFER_POOL_SHARED_SIZE`, `DEFAULT_EGRESS_BUFFER_POOL_SHARED_SIZE` | None in 202505 `sonic-swss/orchagent` | Not handled in `SwitchBCM56860HW` | (b) set / (c) get / (d) | Never called by 202505 orchagent on BRCM (DLD/DLR intervals are for Mellanox/HW-PFCWD; TD2+ lacks HW PFC deadlock engine). |

---

## 2. Complete Coverage Table — Counters, Watermarks, Flex Counters, Debug Counters

### 2.1 Port Stats (`SAI_OBJECT_TYPE_PORT`)
| Stat Group / Stat ID | Caller (`file:line`) | Handler (`file:line`) | Advertised in `queryStatsCapability`? | Status | Notes |
|---|---|---|---|---|---|
| Standard RFC/Ether/RMON/Pause/PFC RX+TX (69 stats in `hwPstatOf`) | `portsorch.cpp:186-247`; `orchdaemon.cpp:701-708`; `countercheckorch.cpp:155-162` | `SwitchBCM56860HW.cpp:1166-1209,1252-1288` -> `hwbcm_core.c:3020-3160` | Yes (`_stats.cpp:477`) | (a) HW | `bcm_stat_get` / `bcm_stat_clear_single`. Covers IF_IN/OUT octets/pkts/discards/errors/qlen, ETHER_STATS_*, ETHER_IN/OUT_PKTS_64..16383, PAUSE_RX/TX, PFC_0..7_RX/TX_PKTS. |
| MMU drops, WRED drops, >1518 EtherStats, IP/IPv6 (14 stats in `g_portXStats`) | `portsorch.cpp:247,295-296,331-334,1676-1695` | `SwitchBCM56860HW_stats.cpp:77-92,419-458` -> `hwbcm_stats.c:610-689` | Yes (`_stats.cpp:477`) | (a) HW | `IN_DROPPED_PKTS`, `OUT_DROPPED_PKTS`, `GREEN/YELLOW/RED/WRED_DROPPED_PACKETS`, `ETHER_STATS_PKTS_1519..16383`, `IP/IPV6_IN_RECEIVES/DISCARDS`. SW baseline for READ_AND_CLEAR. |
| `SAI_PORT_STAT_PFC_0..7_ON2OFF_RX_PKTS` | `orchdaemon.cpp:709-716`; `pfc_detect_broadcom.lua:43,49,52` | **MISSING** from `hwPstatOf` & `g_portXStats`! | **No** (filtered out by FlexCounter!) | (b) silent 0 on direct get / filtered out by FlexCounter | **CRITICAL for PFCWD**: `pfc_detect_broadcom.lua:52` skips deadlock detection completely if `pfc_on2off` is nil! TD2+ HW has `RPFCOFF0..7` (`snmpBcmRxPFCFrameXonPriority0..7`)! |
| `SAI_PORT_STAT_IP_IN_UCAST_PKTS` | `portsorch.cpp:243` | Missing from `hwPstatOf` & `g_portXStats` | No | (b) / filtered | Filtered out by `queryStatsCapability`. |
| `SAI_PORT_STAT_IF_IN_VLAN_DISCARDS` | `SwitchStateBase.cpp:4186` (not in `portsorch.cpp` list) | Missing | No | (d) | Not polled by `portsorch.cpp`. |
| `SAI_PORT_STAT_GREEN/YELLOW/RED/WRED_DROPPED_BYTES` | Not in `portsorch.cpp:329-335` (only `PACKETS`) | Missing | No | (d) | TD2+ `trident2/cosq.c` has no WRED drop-byte counter (`bcmCosqStat*DiscardDroppedBytes` not in TD2 `cosq.c`). |
| `SAI_PORT_STAT_ECN_MARKED_PACKETS` | Not in `portsorch.cpp` (only `WRED_ECN_QUEUE` uses `SAI_QUEUE_STAT_WRED_ECN_MARKED_*`) | Missing | No | (b) / (d) | `EGR_ECN_COUNTER` is NULL on BCM56860 (`bcm56860_a0.c:28487`). |
| `SAI_PORT_STAT_PFC_0..7_RX/TX_PAUSE_DURATION_US` | Not in `portsorch.cpp` / `orchdaemon.cpp` | Missing | No | (d) | No HW register on TD2+ (`PFC_XOFF_TIMER` is NULL in `bcm56860_a0.c:82341`). |
| `SAI_PORT_STAT_IF_IN_FEC_*` (20 stats) | `portsorch.cpp:248-267` | Missing | No | (b) / filtered | Filtered out; 1G/10G copper ports on AS5812-54T don't use RS-FEC (40G KR4/SR4 QSFP ports have limited CL74 FC-FEC only). |
| `SAI_PORT_STAT_TRIM_PACKETS` | `portsorch.cpp:268` | Missing | No | (b) / filtered | Not supported on TD2+ HW. |
| `SAI_PORT_STAT_IN/OUT_DROP_REASON_RANGE_BASE + idx` | `debug_counter/drop_counter.cpp:126,130`; `FlexCounter.cpp:2758-2765` | Falls through `hwPstatOf` -> returns `0` with `SAI_STATUS_SUCCESS`! | N/A (`FlexCounter.cpp:2762` disables capa query!) | (b) silent no-op (returns 0) | See Debug Counters below! |

### 2.2 Queue Stats & Watermarks (`SAI_OBJECT_TYPE_QUEUE`)
| Stat ID | Caller (`file:line`) | Handler (`file:line`) | Advertised? | Status | Notes |
|---|---|---|---|---|---|
| `PACKETS`, `BYTES`, `DROPPED_PACKETS`, `DROPPED_BYTES` | `portsorch.cpp:301-304`; `orchdaemon.cpp:721`; `pfcactionhandler.cpp:603-604`; `countercheckorch.cpp:213` | `SwitchBCM56860HW_stats.cpp:45-48,344-360` -> `hwbcm_stats.c:273-342` | Yes | (a) HW (cos 0..7 & CPU 0..31); (b) 0 for cos 8..9 | `bcm_cosq_stat_get` (TX) + `soc_counter_get(NON_DMA_COSQ_DROP_*)` (drops). SW baseline for READ_AND_CLEAR. |
| `CURR_OCCUPANCY_BYTES/CELLS`, `SHARED_CURR_OCCUPANCY_BYTES/CELLS` | `orchdaemon.cpp:722` (`CURR_OCCUPANCY_BYTES`) | `SwitchBCM56860HW_stats.cpp:49-52` -> `hwbcm_stats.c:343-364` | Yes | (a) HW (cos 0..7) | `bcmCosqStatEgress{UC,MC}Queue{Min,}BytesCurrent`. |
| `WATERMARK_BYTES/CELLS`, `SHARED_WATERMARK_BYTES/CELLS` | `portsorch.cpp:315`; `watermarkorch.cpp:199-211,266-272` | `SwitchBCM56860HW_stats.cpp:53-56` -> `hwbcm_stats.c:365-371` | Yes | (a) HW (cos 0..7) | BST `bcmBstStatIdUcast`/`Mcast` (total queue peak; TD2+ lacks separate shared-only queue BST). |
| `WRED_DROPPED_PACKETS`, `WRED_DROPPED_BYTES`, `GREEN/YELLOW/RED_DROPPED_*`, `WRED_ECN_MARKED_PACKETS`, `WRED_ECN_MARKED_BYTES` | `portsorch.cpp:339-342,1629-1648` | Not in `g_queueStats` -> returns `SAI_STATUS_NOT_SUPPORTED` | No | (c) error (filtered by capa query) | `portsorch.cpp:1607-1652` sets STATE_DB WRED queue capabilities to `false`. TD2+ has NO per-queue WRED drop or ECN-marked counters in `trident2/cosq.c`! |
| `PAUSE_STATUS`, `PFC_WD_*`, `TRIM_PACKETS`, `CREDIT_WD_DELETED_PACKETS` | `portsorch.cpp:305,309` (`TRIM`, `CREDIT_WD`); PFCWD uses `QUEUE_ATTR_PAUSE_STATUS` not stat | Not in `g_queueStats` -> `SAI_STATUS_NOT_SUPPORTED` | No | (c) error (filtered) | Not supported on TD2+ (voq/trim are non-TD2+; `SAI_QUEUE_STAT_PAUSE_STATUS` not used by BRCM orchagent). |

### 2.3 Ingress Priority Group Stats & Watermarks (`SAI_OBJECT_TYPE_INGRESS_PRIORITY_GROUP`)
| Stat ID | Caller (`file:line`) | Handler (`file:line`) | Advertised? | Status | Notes |
|---|---|---|---|---|---|
| `CURR_OCCUPANCY_BYTES/CELLS`, `SHARED_CURR_OCCUPANCY_BYTES/CELLS`, `XOFF_ROOM_CURR_OCCUPANCY_BYTES/CELLS` | Not polled by default flex counters (available on demand) | `SwitchBCM56860HW_stats.cpp:60-65,362-377` -> `hwbcm_stats.c:431-449` | Yes | (a) HW | `bcmCosqStatIngressPortPG{Min,Shared,Headroom}BytesCurrent`. |
| `SHARED_WATERMARK_BYTES/CELLS`, `XOFF_ROOM_WATERMARK_BYTES/CELLS` | `portsorch.cpp:320-321`; `watermarkorch.cpp:187,193,260,263` | `SwitchBCM56860HW_stats.cpp:66-69` -> `hwbcm_stats.c:450-463` | Yes | (a) HW | BST `bcmBstStatIdPriGroupShared` / `bcmBstStatIdPriGroupHeadroom`. |
| `PACKETS`, `BYTES`, `DROPPED_PACKETS` | `portsorch.cpp:326` (`PG_DROP`); **`pfcactionhandler.cpp:609-610,640` (DIRECT call!)** | Not in `g_pgStats` -> returns `SAI_STATUS_NOT_SUPPORTED` | No | (c) error | Filtered out by `PG_DROP` FlexCounter, **but** `PfcWdLossyHandler::getHwCounters()` calls `get_ingress_priority_group_stats({PACKETS, DROPPED_PACKETS})` directly and fails! Note: TD2+ MMU has no per-PG packet/drop counter (`trident2/cosq.c`). |
| `WATERMARK_BYTES` (total PG watermark) | Not polled by `portsorch.cpp` / `watermarkorch.cpp` | Not in `g_pgStats` -> `SAI_STATUS_NOT_SUPPORTED` | No | (c) / (d) | TD2+ BST tracks shared and headroom separately (`PriGroupShared`, `PriGroupHeadroom`). |

### 2.4 Buffer Pool Stats & Watermarks (`SAI_OBJECT_TYPE_BUFFER_POOL`)
| Stat ID | Caller (`file:line`) | Handler (`file:line`) | Advertised? | Status | Notes |
|---|---|---|---|---|---|
| `WATERMARK_BYTES`, `WATERMARK_CELLS` | `bufferorch.cpp:30,316`; `watermarkorch.cpp:217,275` | `SwitchBCM56860HW_stats.cpp:72-75,379-412` -> `hwbcm_stats.c:535-588` | Yes | (a) HW (broken for UC-only egress SP1..3 & clear probe!) | See **Bug #R1** (`clear_buffer_pool_stats` probe fails -> never clears!) & **Bug #R4** (`egr_pool_rep` returns 0 for UC-only egress SP1..3!). |
| `XOFF_ROOM_WATERMARK_BYTES` | `bufferorch.cpp:31,316`; `watermarkorch.cpp:223,278` | Not in `g_poolStats` -> returns `SAI_STATUS_NOT_SUPPORTED` | No | (c) error | **Directly triggers Bug #R1** in `bufferorch.cpp:316-333`! TD2+ has no shared headroom pool BST stat. |
| `CURR_OCCUPANCY_BYTES`, `XOFF_ROOM_CURR_OCCUPANCY_BYTES`, `DROPPED_PACKETS` | Not polled by `bufferorch.cpp` / `watermarkorch.cpp` | Not in `g_poolStats` -> `SAI_STATUS_NOT_SUPPORTED` | No | (c) / (d) | TD2+ lacks `bcmCosqStat{Ingress,Egress}PoolSharedBytesCurrent` in `trident2/cosq.c` (per-port pool current exists: `IngressPortPoolSharedBytesCurrent`, `EgressPortPoolSharedBytesCurrent`). |

### 2.5 RIF, Tunnel, ACL Counter, Policer, Counter Objects, Debug Counters, Bulk Stats
| Object / Feature | API / Stat | Caller (`file:line`) | Handler (`file:line`) | Status | Notes |
|---|---|---|---|---|---|
| `ROUTER_INTERFACE` stats | `IN/OUT_PACKETS`, `IN/OUT_OCTETS`, `IN/OUT_ERROR_PACKETS/OCTETS` | `intfsorch.cpp:50-57`; `flexcounterorch.cpp:232-235`; `FlexCounter.cpp:2788-2791` | Falls through `SwitchBCM56860HW::getStatsExt:1248` -> `SwitchState::getStatsExt` (vslib 0s); `queryStatsCapability` falls through `SwitchStateBase:4344` (`SAI_STATUS_NOT_SUPPORTED`) -> FlexCounter legacy probe succeeds with 0s! | (b) silent no-op (always 0) | No `bcm_l3_ingress_stat_*` / `bcm_l3_intf_stat_*` flex counter attached on RIFs. `show interfaces counters rif` shows all 0s! |
| `TUNNEL` stats | `IN/OUT_OCTETS`, `IN/OUT_PACKETS` | `vxlanorch.cpp:71-74`; `flexcounterorch.cpp:244-247`; `FlexCounter.cpp:2823-2828` | Falls through `SwitchBCM56860HW::getStatsExt:1248` -> `SwitchState::getStatsExt` (vslib 0s) | (b) silent no-op (always 0) | No `bcm_vxlan_stat_attach` / `bcm_stat_group_create` on VXLAN tunnels. |
| `ACL_COUNTER` | `SAI_ACL_COUNTER_ATTR_PACKETS`, `SAI_ACL_COUNTER_ATTR_BYTES` (get/set) | `aclorch.cpp:513-514`; `FlexCounter.cpp:2861-2864` (`ATTR_TYPE_ACL_COUNTER`) | `SwitchBCM56860HW.cpp:3176,3261-3292` -> `SwitchBCM56860HW_acl.cpp:1626-1795` | (a) HW | `hwbcm_acl_stat_get` (`bcm_field_stat_get`), rebase offset on `set`. |
| `POLICER` stats | `PACKETS`, `BYTES`, `GREEN/YELLOW/RED_PACKETS/BYTES` | `p4orch/acl_util.h:687-704`; `FlexCounter.cpp:2865-2868` | Falls through `SwitchBCM56860HW::getStatsExt:1248` -> `SwitchState::getStatsExt` (vslib 0s) | (b) silent no-op (always 0) | Policer created in `SwitchBCM56860HW_copp.cpp:254`, but `getStatsExt` never queries `bcm_policer_stat_get` / `bcm_field_stat_get` for `SAI_OBJECT_TYPE_POLICER`. |
| `COUNTER` (CoPP TRAP) | `SAI_COUNTER_STAT_PACKETS`, `SAI_COUNTER_STAT_BYTES` | `flex_counter/flow_counter_handler.cpp:12-13`; `flexcounterorch.cpp:252-264`; `FlexCounter.cpp:2815-2822` | `SwitchBCM56860HW.cpp:1236-1240` -> `SwitchBCM56860HW_copp.cpp:771-830` | (a) HW | Reads FP stats via `hwbcm_copp_trap_stats_get` and clears on `READ_AND_CLEAR`. |
| `COUNTER` (ROUTE / SRv6) | `SAI_COUNTER_STAT_PACKETS`, `SAI_COUNTER_STAT_BYTES` | `flexcounterorch.cpp:265-281`; `FlexCounter.cpp:2815-2822,2869-2872` | `hwCoppCounterStats` returns `false` -> falls through to `SwitchState::getStatsExt` (vslib 0s) | (b) silent no-op (always 0) | `SAI_ROUTE_ENTRY_ATTR_COUNTER_ID` not wired to `bcm_l3_route_stat_attach`. |
| `DEBUG_COUNTER` | `create`/`set`/`remove`, `INDEX`, `IN/OUT_DROP_REASON_LIST`, `queryAttrEnumValuesCapability`, `PORT/SWITCH_STAT_*_DROP_REASON_RANGE_BASE` | `debugcounterorch.cpp:233-234`; `drop_counter.cpp:116-138,211-253,305-445`; `FlexCounter.cpp:2758-2766,2792-2800` | `SwitchBCM56860HW` has **NO** handler! `SwitchStateBase.cpp:124-143,299-305,4150` handles DB + returns `SAI_STATUS_NOT_SUPPORTED` for `queryAttrEnumValuesCapability` | (c) cap query / (b) silent no-op if created | `debugcounterorch` queries `queryAttrEnumValuesCapability` -> gets `NOT_SUPPORTED` -> advertises 0 supported drop reasons (`[]`) so SONiC rejects user-configured drop counters. If created directly via SAI, `getStatsExt` returns 0s! |
| `FlexCounter` / Bulk Stats | `bulkGetStats`, `bulkClearStats` | `syncd/FlexCounter.cpp:975,1288,1467` | `Sai.cpp:574-607`; `VirtualSwitchSaiInterface.cpp:1160-1189` | (c) `SAI_STATUS_NOT_IMPLEMENTED` | FlexCounter gracefully falls back to single `getStatsExt` / `getStats`+`clearStats` per object (`FlexCounter.cpp:943-969,1039-1057`). |
| `clearStats` | All object types | `syncd/FlexCounter.cpp:1219`; `bufferorch.cpp:316` | `VirtualSwitchSaiInterface.cpp:1135-1158` -> delegates to `getStatsExt(..., READ_AND_CLEAR)` | (a) HW (for PORT/QUEUE/PG/POOL/TRAP) | Fixed stack buffer `uint64_t counters[128]` (`VS_MAX_COUNTERS = 128`). |

*(Part 2 with Detailed Gap Analysis and Robustness Bugs follows immediately.)*
