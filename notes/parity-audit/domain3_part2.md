# Domain 3 SAI Parity Audit — Part 2 of 2: Gap Analysis & Robustness Bugs

## Deliverable 2: Detailed Gap Analysis for EVERY `(b)` Silent No-Op & `(c)` Error Item

Testbench constraint used below: **1 switch + two 1G copper links to 1 PC (one is mgmt port in netns), no 40G, no 2nd switch** (so we have 1 front-panel 1G data link to the PC, or 2 if mgmt is temporarily swapped / loopback cable or 1G PC link + mgmt; note with 1 front-panel data port to PC, ingress/egress traffic between PC and switch CPU/SVI/loopback or self-loop can be tested, while 2-port L2 transit/SPAN/port-to-port requires either the 2nd copper link on a front port or a physical loopback cable; we note this concretely per item).

### 2A. ACL Gaps

1. **`SAI_ACL_TABLE_ATTR_ACL_ACTION_TYPE_LIST` not validated (`(b)`)**
   - **TD2+ SDK API:** None extra needed — validate against `hwbcm_acl_stage_acts(hwStage)` (`hwbcm_acl.c:150`) and return `SAI_STATUS_NOT_SUPPORTED` if unsupported actions requested.
   - **SONiC consequence:** Tables requesting unsupported actions (e.g., `NO_NAT`, `MARK_META`, EGRESS `REDIRECT`) succeed at create time, then rules silently drop those actions.
   - **Effort:** S.
   - **1-switch testable:** Yes (create table via `config acl` / Redis / SAI and check return status).

2. **ACL CRM Counters: `SAI_ACL_TABLE_ATTR_AVAILABLE_ACL_ENTRY` / `AVAILABLE_ACL_COUNTER`, `SAI_SWITCH_ATTR_AVAILABLE_ACL_TABLE` / `AVAILABLE_ACL_TABLE_GROUP` (`(b)`)**
   - **TD2+ SDK API:** `bcm_field_group_status_get(unit, group, &st)` (`st.entries_free`, `st.counters_free` — already wrapped in `hwbcm_acl_group_status()` at `hwbcm_acl.c:294`!) and `bcm_field_status_get(unit, &status)` (`include/bcm/field.h`).
   - **SONiC consequence:** `crm show resources acl` / `crmorch` shows hardcoded fake vslib counts (`3` tables, static entry counts) instead of real TD2+ TCAM/counter occupancy; CRM threshold alarms never fire before TCAM exhaustion.
   - **Effort:** S.
   - **1-switch testable:** Yes (`crm show resources acl` before/after adding ACL tables & rules).

3. **`SAI_ACL_TABLE_GROUP_MEMBER_ATTR_PRIORITY` & `SAI_ACL_TABLE_GROUP_ATTR_TYPE` (`SEQUENTIAL`) (`(b)`)**
   - **TD2+ SDK API:** `bcm_field_group_priority_set(unit, group, priority)` (or pass mapped priority at `bcm_field_group_config_create` with unique tie-breaker); TD2+ hardware only supports `PARALLEL` lookup across slices (return `SAI_STATUS_NOT_SUPPORTED` for `SEQUENTIAL`).
   - **SONiC consequence:** Multi-table action conflicts resolve by table creation order (`10..40`) instead of SAI member priority (`portsorch.cpp:2702` currently passes constant `100`, so mostly affects custom/p4orch multi-table configs).
   - **Effort:** S.
   - **1-switch testable:** Yes (two ingress tables on 1 front port matching same PC packet with conflicting actions, e.g. `SET_DSCP` or `DROP` vs `COPY`).

4. **RIF ACL Binding on Port-based RIF (`SAI_ROUTER_INTERFACE_ATTR_INGRESS/EGRESS_ACL`) (`(b)` for Port RIF)**
   - **TD2+ SDK API:** `bcm_field_qualify_InPorts` (using the port RIF's physical/trunk port) for ingress and `bcm_field_qualify_OutPort` for egress, or `bcm_field_qualify_L3Ingress` (`bcm_l3_ingress_t` / `bcmFieldQualifyInterfaceClassL3`, verified in `trident2plus/field.c:5470,5863`).
   - **SONiC consequence:** Binding an ACL to a routed port RIF logs a warning (`SwitchBCM56860HW_acl.cpp:488`) and installs **0 HW entries** — traffic on that L3 interface bypasses the ACL completely.
   - **Effort:** S.
   - **1-switch testable:** Yes (create L3 port RIF on data port to PC, bind ACL, send ping/UDP from PC to switch IP).

5. **`SAI_ACL_ENTRY_ATTR_FIELD_ACL_IP_TYPE` unknown enum values (`(b)` silent match widening)**
   - **TD2+ SDK API:** Additional TD2+ `bcm_field_IpType_t` values (`bcmFieldIpTypeIpv4NoOpts`, `bcmFieldIpTypeIpv4WithOpts`, `bcmFieldIpTypeIpv6OneExtHdr`, `bcmFieldIpTypeIpv6TwoExtHdr`, `bcmFieldIpTypeMplsUnicast`, `bcmFieldIpTypeMplsMulticast` in `include/bcm/field.h`); return `SAI_STATUS_NOT_SUPPORTED` for any unmapped enum instead of `HWBCM_ACL_IPT_ANY`.
   - **SONiC consequence:** Any rule using an unmapped `IP_TYPE` silently drops the qualifier and matches **all** traffic (security hole!).
   - **Effort:** S.
   - **1-switch testable:** Yes (install entry with unmapped IP type, verify rejected or matched accurately against PC packets).

6. **ACL Match Fields — VXLAN / Inner / Tunnel (`TUNNEL_VNI`, `TUNNEL_TERMINATED`, `INNER_ETHER_TYPE`, `INNER_IP_PROTOCOL`, `INNER_SRC_IP`, `INNER_DST_IP`, `INNER_SRC_IPV6`, `INNER_DST_IPV6`, `INNER_L4_SRC_PORT`, `INNER_L4_DST_PORT`, `INNER_SRC_MAC`, `INNER_DST_MAC`) (`(c)`)**
   - **TD2+ SDK API:** Verified in `trident2plus/field.c:5428-5600`: TD2+ IFP supports `bcmFieldQualifyInnerSrcIp`, `bcmFieldQualifyInnerDstIp`, `bcmFieldQualifyInnerSrcIp6`, `bcmFieldQualifyInnerIpProtocol`, `bcmFieldQualifyInnerL4SrcPort`, `bcmFieldQualifyInnerL4DstPort`, `bcmFieldQualifyInnerTos`, `bcmFieldQualifyInnerTtl`, `bcmFieldQualifyInnerIpFrag`, `bcmFieldQualifyVpn` (`trident2plus/field.c:244,1094` — maps VNI->VFI VPN), `bcmFieldQualifyTunnelTerminated` (check: `trident2plus/field.c` does NOT list `TunnelTerminated` directly, uses `bcmFieldQualifyTunnelType` / `bcmFieldQualifyVpn` / `bcmFieldQualifyVxlanHeaderBits`). Inner MAC / Inner EtherType / Inner DstIp6 are **not** standard TD2+ IFP qualifiers (require UDF or unsupported on TD2+).
   - **SONiC consequence:** VXLAN inner-header ACLs and DTEL tables fail at `create_acl_table` with `SAI_STATUS_NOT_SUPPORTED`.
   - **Effort:** M–L (requires intra-slice double-wide key handling + VNI-to-VPN lookup).
   - **1-switch testable:** Yes (send VXLAN-encapsulated UDP/4789 packets from PC on the 1G data link to switch VTEP IP, match inner IP/L4, verify ACL counter/drop).

7. **ACL Match Fields — Metadata & Misc (`ACL_USER_META`, `ROUTE_DST_USER_META`, `OUTER_VLAN_PRI`, `OUTER_VLAN_CFI`, `INNER_VLAN_*`, `PACKET_VLAN`, `IPV6_FLOW_LABEL`, `BTH_OPCODE`, `AETH_SYNDROME`, `UDF`, `TCP_FLAGS` ECE/CWR, `MACSEC_SCI`) (`(c)`)**
   - **TD2+ SDK API:**
     - `ACL_USER_META` + `SET_ACL_META_DATA`: `bcmFieldActionClassDestSet` / `bcmFieldActionClassSourceSet` (`trx/field.c:7333,10697,11980`) in IFP/VFP -> `bcmFieldQualifyDstClassField` / `bcmFieldQualifySrcClassField` in EFP (`trident2plus/field.c:5838,5843`)! (Needed by SONiC `MARK_META` + `EGR_SET_DSCP`.)
     - `OUTER_VLAN_PRI`/`CFI`, `INNER_VLAN_*`, `PACKET_VLAN`, `ROUTE_DST_USER_META`: `bcmFieldQualifyOuterVlanPri`, `bcmFieldQualifyOuterVlanCfi`, `bcmFieldQualifyInnerVlanId/Pri/Cfi`, `bcmFieldQualifyVlanFormat`, `bcmFieldQualifyDstClassL3` (all verified in `trident2plus/field.c:193,847-1022`).
     - `BTH_OPCODE` / `AETH_SYNDROME` (RoCEv2): `bcm_udf_create()` + `bcm_udf_pkt_format_create()` + `bcm_field_qualify_udf()` (`include/bcm/udf.h:257`).
     - `TCP_FLAGS` bits 6..7 (ECE/CWR) & `MACSEC_SCI`: **Not supported in TD2+ hardware** (`TcpControl` is 6 bits; no MACsec block).
   - **SONiC consequence:** `MARK_META` / `EGR_SET_DSCP` tables fail at `create_acl_table` (despite `queryAttributeCapability` lying that they are supported); RoCEv2/P4Orch tables fail.
   - **Effort:** S (for VLAN pri/cfi/format & `ACL_USER_META`/`SET_ACL_META_DATA`), M (for UDF/RoCEv2).
   - **1-switch testable:** Yes for VLAN pri/cfi, `ACL_USER_META` (requires 1 ingress + 1 egress packet, testable via routed/loopback or PC->switch->CPU/PC), UDF (send raw UDP/4791 RoCE packet from PC and check ACL counter).

8. **ACL Entry Actions — `SET_POLICER` (`(b)`)**
   - **TD2+ SDK API:** `bcm_field_entry_policer_attach(unit, entry, level, policer_id)` / `bcm_field_entry_policer_detach()` (`include/bcm/field.h:11693,11710`) + `bcmFieldActionGpDrop`/`YpDrop`/`RpDrop` (same mechanism already proven in `hwbcm_copp.c:925`!). Note TD2+ FP policers are slice-local; each table/slice needs a policer in its pool.
   - **SONiC consequence:** Any ACL rule with a policer (P4Orch / custom ACL) installs without rate limiting!
   - **Effort:** M.
   - **1-switch testable:** Yes (send UDP burst from PC into front port matching ACL rule with policer, verify rate limit via ACL/port counters or CPU rx rate).

9. **ACL Entry Actions — `SET_DO_NOT_LEARN`, `SET_ACL_META_DATA`, `NO_NAT`, `SET_SRC_MAC`, `SET_DST_MAC`, `SET_INNER_SRC_MAC`, `SET_SRC_IP`, `SET_DST_IP`, `SET_USER_TRAP_ID`, `FLOOD`, `PACKET_TRIM_DISABLE`, `DTEL_*`, `MACSEC_FLOW` (`(b)`)**
   - **TD2+ SDK API:**
     - `SET_DO_NOT_LEARN`: `bcmFieldActionDoNotLearn` (`trx/field.c:8005,10694,11979`, IFP & VFP).
     - `SET_ACL_META_DATA`: `bcmFieldActionClassDestSet` / `bcmFieldActionClassSourceSet` (`trx/field.c:7333,10697,11980`).
     - `NO_NAT`: `bcmFieldActionNatCancel` (`trx/field.c:11225`, `soc_feature_nat` is TRUE on BCM56860 at `feature.c:6652`, though NAT itself is not implemented in this SAI).
     - `SET_SRC_MAC` / `SET_DST_MAC`: `bcmFieldActionSrcMacNew` / `bcmFieldActionDstMacNew` (`trx/field.c:8072-8073,11113,11139` — note in ESW takes L3 next-hop/egress rewrite or MAC index; verify EFP/IFP applicability).
     - `SET_USER_TRAP_ID`: `bcmFieldActionCopyToCpu` + `bcmFieldActionCosQCpuNew` + `bcmFieldActionMatchPbmp`/KNET `BCM_KNET_FILTER_M_FP_RULE` (maps rule ID to genetlink/FD).
     - `SET_SRC_IP` / `SET_DST_IP` / `SET_INNER_SRC_MAC` / `PACKET_TRIM_DISABLE` / `DTEL_*` / `MACSEC_FLOW`: **Not supported in TD2+ hardware** (should return `SAI_STATUS_NOT_SUPPORTED` instead of silent no-op!).
     - `FLOOD`: No direct single `bcmFieldActionFlood` on TD2+ (only `bcmFieldActionDropCancel` or redirect to L2MC group via `bcmFieldActionRedirectMcast`).
   - **SONiC consequence:** Rules specifying these actions silently succeed with **no action executed** (dangerous for `NO_NAT`, `SET_DO_NOT_LEARN`, `SET_ACL_META_DATA`). At minimum, unsupported actions MUST return `SAI_STATUS_NOT_SUPPORTED`.
   - **Effort:** S (to reject unsupported + add `DoNotLearn` & `ClassDestSet`), M (for `USER_TRAP_ID` / MAC rewrite).
   - **1-switch testable:** Yes (`DoNotLearn`: send unknown SMAC from PC, verify FDB table doesn't learn it; `SET_ACL_META_DATA`: match in EFP or verify counter; rejection tests need no traffic).

10. **ACL Range Types — `OUTER_VLAN`, `PACKET_LENGTH` (`(c)`)**
    - **TD2+ SDK API:** `bcm_field_range_create(unit, &range, BCM_FIELD_RANGE_OUTER_VLAN | BCM_FIELD_RANGE_PACKET_LENGTH, min, max)` (verified supported in `trx/field.c:15144-15156` & `field_common.c:26543-26555`). `INNER_VLAN` / `IN_PORT` / `DST_MAC` ranges are **not** supported by TD2+ `FP_RANGE_CHECK`.
    - **SONiC consequence:** Not called by SONiC 202505 `aclorch.cpp` (only L4 src/dst port ranges are used), affects custom SAI callers only.
    - **Effort:** S.
    - **1-switch testable:** Yes (send tagged VLAN or sized packet from PC, check ACL counter).

---

### 2B. Mirror Gaps

11. **`SAI_MIRROR_SESSION_ATTR_POLICER` (`(b)`)**
    - **TD2+ SDK API:** **No per-mirror-session hardware policer on TD2+** (`bcm_mirror_destination_t` has no policer field on ESW; rate-limiting mirrored traffic on TD2+ requires egress shaper on monitor port via `bcm_port_rate_egress_set` or FP policer on ACL mirror rule). Should return `SAI_STATUS_NOT_SUPPORTED` when non-null or document limitation.
    - **SONiC consequence:** `mirrororch.cpp:1048` attaches a policer when configured on a mirror session; silently ignored, so mirrored traffic is unpoliced and can saturate the monitor port / collector.
    - **Effort:** S (to reject cleanly) or M (to attach FP policer for ACL-mirror rules).
    - **1-switch testable:** Yes (verify create error or rate behavior).

12. **`SAI_MIRROR_SESSION_ATTR_MONITOR_PORTLIST` with `count > 1` (`(c)`)**
    - **TD2+ SDK API:** `BCM_MIRROR_DEST_ID_SHARE` + `BCM_MIRROR_DEST_MTP_ADD` / `BCM_MIRROR_DEST_MTP_DELETE` + `rtag` (`bcm_mirror_destination_create`, verified explicitly supported on `SOC_IS_TD2_TT2` in `src/bcm/esw/mirror.c:27777-27809`!).
    - **SONiC consequence:** Not called by SONiC 202505 `mirrororch.cpp` (uses single `MONITOR_PORT` or LAG trunk), affects custom SAI callers.
    - **Effort:** M.
    - **1-switch testable:** Partially (create/verify HW MTP entries; verifying load balancing across 2 monitor ports requires 2 active data links).

13. **`SAI_MIRROR_SESSION_TYPE_SFLOW` (`(c)`), `SAI_MIRROR_SESSION_ATTR_TRUNCATE_SIZE` (`(b)`), `SAMPLE_RATE` (`(b)`), `CONGESTION_MODE` (`(b)`), `COUNTER_ID` (`(b)`), `ERSPAN_SESSION_ID` (`(b)`), `GRE_HEADER_FIRST_16BIT` (`(b)`), IPv6 ERSPAN (`(c)`)**
    - **TD2+ SDK API:** Verified against `src/bcm/esw/mirror.c`:
      - `TRUNCATE_SIZE`: **Not supported on TD2+** (`TRUNCATE_MIRROR_COPYf` is TH3/TD3-only, `mirror.c:15965-15985`).
      - `SFLOW` mirror session / `BCM_MIRROR_PORT_SFLOW` / `sample_rate_dividend`: **Not supported on TD2+** (`soc_feature_sflow_ing_mirror` is NOT in `soc_features_bcm56860_a0`, `mirror.c:8084,12916` returns `BCM_E_UNAVAIL`; TD2+ sFlow samples to CPU via `bcm_port_sample_rate_set`, NOT to mirror MTP!).
      - IPv6 ERSPAN (`version=6`): **Not supported on TD2+** (`mirror.c:14876` returns `BCM_E_UNAVAIL`).
      - `ERSPAN_SESSION_ID` (`BCM_MIRROR_DEST_TUNNEL_WITH_SPAN_ID`): **Not supported on ESW** (`mirror.c:27696` returns `BCM_E_PARAM`).
      - `CONGESTION_MODE`, `COUNTER_ID`, `GRE_HEADER_FIRST_16BIT`: **Not supported on TD2+**.
    - **SONiC consequence:** IPv6 ERSPAN properly fails with `NOT_SUPPORTED`. The `(b)` attributes are not passed by SONiC 202505 `mirrororch.cpp` (except `POLICER`), though returning `NOT_SUPPORTED` is cleaner than silent ignore.
    - **Effort:** S (just return `NOT_SUPPORTED` if non-default).
    - **1-switch testable:** Yes (config rejection check).

14. **`sai_object_type_get_availability(SAI_OBJECT_TYPE_MIRROR_SESSION)` (`(c)`)**
    - **TD2+ SDK API:** Track active MTP usage against `BCM_MIRROR_MTP_COUNT` (`4` on TD2+, `hwbcm_mirror.h:44`).
    - **SONiC consequence:** `MirrorOrch::isHwResourcesAvailable()` (`mirrororch.cpp:360`) skips pre-check; 5th active mirror fails late on `set_port_attribute` instead of failing cleanly up front.
    - **Effort:** S.
    - **1-switch testable:** Yes (create 4+ sessions and query availability).

---

### 2C. CoPP / Hostif / Policers / sFlow / DTEL / MACsec Gaps

15. **`SAI_HOSTIF_ATTR_OPER_STATUS` (`(b)` — HIGH IMPACT!)**
    - **TD2+ SDK API / KNET:** Write `"<ifname>=up"` / `"<ifname>=down"` to `/proc/bcm/knet/link` (verified in `sdk-6.5.27-wb/systems/linux/kernel/modules/bcm-knet/bcm-knet.c:7070-7122`, which calls `netif_carrier_on(dev)` / `netif_carrier_off(dev)`!) on both `create_hostif` (initial `OPER_STATUS`, usually `false`) and `set_hostif_attribute(SAI_HOSTIF_ATTR_OPER_STATUS)`.
    - **SONiC consequence:** Every KNET front-panel netdev (`Ethernet0..Ethernet52`) permanently shows `LOWER_UP` / carrier UP in Linux (`ip link show`) even with no cable plugged in or admin down! Breaks Linux/FRR/zebra link-down route withdrawal, LACP/teamd carrier detection, and LLDPd port state tracking.
    - **Effort:** S (trivial `fopen("/proc/bcm/knet/link", "w")` + `fprintf`).
    - **1-switch testable:** Yes! Unplug or `config interface shutdown EthernetX` on the 1G link to PC, check `ip link show EthernetX` for `NO-CARRIER` vs `LOWER_UP`.

16. **`SAI_HOSTIF_ATTR_VLAN_TAG` (`STRIP` / `KEEP` / `ORIGINAL`) (`(b)`)**
    - **TD2+ SDK API:** `bcm_knet_filter_destroy()` + `bcm_knet_filter_create()` with/without `BCM_KNET_FILTER_F_STRIP_TAG` (`include/bcm/knet.h:72`) and `BCM_KNET_NETIF_F_KEEP_RX_TAG` (`include/bcm/knet.h:34`).
    - **SONiC consequence:** `PortsOrch::setHostIntfsStripTag()` (`portsorch.cpp:2842,6549,6590,7467,7515`) toggles `KEEP`/`STRIP` when ports join/leave VLANs or LAGs; currently ignored in favor of hardcoded PVID-strip + other-keep filters. Works for standard access/trunk VLANs (because PVID filter strips access VLAN and prio-50 keeps trunk tags), **fails** if a port explicitly needs `STRIP` on a non-PVID VLAN or `KEEP` on its PVID (e.g. `VLAN_TAG_KEEP` on bridge/LAG member where Linux bridge expects tagged PVID frames).
    - **Effort:** S.
    - **1-switch testable:** Yes (send 802.1Q tagged frame matching PVID from PC, `tcpdump -e -i EthernetX` to observe whether VLAN tag is kept or stripped).

17. **`SAI_HOSTIF_ATTR_OBJ_ID = CPU_PORT / VLAN / LAG` and `SAI_HOSTIF_ATTR_QUEUE` (`(b)`)**
    - **TD2+ SDK API:**
      - CPU netif: `bcm_knet_netif_create()` with `BCM_KNET_NETIF_T_TX_CPU_INGRESS` (already written as `hwbcm_knet_cpu_netif_create()` in `hwbcm_core.c:3370`, just never called!).
      - VLAN netif: `bcm_knet_netif_create()` with `type=BCM_KNET_NETIF_T_TX_CPU_INGRESS`, `vlan=vid`, `flags=BCM_KNET_NETIF_F_ADD_TAG` + RX filter `BCM_KNET_FILTER_M_VLAN`.
      - TX queue: `netif.cosq = (bcm_cos_queue_t)queue` (`include/bcm/knet.h:52`).
    - **SONiC consequence:** `PortsOrch::createCpuHostIntf()` (`portsorch.cpp:6415`) and `createVlanHostIntf()` (`portsorch.cpp:3590`, when `vlan_interface` hostif is enabled) silently create no Linux netdev; packets with no matching port filter are dropped instead of delivered to CPU netdev; `SAI_HOSTIF_ATTR_QUEUE` TX CoS queue is always 0.
    - **Effort:** S (CPU netif + `cosq`), M (VLAN/LAG netifs).
    - **1-switch testable:** Yes (verify CPU/VLAN netdev exists via `ip link` and can TX/RX packets to/from PC).

18. **sFlow end-to-end: `SAI_OBJECT_TYPE_SAMPLEPACKET` + `SAI_PORT_ATTR_INGRESS/EGRESS_SAMPLEPACKET_ENABLE` + `SAI_HOSTIF_TRAP_TYPE_SAMPLEPACKET` + `SAI_HOSTIF_TYPE_GENETLINK` + `SAI_HOSTIF_TABLE_ENTRY` (`TRAP_ID -> GENETLINK`) (`(b)` — HIGH IMPACT!)**
    - **TD2+ SDK API:**
      - Per-port ingress/egress sampling: `bcm_port_sample_rate_set(unit, port, ing_rate, egr_rate)` (`include/bcm/port.h:1983`, verified supported on XGS3/TD2+ in `src/bcm/esw/port.c:31150`) and `bcm_port_control_set(unit, port, bcmPortControlSampleIngressEnable / bcmPortControlSampleEgressEnable / bcmPortControlSampleIngressRate / bcmPortControlSampleEgressRate)` (`port.h:3556-3561`) + `bcm_switch_control_set(unit, bcmSwitchSampleIngressRandomSeed, seed)`.
      - Sample packet delivery to Linux `psample` genetlink (`hsflowd`): KNET filter with `BCM_KNET_FILTER_M_REASON` matching `bcmRxReasonSampleSource` / `bcmRxReasonSampleDest` sending to `BCM_KNET_DEST_T_BCM_RX_API` -> `bcm_rx_register()` callback -> userspace libnl/netlink `psample` multicast group (or kernel `bcm-knet` `psample` RX callback, which is how vendor Broadcom SAI delivers sFlow to `hsflowd`).
      - CoPP trap policing for `SAMPLEPACKET`: `bcm_rx_cosq_mapping_set()` for `bcmRxReasonSampleSource`/`bcmRxReasonSampleDest` or IFP/CPU-queue shaper (`bcm_cosq_port_bandwidth_set` on CPU queue).
    - **SONiC consequence:** `config sflow enable` and `sfloworch` appear to succeed with zero errors, **but zero packets are ever sampled or delivered to `hsflowd` / sFlow collector**!
    - **Effort:** L (sampling rate programming is S, but `bcm_rx_register` + Linux `psample` genetlink bridge + `HOSTIF_TABLE_ENTRY` dispatch is L).
    - **1-switch testable:** Yes! Send traffic from PC on 1G link, enable sFlow (`sample_rate=10`), verify sampled packets arrive on `psample` genetlink / `hsflowd` / collector on PC.

19. **`SAI_HOSTIF_TABLE_ENTRY` (`WILDCARD`, `PORT`, `LAG`, `VLAN`, `TRAP_ID`; channels `NETDEV_PHYSICAL_PORT`, `NETDEV_LOGICAL_PORT`, `NETDEV_L3`, `GENETLINK`, `FD`) & `SAI_HOSTIF_TYPE_FD` & `SAI_OBJECT_TYPE_HOSTIF_USER_DEFINED_TRAP` (`(b)`)**
    - **TD2+ SDK API:** `bcm_knet_filter_create()` with `BCM_KNET_FILTER_M_INGPORT` / `M_VLAN` / `M_REASON` / `M_FP_RULE` routing to `BCM_KNET_DEST_T_NETIF` or `BCM_KNET_DEST_T_BCM_RX_API` (`bcm_rx_register` -> FD / genetlink socket). For `HOSTIF_USER_DEFINED_TRAP`, assign a unique FP rule match / CPU CoS (`bcmFieldActionCosQCpuNew` / `bcmFieldActionCopyToCpu`) + KNET filter `BCM_KNET_FILTER_M_FP_RULE` / `M_CPU_COS`.
    - **SONiC consequence:** Default `WILDCARD -> NETDEV_PHYSICAL_PORT` works by coincidence via hardcoded KNET port filters, but any `TRAP_ID -> GENETLINK` (sFlow, TAM, P4Orch), `FD`, or L3/VLAN channel mapping silently does nothing.
    - **Effort:** M–L.
    - **1-switch testable:** Yes (trap a packet from PC and verify receipt on target netdev/FD/genetlink).

20. **Port Storm Control: `SAI_PORT_ATTR_BROADCAST_STORM_CONTROL_POLICER_ID`, `FLOOD_STORM_CONTROL_POLICER_ID`, `MULTICAST_STORM_CONTROL_POLICER_ID` (`(b)` — HIGH IMPACT!)**
    - **TD2+ SDK API:** `bcm_rate_bandwidth_set(unit, port, flags, kbits_sec, kbits_burst)` (`include/bcm/rate.h:182`, verified on `SOC_IS_TRX`/TD2+ in `src/bcm/esw/rate.c:2211`) with `flags = BCM_RATE_BCAST` / `BCM_RATE_DLF` / `BCM_RATE_MCAST` (and `BCM_RATE_MODE_PACKETS` for packet mode, or byte mode by default matching `policerorch.cpp:158` which hardcodes `BYTES`!), or legacy `bcm_rate_bcast_set` / `bcm_rate_dlfbc_set` / `bcm_rate_mcast_set` (`rate.h:104-144`). Pass `BCM_RATE_DISABLE` on unbind (`SAI_NULL_OBJECT_ID`).
    - **SONiC consequence:** `config interface storm-control` (`PolicerOrch::handlePortStormControlTable`, `policerorch.cpp:148-371`) succeeds with zero errors, **but broadcast/unknown-unicast/multicast floods are NOT rate-limited at all in hardware**!
    - **Effort:** S.
    - **1-switch testable:** Yes! Blast broadcast/multicast/unknown-unicast frames from PC into the 1G front port at high rate, verify port RX/drop counters or rate arriving at CPU/loopback is capped to configured kbps.

21. **`SAI_HOSTIF_TRAP_TYPE_L3_MTU_ERROR` miswired (`(b)`) & `NEIGHBOR_MISS` unpoliced (`(b)`)**
    - **TD2+ SDK API:**
      - `L3_MTU_ERROR`: `bcm_switch_control_set(unit, bcmSwitchL3MtuFailToCpu, to_cpu)` (verified supported on `TRX`/TD2+ in `src/bcm/esw/switch.c:13859`!) + `bcm_rx_cosq_mapping_set(unit, idx, reasons={bcmRxReasonL3MtuFail}, ..., int_prio, cosq)` (`include/bcm/rx.h:705`) for CPU queue mapping.
      - `NEIGHBOR_MISS`: `bcm_rx_cosq_mapping_set()` for `bcmRxReasonL3DestMiss` / `bcmRxReasonL3Slowpath` + CPU queue shaper or IFP `bcmFieldQualifyL3Routable` + `DstL3Egress` miss.
    - **SONiC consequence:** Oversized routed packets exceeding egress RIF MTU are **never trapped to CPU** (so Linux never sends ICMP Fragmentation Needed / Packet Too Big -> Path MTU Discovery is broken!). Also `L3_MTU_ERROR` and `NEIGHBOR_MISS` cannot be rate-limited by CoPP policers.
    - **Effort:** S (for `bcmSwitchL3MtuFailToCpu` + `bcm_rx_cosq_mapping_set`).
    - **1-switch testable:** Yes for `L3_MTU_ERROR`? Partially (needs ingress->egress L3 forwarding to hit egress RIF MTU check; testable with 2 front ports or loopback on a 2nd port; `NEIGHBOR_MISS` is directly testable with 1 data port: PC sends IP packet to an unresolved neighbor subnet routed out an SVI/port).

22. **Unhandled `HOSTIF_TRAP_TYPE_*`: `ISIS`, `IPV6_MLD_V1_V2`, `IPV6_MLD_V1_REPORT`, `IPV6_MLD_V1_DONE`, `MLD_V2_REPORT`, `BFD_MICRO`, `BFDV6_MICRO`, `SNAT_MISS`, `DNAT_MISS`, `SWITCH_CUSTOM_RANGE_BASE`, `ROUTER_CUSTOM_RANGE_BASE` (`(b)`)**
    - **TD2+ SDK API:**
      - `MLD_*`: `bcm_switch_control_set(unit, bcmSwitchMldPktToCpu / bcmSwitchMldQueryToCpu / bcmSwitchMldReportDoneToCpu / bcmSwitchMldPktDrop, val)` (`include/bcm/switch.h:138-165`, verified in `esw/switch.c:5676-5776`) + CoPP IFP ICMPv6 types `130, 131, 132, 143` (`bcmFieldQualifyIcmpTypeCode`).
      - `ISIS`: L2 cache entries (`bcm_l2_cache_set`) + CoPP IFP matching DMAC `01:80:c2:00:00:14`, `01:80:c2:00:00:15`, `09:00:2b:00:00:05`.
      - `BFD_MICRO` / `BFDV6_MICRO`: CoPP IFP UDP dst port `6784` + `to_me` (`DstL3Egress`).
      - `SNAT_MISS` / `DNAT_MISS`: `bcmSwitchNatMissToCpu` (only relevant if NAT is enabled; `gIsNatSupported` gates it in `copporch.cpp:401`).
      - Also implement `sai_query_attribute_enum_values_capability(SAI_OBJECT_TYPE_HOSTIF_TRAP, SAI_HOSTIF_TRAP_ATTR_TRAP_TYPE)` so `CoppOrch` only installs traps actually supported!
    - **SONiC consequence:** IS-IS, IPv6 MLD snooping, and micro-BFD packets are neither trapped to CPU nor CoPP-policed, while STATE_DB falsely advertises them as `installed`.
    - **Effort:** S (MLD + ISIS + BFD_MICRO + enum capability query).
    - **1-switch testable:** Yes (send IS-IS L2 frame, ICMPv6 MLD type 130/131/143, or UDP/6784 to switch IP from PC, verify trap + CoPP rate limit).

23. **`SAI_HOSTIF_TRAP_ATTR_EXCLUDE_PORT_LIST`, `SAI_HOSTIF_TRAP_ATTR_MIRROR_SESSION`, `SAI_HOSTIF_TRAP_GROUP_ATTR_ADMIN_STATE` (`(b)`)**
    - **TD2+ SDK API:**
      - `EXCLUDE_PORT_LIST`: In `hwbcm_copp.c`, qualify `bcmFieldQualifyInPorts` with `all_ports & ~exclude_pbmp` (note: switch-control traps like ARP are global, so excluded ports need a higher-prio IFP `CopyToCpuCancel`+`SwitchToCpuCancel` entry).
      - `TRAP_GROUP_ATTR_ADMIN_STATE`: When `false`, skip installing (or remove) FP entries for traps in that group.
      - `TRAP_ATTR_MIRROR_SESSION`: `bcmFieldActionMirrorIngress` on the CoPP FP entry.
    - **SONiC consequence:** Low in default SONiC (`copporch.cpp` does not set these), affects custom SAI/admin-disable workflows.
    - **Effort:** S.
    - **1-switch testable:** Yes.

24. **`SAI_OBJECT_TYPE_POLICER` stats (`get_policer_stats`, `clear_policer_stats`) & `ENABLE_COUNTER_PACKET_ACTION_LIST` (`(b)`)**
    - **TD2+ SDK API:** TD2+ ESW has **no** standalone `bcm_esw_policer_stat_get` (`src/bcm/esw/policer.c` does not implement it). On TD2+, vendor Broadcom SAI aggregates `bcm_field_stat_get(unit, stat_id, bcmFieldStatGreenPackets/YellowPackets/RedPackets/GreenBytes/YellowBytes/RedBytes)` across all FP entries attached to that policer (note `hwbcm_copp.c:930-935` ALREADY attaches `{Red,NotRed} x {Packets,Bytes}` stats to every CoPP FP entry, so `RED_PACKETS`/`RED_BYTES`/`GREEN_PACKETS`/`GREEN_BYTES` are already in hardware and just need wiring into `getStatsExt(SAI_OBJECT_TYPE_POLICER)`!).
    - **SONiC consequence:** `FlexCounter` / `show policer` / P4Orch meter stats always read `0` even while CoPP is actively dropping packets!
    - **Effort:** S (for CoPP policers using existing `hwbcm_copp_trap_stats_get` — just also read `bcmFieldStatRedBytes`!).
    - **1-switch testable:** Yes (flood ARP/IP2ME from PC above CoPP CIR, read policer stats via `get_policer_stats` / Redis COUNTERS_DB).

25. **`DTEL` (`dtelorch.cpp`: 11 APIs, 29 attrs) & `MACSEC` (`macsecorch.cpp`: 15 `sai_macsec_api` + MACsec ACLs) (`(b)` / `(c)`)**
    - **TD2+ SDK API:** **Not supported in TD2+ (BCM56864) hardware.** Verified in `sdk-6.5.27-wb/src/soc/common/feature.c`: `soc_feature_xflow_macsec` is NOT set on BCM56860, and TD2+ has no INT/DTEL/IFA hardware block (introduced in TD3/TH3).
    - **SONiC consequence:** Currently `sai_dtel_api` and `sai_macsec_api` calls fall through to vslib's `default: return SAI_STATUS_SUCCESS` (pretending DTEL/MACsec exist in HW, until MACsec hits `ACL_STAGE_*_MACSEC` which errors). They should explicitly return `SAI_STATUS_NOT_SUPPORTED` (and `queryAttributeCapability` should return `false` for `MACSEC_SCI`, DTEL, etc.).
    - **Effort:** S (explicit `NOT_SUPPORTED` cases in `SwitchBCM56860HW::create/set/remove`).
    - **1-switch testable:** Yes (confirm clean `NOT_SUPPORTED` rejection).

---

## Deliverable 3: Robustness Bugs (Exact `file:line` & Severity)

### Critical / High Severity

1. **[HIGH] Hostif trap removal & `PACKET_ACTION=FORWARD` leave HW CPU steering permanently enabled & UNPOLICED**
   - **Location:** `SwitchBCM56860HW_copp.cpp:704-715` (`hwCoppOnTrapRemove`), `SwitchBCM56860HW.cpp:2147-2155` (`hwCreateHostifTrap`)
   - **Detail:** Removing a `HOSTIF_TRAP` or setting `PACKET_ACTION=FORWARD` removes the CoPP FP entry (`coppForgetTrap`), **but never disables the switch control / L2 cache steering** (`hwbcm_trap_set` is skipped for `FORWARD` and never called on remove). Traffic keeps reaching the CPU completely unpoliced.

2. **[HIGH] Shared `HWBCM_TRAP_*` buckets clobber sibling trap actions (e.g. `ARP_RESPONSE=TRAP` breaks L2 ARP flooding; `PVRST` clobbers `STP`; `L3_MTU_ERROR` miswired to `NEIGH_MISS`)**
   - **Location:** `SwitchBCM56860HW.cpp:2061-2111` & `hwbcm_core.c:3642-3675`
   - **Detail:**
     - `ARP_REQUEST`, `ARP_RESPONSE`, and IPv6 ND all map to `HWBCM_TRAP_ARP`, which sets ALL 5 controls (`ArpRequestToCpu/Drop`, `ArpReplyToCpu/Drop`, `NdPktToCpu`). Setting `ARP_RESPONSE=TRAP` sets `ArpRequestDrop=1`, dropping L2-switched ARP requests!
     - `SAI_HOSTIF_TRAP_TYPE_PVRST` maps to `HWBCM_TRAP_STP`, overwriting the `01:80:c2:00:00:00` STP L2 cache entry.
     - `SAI_HOSTIF_TRAP_TYPE_L3_MTU_ERROR` maps to `HWBCM_TRAP_NEIGH_MISS`, never setting `bcmSwitchL3MtuFailToCpu`.

3. **[HIGH] Unmapped `SAI_ACL_ENTRY_ATTR_FIELD_ACL_IP_TYPE` silently widens ACL rule to match ALL packets**
   - **Location:** `SwitchBCM56860HW_acl.cpp:171-188, 741-751`
   - **Detail:** `ipTypeOf()` returns `HWBCM_ACL_IPT_ANY` for unknown/unsupported IP types as well as `SAI_ACL_IP_TYPE_ANY`, and line `:745` skips setting `HWBCM_ACL_Q_IP_TYPE` whenever `t == HWBCM_ACL_IPT_ANY`. Any rule using e.g. `SAI_ACL_IP_TYPE_IPV4_HEADER` or `MPLS` silently becomes a match-all rule (security/forwarding breach).

4. **[HIGH] Partial HW programming + DB/HW desync on `aclProgramEntry` / `set()` failure**
   - **Location:** `SwitchBCM56860HW.cpp:3135,3191-3200`; `SwitchBCM56860HW_acl.cpp:1171-1214,1258,1947-1951,2056-2072`
   - **Detail:**
     - `SwitchBCM56850::set()` mutates vslib DB *before* HW programming and never rolls back on HW failure (`SwitchBCM56860HW.cpp:3193`).
     - In `aclProgramEntry` (same-counter path `:1171-1214`), if `hwbcm_acl_entry_update` fails on variant `i`, `hwbcm_acl.c:710` removes it from HW, `aclRetire` deletes it, and if recreate fails (e.g. TCAM full), variant `i` is dropped while variants `0..i-1` keep the new rule and `E.hw.swap(newHw)` commits a **partial subset of ports/VLANs** (ACL enforcement hole!).
     - In `hwCreateAclTableGroupMember:1947-1951`, if `aclRefreshTable()` fails, `SwitchBCM56860HW::create` rolls back vslib store, **but `m_aclGroups[g].members` and `m_aclGroupMembers` are never rolled back** (leaked dangling member OID permanently alters `aclScopeOf`).
     - In `hwAclBind:2056-2072`, `m_aclBindings` is updated before `aclBindChanged()`; on failure, `m_aclBindings` retains the failed binding.

5. **[HIGH] CoPP `OSPF`/`OSPFV6`/`VRRP`/`VRRPV6`/`PIM`/`IGMP`/`DHCPV6` rules match ANY transit IP packet and DROP transit traffic on `TRAP`**
   - **Location:** `hwbcm_copp.c:256-262, 301-324`
   - **Detail:** These rules qualify ONLY on `EtherType` + `IpProtocol` (or UDP 546/547) with `*steer = 1` (so `HWBCM_COPP_ACT_TRAP` adds `bcmFieldActionDrop` at `:749`), without matching multicast DMAC, `TTL<=1`, or `to_me`. Any routed unicast transit OSPF (virtual link), VRRP, PIM, IGMP, or DHCPv6 relay packet crossing the switch is intercepted and **dropped**!

6. **[HIGH] CoPP `g_have_stats = 0` permanently disables ALL CoPP stats globally on ANY transient `entry_install` failure (even TCAM full!)**
   - **Location:** `hwbcm_copp.c:945-959`
   - **Detail:** When `bcm_field_entry_install` fails for *any* reason (e.g., TCAM full, `BCM_E_RESOURCE`), line `:958` sets global `g_have_stats = 0`, permanently disabling CoPP counters for the lifetime of the process even if the failure had nothing to do with stat counters.

7. **[HIGH] NULL pointer dereferences on SAI list attributes in ACL**
   - **Location:** `SwitchBCM56860HW_acl.cpp:634-636, 647-649, 775-777, 890-892, 1385-1387, 2189-2191`
   - **Detail:** All 6 loops iterate `i < count` and dereference `.list[i]` without checking `.list != nullptr` (unlike `SwitchBCM56860HW_mirror.cpp:220,861`, which properly checks `!= NULL`). Crashes syncd on malformed `count > 0, list == NULL`.

8. **[HIGH] All CoPP / Policer HW errors silently swallowed (unprotected CPU)**
   - **Location:** `SwitchBCM56860HW_copp.cpp:415-420, 480-489, 525-547, 656, 701`
   - **Detail:** `hwCreatePolicer`, `hwSetPolicer`, `hwCoppOnTrapCreate/Set` log and return `SAI_STATUS_SUCCESS` even when `bcm_policer_create/set` or `hwbcm_copp_trap_apply` fail (e.g., `COPP_MAX_POLICERS=32` or `COPP_MAX_TRAPS=64` exceeded, or IFP full). DB shows new rates/traps installed while CPU is unpoliced or running stale limits.

---

### Medium Severity

9. **[MEDIUM] KNET VFI filter leaked on `hwbcm_knet_netif_create` error path**
   - **Location:** `hwbcm_core.c:3340-3361`
   - **Detail:** When RIOT is enabled, `knet_vfi_filter_create()` creates `slot->vfi_filter_id` (`:3341`). If the subsequent catch-all `bcm_knet_filter_create` fails (`:3354`), cleanup (`:3357-3359`) destroys `pvid_filter_id` and `netif.id`, **forgetting `knet_filter_destroy_quiet(&slot->vfi_filter_id)`** — leaking an orphan kernel KNET filter.

10. **[MEDIUM] `hwbcm_port_pvid_set` ignores `knet_pvid_filter_create` failure after destroying old filter**
    - **Location:** `hwbcm_core.c:2938-2944`
    - **Detail:** Destroys existing `pvid_filter_id`, then casts `(void)knet_pvid_filter_create(slot, name)`. If creation fails, untagged/PVID RX frames on that port arrive tagged in Linux and are silently dropped by the IP stack.

11. **[MEDIUM] `hwRemoveMirrorSession` leaves ports & ACLs detached if `hwbcm_mirror_dest_destroy` fails; `hwSetMirrorSession` leaves zombie `destGport = -1` on double rollback failure**
    - **Location:** `SwitchBCM56860HW_mirror.cpp:632-679, 712-726, 756-816`
    - **Detail:**
      - `hwRemoveMirrorSession` detaches all ports, erases `m_portIngMirrors`/`m_portEgrMirrors`, and detaches ACLs *before* calling `hwbcm_mirror_dest_destroy()`. If destroy fails (`:808-813`), it returns error without restoring bindings.
      - `hwSetMirrorSession` is non-hitless (detaches all ports/ACLs, destroys MTP, recreates, re-attaches); if both recreate and rollback fail (`:675-676`), `destGport` becomes `-1` while `m_port*Mirrors` still list the session; if partial port re-attach fails (`:716-725`), `m_port*Mirrors` is out of sync with HW. Unchecked `hwbcm_mirror_port_delete` at `:634,764,786,951,1000,1020`.

12. **[MEDIUM] `hwRemoveAclTable` leaks `m_aclCounters[c].entries` & wipes HW entries before checking `group_destroy` failure**
    - **Location:** `SwitchBCM56860HW_acl.cpp:1477-1510`
    - **Detail:** Retires all remaining HW entries without erasing them from `m_aclCounters[counter].entries`, and if `hwbcm_acl_group_destroy` fails (`:1496-1501`), returns error after all HW rules were already deleted.

13. **[MEDIUM] `SwitchBCM56860HW::create` leaks HW object if `hwAclBindOnCreate` fails**
    - **Location:** `SwitchBCM56860HW.cpp:3007-3031`
    - **Detail:** If `hwCreatePort`/`Lag`/`Vlan`/`RouterInterface` succeeds and then `hwAclBindOnCreate()` (`:3011`) fails, rollback only calls `SwitchBCM56850::remove()` (`:3021`), NOT the corresponding `hwRemove*()`, leaking the newly created HW object in the ASIC.

14. **[MEDIUM] `hwRemoveAclCounter` does not check for active entry references & `hwSetAclCounter` corrupts offset on `stat_get` error**
    - **Location:** `SwitchBCM56860HW_acl.cpp:1689, 1709-1717`
    - **Detail:** Removing a counter still referenced by entries succeeds instead of returning `SAI_STATUS_OBJECT_IN_USE`; in `hwSetAclCounter`, ignoring a failed `hwbcm_acl_stat_get` causes `attr->value.u64 - livePkts` to underflow/skew by the missed variant's count.

15. **[MEDIUM] `trap_l2cache_set` deletes old L2 cache entry before inserting new one & ignores `bcm_stk_my_modid_get` return value**
    - **Location:** `hwbcm_core.c:3573-3602`
    - **Detail:** If `bcm_l2_cache_set` fails (`:3597`), the previous L2 cache trap entry was already deleted (`:3574`), causing link-local BPDUs/LACP/LLDP to flood instead of trapping. Return value of `bcm_stk_my_modid_get` at `:3594` is unchecked.

16. **[MEDIUM] `hwCoppCounterStats` only returns first trap's stats if multiple traps share one `COUNTER_ID`**
    - **Location:** `SwitchBCM56860HW_copp.cpp:785-827`
    - **Detail:** Breaks/returns immediately on the first matching trap (`:826`) instead of summing across all traps bound to `counter_id`.

17. **[MEDIUM] `hwbcm_copp.c` ARP request/response & SSH/SNMP classifier blind spots**
    - **Location:** `hwbcm_copp.c:200-213, 271-278`
    - **Detail:** Unicast ARP requests (Linux NUD probes) miss `ARP_REQUEST` (which requires `DMAC=ff:ff:ff:ff:ff:ff`) and hit `ARP_RESPONSE`; outbound SSH/SNMP return traffic (`sport=22/161`) misses `SSH`/`SNMP` (which only match `dport=22/161`) and falls through to `IP2ME`.

18. **[MEDIUM] `SwitchBCM56860HW_acl.cpp` `FIELD_TOS` vs `FIELD_DSCP`/`FIELD_ECN` clobbering**
    - **Location:** `SwitchBCM56860HW_acl.cpp:689-705`
    - **Detail:** Iterating `m_objectHash` (ordered by attribute name string) means `FIELD_TOS` overwrites `r.tos`/`r.tos_mask` completely, clobbering `DSCP`/`ECN` if both are present.

---

### Low Severity

19. **[LOW] Missing mutexes in `hwbcm_acl.c` (`s_groups[]`, `s_modid`) and `hwbcm_core.c` (`g_knet[]`, `g_trap_l2cache_idx[]`)**
    - **Location:** `hwbcm_acl.c:56,84`; `hwbcm_core.c:276,448`
    - **Detail:** Unlike `hwbcm_copp.c` (`g_copp_lock`), these globals have no internal locking (safe under `Sai.cpp`'s outer mutex for SAI calls, but unsafe if invoked from diag shell or async threads).

20. **[LOW] `SwitchBCM56860HW_acl.cpp:905-914` silently truncates >4 mirror sessions on an ACL entry**
    - **Detail:** Excess entries beyond `HWBCM_ACL_MAX_MIRRORS` (4) are silently dropped with no warning or error.

21. **[LOW] `SwitchBCM56860HW_mirror.cpp:412` reads `a.value.u8` for `SAI_MIRROR_SESSION_ATTR_TOS` (which is `sai_uint16_t` in SAI)**
    - **Detail:** Works on little-endian x86_64 for 8-bit IPv4 TOS, but reads wrong union member (`u8` vs `u16`).

22. **[LOW] `SwitchBCM56860HW_mirror.cpp:956` uses `m_mirrorSessions[rSess]` (`operator[]`) instead of `.find()` during rollback**
    - **Detail:** Could default-insert an empty entry if `rSess` were ever absent.

23. **[LOW] `SwitchBCM56860HW_acl.cpp:1004` installs dead TCAM rule on `scope.all` with empty `pr.inPorts`; `SwitchBCM56860HW.cpp:1989-1996` never updates KNET netdev MAC if `SRC_MAC_ADDRESS` set after hostif create; `hwbcm_copp.c:1324-1329` `snprintf` offset accumulation doesn't clamp on truncation; `SwitchBCM56860HW_copp.cpp:672-682` dead `attrs` population on `TRAP_GROUP` set.**

---

## Verification Notes
- Every SDK API cited above was directly verified in `/home/kaguyou/homelab/openbcm-src/sdk-6.5.27-wb/` headers and `src/bcm/esw/` implementation files (including `trident2plus/field.c`, `trx/field.c`, `mirror.c`, `port.c`, `rate.c`, `policer.c`, `switch.c`, `feature.c`, and `bcm-knet.c`).
- All orchagent call sites were verified directly in `/home/kaguyou/homelab/sonic-swss/orchagent/` and `/home/kaguyou/homelab/sonic-sairedis/syncd/FlexCounter.cpp`.
- No files were modified during this audit.
