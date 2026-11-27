# Domain 2 SAI Parity Audit (Part 2 of 2): Gap Analysis & Robustness Bugs

Lab testability baseline used below: **1 AS5812-54T switch + 2x 1G copper links to 1 PC (1 mgmt port in netns + 1 front-panel 1G data port, no 40G, no 2nd switch)**.
Key lab insight: Using 802.1Q VLAN subinterfaces (`eth0.10`, `eth0.20`, ...) and Linux netns / `vxlan` / `ipip` / `gre` / `scapy` on the PC over the single front-panel 1G copper link allows testing almost all L3, IPv6, VRF, VLAN/SVI, sub-port, MTU, uRPF, VXLAN, IPinIP, MPLS, NAT, and even 2-path VLAN-SVI ECMP behaviors; only multi-physical-port ECMP/LAG load distribution, multi-switch BFD/NVGRE, or physical link-failover across 2 data links require a 2nd data link.

---

## 2. Detailed Analysis of Every `(b)` Silent No-Op and `(c)` Error Attribute

### 2.1 VIRTUAL_ROUTER Gaps
1. **`SAI_VIRTUAL_ROUTER_ATTR_ADMIN_V4_STATE` / `_ADMIN_V6_STATE` (create & set) — `(b)`**
   - **Vendor TD2+ `bcm_*` APIs**: Track per-VRF v4/v6 admin state and apply to every RIF in that VRF via `bcm_l3_ingress_create()` (`BCM_L3_INGRESS_WITH_ID | BCM_L3_INGRESS_REPLACE` with `BCM_L3_INGRESS_ROUTE_DISABLE_IP4_UCAST` / `BCM_L3_INGRESS_ROUTE_DISABLE_IP6_UCAST`) in L3 ingress mode, or `bcm_vlan_control_vlan_set()` (`BCM_VLAN_IP4_DISABLE` / `BCM_VLAN_IP6_DISABLE`) in non-ingress mode.
   - **SONiC consequence**: Configuring `v4=false` or `v6=false` on a VRF (`vrforch.cpp:38-47`) fails to stop forwarding in HW; traffic continues routing in that VRF.
   - **Effort**: **S**
   - **Testable on 1-switch + 1-PC lab**: **Yes** — put two VLAN subinterfaces on PC in same VRF, send routed v4/v6 packets, toggle `v4`/`v6` false and verify forwarding stops.

2. **`SAI_VIRTUAL_ROUTER_ATTR_SRC_MAC_ADDRESS` (create & set) — `(b)`**
   - **Vendor TD2+ `bcm_*` APIs**: Store per-VR MAC; use as fallback when RIF omits `SRC_MAC_ADDRESS` (before falling back to switch MAC), and on `set` update `bcm_l3_intf_create(BCM_L3_WITH_ID|BCM_L3_REPLACE)` + `bcm_l2_station_delete`/`bcm_l2_station_add` for RIFs inheriting VR MAC.
   - **SONiC consequence**: Custom VRF/VNet router MAC (`vrforch.cpp:48-53`, `vnetorch.cpp:441`) is silently ignored; switch global MAC is used instead, breaking VNet/VRF-specific MAC termination and egress SMAC rewrite.
   - **Effort**: **S**
   - **Testable on 1-switch + 1-PC lab**: **Yes** — create VRF with custom `src_mac`, verify routed packet egress SMAC & MY_STATION hit in `tcpdump`.

3. **`SAI_VIRTUAL_ROUTER_ATTR_VIOLATION_TTL1_PACKET_ACTION`, `_VIOLATION_IP_OPTIONS_PACKET_ACTION`, `_UNKNOWN_L3_MULTICAST_PACKET_ACTION` (create & set) — `(b)`**
   - **Vendor TD2+ `bcm_*` APIs**:
     - TTL1: TD2+ has global `bcm_switch_control_set(unit, bcmSwitchL3UcastTtl1ToCpu, ...)` and `bcmSwitchL3UcTtlErrToCpu` (per-VRF granularity requires IFP `bcm_field_*` with `bcmFieldQualifyVrf` + `bcmFieldQualifyTtl`).
     - IP options: `bcm_switch_control_set(unit, bcmSwitchL3HeaderErrToCpu, ...)` / `bcmSwitchL3SlowpathToCpu` (or IFP `bcmFieldQualifyIpInfo` + `bcmFieldQualifyVrf`).
     - Unknown L3 MC: per-L3-IIF `bcm_l3_ingress_t.flags` (`BCM_L3_INGRESS_UNKNOWN_IP4_MCAST_TOCPU`, `BCM_L3_INGRESS_UNKNOWN_IP6_MCAST_TOCPU`) via `bcm_l3_ingress_create()`.
   - **SONiC consequence**: VRF-level `ttl_action`, `ip_opt_action`, `l3_mc_action` (`vrforch.cpp:54-68`) have no HW effect; TTL=1 always follows global hostif trap.
   - **Effort**: **M**
   - **Testable on 1-switch + 1-PC lab**: **Yes** — send TTL=1, IPv4-with-options, and unknown multicast packets via `scapy` on PC and observe CPU `tcpdump` vs drop.

### 2.2 ROUTER_INTERFACE Gaps
4. **`SAI_ROUTER_INTERFACE_TYPE_SUB_PORT` + `SAI_ROUTER_INTERFACE_ATTR_OUTER_VLAN_ID` — `(b)`**
   - **Vendor TD2+ `bcm_*` APIs**: Enable `bcm_vlan_control_port_set(unit, port, bcmVlanTranslateIngressEnable, 1)` + `bcm_vlan_port_create()` (or `bcm_vlan_translate_action_add` + `bcm_l3_ingress_create()` + `bcm_vlan_gport_add()` + `bcm_l3_intf_create()` with `l3a_vid = outer_vlan` and `bcm_l3_egress_t.vlan = outer_vlan`, plus egress VLAN xlate if needed) so `(port, outer_vid)` maps to a dedicated L3_IIF/VRF.
   - **SONiC consequence**: Creating routed sub-interfaces (`Ethernet0.100`, `PortChannel0001.100` in `intfsorch.cpp:1223,1250-1265`) silently succeeds in SW DB with NO HW L3 interface; all sub-interface traffic fails to route and neighbors on it are ignored (`_l3.cpp:838`).
   - **Effort**: **M**
   - **Testable on 1-switch + 1-PC lab**: **Yes** — configure two sub-interfaces (`Ethernet0.10`, `Ethernet0.20`) on the 1G copper link to PC and route between `eth0.10` and `eth0.20` on PC.

5. **`SAI_ROUTER_INTERFACE_ATTR_ADMIN_V4_STATE` / `_ADMIN_V6_STATE` (create & set) — `(b)`**
   - **Vendor TD2+ `bcm_*` APIs**:
     - Non-ingress mode: `bcm_vlan_control_vlan_get`/`set()` toggling `BCM_VLAN_IP4_DISABLE` / `BCM_VLAN_IP6_DISABLE`.
     - L3 ingress mode (RIOT): `bcm_l3_ingress_get`/`create(BCM_L3_INGRESS_WITH_ID|BCM_L3_INGRESS_REPLACE)` toggling `BCM_L3_INGRESS_ROUTE_DISABLE_IP4_UCAST` / `BCM_L3_INGRESS_ROUTE_DISABLE_IP6_UCAST` (and on RIOT VPN overlay IIF as well).
   - **SONiC consequence**: Administratively disabling IPv4 or IPv6 routing on an interface (`intfsorch.cpp:311-326`, e.g., admin-down SVI/subport) leaves HW routing active!
   - **Effort**: **S**
   - **Testable on 1-switch + 1-PC lab**: **Yes** — disable IPv4/IPv6 admin state on a VLAN RIF and verify routed packets stop forwarding in HW.

6. **`SAI_ROUTER_INTERFACE_ATTR_SRC_MAC_ADDRESS` (set) — `(b)`**
   - **Vendor TD2+ `bcm_*` APIs**: `bcm_l3_intf_get()` -> update `intf.l3a_mac_addr` -> `bcm_l3_intf_create(BCM_L3_WITH_ID|BCM_L3_REPLACE|BCM_L3_ADD_TO_ARL)` (note: also remove old L2 ARL/station entry via `bcm_l2_addr_delete` / `bcm_l2_station_delete` and update RIOT overlay intf + CPU L2 entry if VNI-mapped).
   - **SONiC consequence**: Changing an SVI/interface MAC after creation (`intfsorch.cpp:251-254,1009-1015`, e.g., VRRP/anycast gateway MAC update) is silently ignored in HW; packets to new MAC are not routed and egress still carries old SMAC.
   - **Effort**: **S**
   - **Testable on 1-switch + 1-PC lab**: **Yes** — update SVI MAC via CONFIG_DB and verify new SMAC on routed egress + termination of new DMAC via `scapy`/`tcpdump`.

7. **`SAI_ROUTER_INTERFACE_ATTR_LOOPBACK_PACKET_ACTION` (create & set) — `(b)`**
   - **Vendor TD2+ `bcm_*` APIs**: Per-L3-IIF `bcm_l3_ingress_t.flags |= BCM_L3_INGRESS_ICMP_REDIRECT_TOCPU` (for `TRAP`/`COPY` on same-interface routing) and global/per-interface `bcm_switch_control_set(unit, bcmSwitchL3UcSameInterfaceDrop, ...)` (for `DROP`, `include/bcm/switch.h:2790`).
   - **SONiC consequence**: Hairpin/same-RIF loopback action (`intfsorch.cpp:440-443,1191`) is ignored; packets routed back out the same RIF are forwarded regardless of `DROP`/`TRAP` config.
   - **Effort**: **S**
   - **Testable on 1-switch + 1-PC lab**: **Yes** — route between two hosts/IPs on the same VLAN SVI over the 1G link and check drop/punt/forward behavior.

8. **`SAI_ROUTER_INTERFACE_STAT_*` (`get_router_interface_stats[_ext]` / `clear`) — `(b)`**
   - **Vendor TD2+ `bcm_*` APIs**: Attach flex counters via `bcm_stat_group_create(unit, bcmStatObjectIngL3Intf, ...)` + `bcm_l3_ingress_stat_attach()` + `bcm_l3_ingress_stat_counter_get()` (for `IN_PACKETS`, `IN_OCTETS`, `IN_ERROR_*`), and `bcm_stat_group_create(unit, bcmStatObjectEgrVlan, ...)` + `bcm_vlan_stat_attach()` + `bcm_vlan_stat_counter_get()` (or per-egress `bcmStatObjectEgrL3Intf` + `bcm_l3_egress_stat_attach()`; note `bcm_l3_intf_stat_attach` is NOT implemented in ESW TD2+ SDK). Also expose in `queryStatsCapability()`.
   - **SONiC consequence**: `show interfaces counters rif` (`intfsorch.cpp:50-57`) always shows all zeros.
   - **Effort**: **M**
   - **Testable on 1-switch + 1-PC lab**: **Yes** — route packets between two VLAN SVIs on PC and verify `show interfaces counters rif` increments.

9. **`SAI_ROUTER_INTERFACE_ATTR_ADMIN_MPLS_STATE` & `ATTR_NAT_ZONE_ID` (create & set) — `(b)`**
   - **Vendor TD2+ `bcm_*` APIs**:
     - MPLS: `bcm_l3_ingress_t.flags` (`BCM_L3_INGRESS_ROUTE_DISABLE_MPLS`) via `bcm_l3_ingress_create()`, or `bcm_port_control_set(port, bcmPortControlMpls, ...)`.
     - NAT zone: `bcm_l3_ingress_t.flags |= BCM_L3_INGRESS_SRC_DST_NAT_REALM_ID` + `bcm_l3_ingress_t.nat_realm_id` via `bcm_l3_ingress_create()`.
   - **SONiC consequence**: MPLS label switching enablement and NAT inside/outside zone assignment (`intfsorch.cpp:200,284,1279,1288`) do nothing in HW.
   - **Effort**: **S** (just the RIF attribute once MPLS/NAT modules exist; **L** for full MPLS/NAT).
   - **Testable on 1-switch + 1-PC lab**: **Yes** (with MPLS/NAT implemented; PC can craft MPLS/NAT packets via `scapy`/Linux).

### 2.3 NEIGHBOR_ENTRY Gaps
10. **`set_neighbor_entry_attribute` (`SAI_NEIGHBOR_ENTRY_ATTR_DST_MAC_ADDRESS`, `NO_HOST_ROUTE`, `PACKET_ACTION`) — `(b)`**
    - **Vendor TD2+ `bcm_*` APIs**: Dispatch `SAI_OBJECT_TYPE_NEIGHBOR_ENTRY` in `SwitchBCM56860HW::set()` -> `hwSetNeighbor()`: resolve new FDB target `(port, trunk, vpGport, ovlIntf)`, call `hwbcm_l3_egress_replace()` / `hwbcm_vxlan_overlay_egress_replace()` / `hwbcm_l3_egress_cpu_replace()` on `m_neighborEgr[key]` (and all `matchingNh`!), call `hwbcm_l3_host_add`/`delete` if `NO_HOST_ROUTE` toggled, and call `hwVxlanRetryPending()`.
    - **SONiC consequence**: **Severe operational bug!** Whenever an existing neighbor changes MAC (NIC replacement, VRRP failover, GARP, VM migration), `neighorch.cpp:1212` calls `set_neighbor_entry_attribute`, which silently succeeds while the ASIC keeps forwarding to the **old MAC**, blackholing traffic until ARP expires and is deleted+re-added!
    - **Effort**: **S**
    - **Testable on 1-switch + 1-PC lab**: **Yes** — change MAC of a PC VLAN subinterface / send GARP for existing IP, verify routed packet DMAC updates immediately in `tcpdump`.

11. **Neighbor MAC move on VLAN RIF (ASIC L2 learn callback) — `(b)`**
    - **Vendor TD2+ `bcm_*` APIs**: Already have `hwbcm_l3_egress_replace()`; fix `SwitchBCM56860HW.cpp:1710-1714` to check whether the learned `(port/trunk)` differs from the stored `BRIDGE_PORT_ID` in `fdbHash[sid]` instead of returning early whenever `sid` exists! Update `fdbHash`, emit `SAI_FDB_EVENT_MOVE`, and call `hwRefreshNeighborsOnLearn()`.
    - **SONiC consequence**: Moving a host (same IP+MAC) from one switchport to another on the same VLAN leaves both FDB DB and L3 neighbor egress pinned to the old port (blackhole) until FDB ages out.
    - **Effort**: **S**
    - **Testable on 1-switch + 1-PC lab**: **No** (for real physical port-to-port MAC move, requires 2 front-panel data links; with 1 data link only learn/age/flush on 1 port can be tested).

12. **`SAI_NEIGHBOR_ENTRY_ATTR_PACKET_ACTION` (`TRAP`/`COPY`/`LOG`) — `(b)`**
    - **Vendor TD2+ `bcm_*` APIs**: `bcm_l3_egress_create()` with `BCM_L3_L2TOCPU` (`TRAP`), `BCM_L3_COPY_TO_CPU` (`COPY`/`LOG`), `BCM_L3_DST_DISCARD` (`DROP`).
    - **SONiC consequence**: Low in default SONiC (`neighorch.cpp` leaves default `FORWARD`), but breaks any caller setting `TRAP`/`COPY` on a neighbor.
    - **Effort**: **S**
    - **Testable on 1-switch + 1-PC lab**: **Yes** — via `sai` unit test or direct APP_DB/syncd test.

### 2.4 NEXT_HOP, NEXT_HOP_GROUP & ROUTE_ENTRY Gaps
13. **`SAI_NEXT_HOP_TYPE_TUNNEL_ENCAP` (`TUNNEL_ID`, `TUNNEL_VNI`, `TUNNEL_MAC`, `IP`) — `(b)` on create, `(c)` on route/NHG use**
    - **Vendor TD2+ `bcm_*` APIs**:
      - For VXLAN EVPN Type-5 (RIOT): `bcm_l3_intf_create()` (overlay L3 intf on L3-VNI VPN) + `bcm_l3_egress_create()` with `BCM_L3_FLAGS2_UNDERLAY` (underlay VXLAN-only egress) + overlay egress (`intf = l3vni_ovl_intf`, `mac_addr = TUNNEL_MAC`, `port = remote_vtep_vp_gport`) — see TD2+ RIOT Type-5 recipe (`bcm_vxlan_vpn_create` L3 VNI + `bcm_l3_egress_create`).
      - For IPinIP encap: `bcm_l3_intf_create()` + `bcm_tunnel_initiator_set()` (`bcmTunnelTypeIp4In4`/`Ip6In4`/`Ip4In6`/`Ip6In6`) + `bcm_l3_egress_create()` pointing to that tunnel L3 intf & underlay next hop.
    - **SONiC consequence**: **Completely breaks EVPN Type-5 L3 VNI routes (`vxlanorch.cpp:238-266`), VNet VXLAN routes (`vnetorch.cpp`), IPinIP encap routes (`tunneldecaporch.cpp:1303-1319`), and Dual-ToR Mux (`muxorch.cpp:325-336`)!** Next-hop creation silently succeeds, then route/NHG member creation FAILS with `SAI_STATUS_FAILURE`, stalling or crashing orchagent!
    - **Effort**: **L** (separate implementation for VXLAN Type-5 RIOT vs IPinIP encap)
    - **Testable on 1-switch + 1-PC lab**: **Yes** — PC Linux `ip link add type vxlan` (with VRF/L3 VNI) or `ipip` tunnel over the 1G data link; verify encapsulated outer IP/UDP/VXLAN/RMAC in `tcpdump`.

14. **`SAI_NEXT_HOP_GROUP_MEMBER_ATTR_WEIGHT` (create & set) — `(b)`**
    - **Vendor TD2+ `bcm_*` APIs**: On TD2+, `bcm_l3_egress_ecmp_create()` / `bcm_l3_egress_ecmp_add()` (or `bcm_l3_ecmp_create()` with `bcm_l3_ecmp_member_t`) implements WCMP by repeating each member's egress ID `weight` times (scaled/normalized to fit `max_paths`, max 1024 on TD2+), removing the deduplication in `groupEgressList()` (`_l3.cpp:274`) and dispatching `SAI_OBJECT_TYPE_NEXT_HOP_GROUP_MEMBER` in `SwitchBCM56860HW::set()`.
    - **SONiC consequence**: BGP Link Bandwidth / WCMP (`routeorch.cpp:491,1559`, `nhgorch.cpp:611-614,1116`) silently degrades to equal-cost ECMP; updating member weight does nothing in HW.
    - **Effort**: **S**
    - **Testable on 1-switch + 1-PC lab**: **Yes** — configure two VLAN SVIs (`Vlan10`, `Vlan20`) on the 1G trunk to PC with 2 different next hops and weight 3:1; send 1000 flows with varied L4 ports via `scapy` and measure ~75%/25% split in `tcpdump`.

15. **`SAI_NEXT_HOP_GROUP_TYPE_DYNAMIC_ORDERED_ECMP` + `ATTR_SEQUENCE_ID` — `(b)`**
    - **Vendor TD2+ `bcm_*` APIs**: Sort member array by `SAI_NEXT_HOP_GROUP_MEMBER_ATTR_SEQUENCE_ID` before passing to `bcm_l3_egress_ecmp_create()` (instead of `std::map` OID order in `groupEgressList()`).
    - **SONiC consequence**: `ordered_ecmp=true` (`switchorch.cpp:405-443`, `routeorch.cpp:1509`, `vnetorch.cpp:785`) is falsely advertised as supported by `SwitchStateBase:3986`, but HW ECMP member ordering ignores `SEQUENCE_ID`.
    - **Effort**: **S**
    - **Testable on 1-switch + 1-PC lab**: **Yes** — inspect ECMP member table order via `bsh "d chg L3_ECMP"` after adding members with explicit `sequence_id`s.

16. **`SAI_NEXT_HOP_GROUP_TYPE_FINE_GRAIN_ECMP` (`CONFIGURED_SIZE`, `REAL_SIZE`, `MEMBER_ATTR_INDEX`, `set(MEMBER_ATTR_NEXT_HOP_ID)`) — `(b)/(c)`**
    - **Vendor TD2+ `bcm_*` APIs**: Pre-allocate fixed-size ECMP table of `CONFIGURED_SIZE` slots via `bcm_l3_egress_ecmp_create()` (setting `ecmp.max_paths = configured_size` and `ecmp.dynamic_mode = BCM_L3_ECMP_DYNAMIC_MODE_RESILIENT` or fixed-array slot table), maintain slot vector `slots[index] = egr_id`, update single slot in place on `set(NEXT_HOP_ID)` without shifting other slots, and populate `REAL_SIZE` on `get`. (Or override `queryNextHopGroupTypeCapability` to NOT advertise `FINE_GRAIN_ECMP` if unimplemented.)
    - **SONiC consequence**: `FgNhgOrch` (`fgnhgorch.cpp:234-291,1154-1167`) creates a group (may error on `get(REAL_SIZE)`), then slot-indexed replacements (`set_next_hop_group_member_attribute`) are silent no-ops, breaking consistent/fine-grained ECMP hashing!
    - **Effort**: **M**
    - **Testable on 1-switch + 1-PC lab**: **Yes** — via two VLAN SVIs on PC + `bsh "d chg L3_ECMP"` to verify exact slot indices and minimal flow disruption on single-slot update.

17. **`SAI_NEXT_HOP_GROUP_TYPE_CLASS_BASED` (`SELECTION_MAP`, `NEXT_HOP_GROUP_MAP`) — `(b)`**
    - **Vendor TD2+ `bcm_*` APIs**: TD2+ does not have native SAI CBF hardware selection tables in `L3_ECMP_GROUP` (requires IFP `bcm_field_*` `bcmFieldActionRedirectEcmp` / `bcmFieldActionL3Switch` matching DSCP/TC/class-id). Should override `queryNextHopGroupTypeCapability` to NOT advertise `SAI_NEXT_HOP_GROUP_TYPE_CLASS_BASED` and return `SAI_STATUS_NOT_SUPPORTED`.
    - **SONiC consequence**: `CbfNhgOrch` (`cbf/cbfnhgorch.cpp:301-790`) silently creates plain ECMP groups ignoring traffic class selection maps.
    - **Effort**: **S** (to properly report `NOT_SUPPORTED`), **L** (for IFP emulation).
    - **Testable on 1-switch + 1-PC lab**: **Yes** — DSCP-marked flows over 2 VLAN SVIs on PC.

18. **ECMP Switch Attributes & Capabilities (`NUMBER_OF_ECMP_GROUPS`, `MAX_ECMP_MEMBER_COUNT`, `ECMP_DEFAULT_HASH_ALGORITHM`, `ECMP_DEFAULT_HASH_OFFSET`, `ECMP_MEMBER_COUNT`) — `(b)`**
    - **Vendor TD2+ `bcm_*` APIs**:
      - `bcm_l3_info()` (`l3info_max_ecmp_groups`) & `bcm_l3_route_max_ecmp_get/set()` for ECMP group/member limits.
      - `bcm_switch_control_set(unit, bcmSwitchHashField0Config, BCM_HASH_FIELD_CONFIG_CRC32LO/HI/CRC16/XOR)` for hash algorithm; `bcm_switch_control_set(unit, bcmSwitchHashSelectControl, ...)` / `bcmSwitchECMPHashSet0Offset` for hash offset.
    - **SONiC consequence**: `routeorch.cpp:60,95` reads fake vslib limits instead of TD2+ hardware limits; changing ECMP hash algorithm/offset via `CONFIG_DB` (`switchorch.cpp:41,586,679`) does nothing in HW.
    - **Effort**: **S**
    - **Testable on 1-switch + 1-PC lab**: **Yes** — verify via `bsh` RTAG7 registers and `STATE_DB` `SWITCH_CAPABILITY`.

19. **`SAI_ROUTE_ENTRY_ATTR_COUNTER_ID` & `SAI_ROUTE_ENTRY_ATTR_PREFIX_AGG_ID` — `(b)`**
    - **Vendor TD2+ `bcm_*` APIs**: `bcm_stat_group_create(unit, bcmStatObjectIngL3Route, ...)` + `bcm_l3_route_stat_attach()` + `bcm_l3_route_stat_counter_get()` (TD2+ `L3_DEFIP` flex counters verified in `src/bcm/esw/trident2/l3.c:5057-5400`). Also override `queryAttributeCapability()` to report `false` until implemented!
    - **SONiC consequence**: `FlowCounterRouteOrch` (`flow_counter_handler.cpp:54`) queries capability, gets a false `true` from `SwitchStateBase:4165`, creates `COUNTER` objects and binds them to routes (`flowcounterrouteorch.cpp:472,498`), and all route flow counters permanently read 0!
    - **Effort**: **M**
    - **Testable on 1-switch + 1-PC lab**: **Yes** — enable route flow counter for a prefix, send routed packets from PC, verify counter increments.

20. **`SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION` = `COPY` / `LOG` — `(b)` (wrong behavior)**
    - **Vendor TD2+ `bcm_*` APIs**: Instead of `sharedCpuEgress()` (`BCM_L3_L2TOCPU`, which punts WITHOUT forwarding), clone/create egress with `BCM_L3_COPY_TO_CPU` on the real next hop.
    - **SONiC consequence**: Routes with `COPY`/`LOG` action punt to CPU but drop the forwarded copy.
    - **Effort**: **S**
    - **Testable on 1-switch + 1-PC lab**: **Yes** — verify both forwarded packet on PC and CPU copy on switch.

### 2.5 IPv6 & MTU Check Gaps
21. **IPv6 Unknown Dest / L3 Err / ND Drop Traps (`HOSTIF_TRAP_TYPE_NEIGHBOR_MISS`, `IPV6_ND/NS/NA`) — `(b)`**
    - **Vendor TD2+ `bcm_*` APIs**:
      - Add `bcm_switch_control_set(unit, bcmSwitchV6L3DstMissToCpu, to_cpu)` and `bcm_switch_control_set(unit, bcmSwitchV6L3ErrToCpu, to_cpu)` in `HWBCM_TRAP_NEIGH_MISS` / `HWBCM_TRAP_TTL_ERR` (`hwbcm_core.c:3668-3675`).
      - Separate `HWBCM_TRAP_ND` from `HWBCM_TRAP_ARP` and program `bcmSwitchNdPktToCpu` + `bcmSwitchNdPktDrop`.
    - **SONiC consequence**: IPv6 neighbor miss packets are NOT punted via `V6L3DSTMISS_TOCPU` (only covered if a connected subnet route points to CPU egress); ARP and IPv6 ND trap actions overwrite each other; IPv6 ND `TRAP` does not set `bcmSwitchNdPktDrop`.
    - **Effort**: **S**
    - **Testable on 1-switch + 1-PC lab**: **Yes** — send IPv6 NS/NA and unresolvable IPv6 packets from PC, inspect CPU KNET capture & `bsh "g CPU_CONTROL_1"`.

22. **`SAI_HOSTIF_TRAP_TYPE_L3_MTU_ERROR` — `(b)` (misrouted!)**
    - **Vendor TD2+ `bcm_*` APIs**: Add dedicated `HWBCM_TRAP_L3_MTU_ERR` calling `bcm_switch_control_set(unit, bcmSwitchL3MtuFailToCpu, to_cpu)` (`src/bcm/esw/switch.c:13859`: `CPU_CONTROL_1.L3_MTU_FAIL_TOCPUf`) instead of routing `SAI_HOSTIF_TRAP_TYPE_L3_MTU_ERROR` to `HWBCM_TRAP_NEIGH_MISS` (`SwitchBCM56860HW.cpp:2099`).
    - **SONiC consequence**: Oversize routed packets exceeding `EGR_L3_INTF.MTU` are silently dropped by ASIC and NEVER punted to the Linux kernel, breaking Path MTU Discovery (no ICMP Fragmentation Needed / ICMPv6 Packet Too Big generated)!
    - **Effort**: **S**
    - **Testable on 1-switch + 1-PC lab**: **Yes** — set SVI MTU to 1400, send 1500B routed packet (DF=1 or IPv6) from PC VLAN A -> VLAN B, verify switch sends ICMP Frag-Needed / Packet-Too-Big back to PC.

### 2.6 VXLAN / EVPN Gaps
23. **VXLAN L3 VNI (`VNI_TO_VIRTUAL_ROUTER_ID` / `VIRTUAL_ROUTER_ID_TO_VNI` — EVPN Type-5 / Symmetric IRB) — `(b)`**
    - **Vendor TD2+ `bcm_*` APIs**: With `riot_enable=1`: `bcm_vxlan_vpn_create()` (L3 VNI VPN / VFI) + `bcm_l3_intf_create()` (overlay intf on L3 VNI VFI with RMAC) + `bcm_l3_ingress_create()` (VRF mapped to L3 VNI VFI via `bcm_vlan_control_vlan_set`) + `bcm_l3_egress_create()` (overlay egress with `TUNNEL_MAC` + remote VTEP VP, paired with `TYPE_TUNNEL_ENCAP` next hop).
    - **SONiC consequence**: EVPN Type-5 inter-VTEP L3 routing (`vxlanorch.cpp:41-42,56-59`, `vrforch.cpp:200-289`) is completely non-functional in HW (only L2 VNI + symmetric-less SVI gateway via RIOT works).
    - **Effort**: **L**
    - **Testable on 1-switch + 1-PC lab**: **Yes** — PC acts as remote VTEP (Linux VXLAN device with L3 VNI + RMAC) on the 1G data link; verify encapsulated Type-5 packet and decapsulated routed packet.

24. **VXLAN Tunnel Stats (`SAI_TUNNEL_STAT_IN/OUT_OCTETS/PACKETS`) — `(b)`**
    - **Vendor TD2+ `bcm_*` APIs**: `bcm_stat_group_create(unit, bcmStatObjectIngVxlan/EgrVxlan, ...)` + `bcm_vxlan_stat_attach()` + `bcm_vxlan_stat_multi_get()` (`include/bcm/vxlan.h:480`, `src/bcm/esw/vxlan.c:1422`), wired into `SwitchBCM56860HW::getStatsExt()`.
    - **SONiC consequence**: `show vxlan tunnel` / flex counter tunnel stats (`vxlanorch.cpp:71-74`) always report 0.
    - **Effort**: **M**
    - **Testable on 1-switch + 1-PC lab**: **Yes** — send VXLAN traffic to/from PC Linux VXLAN endpoint and check tunnel counters.

25. **VXLAN P2MP-only mode (`FDB_ENTRY_ATTR_ENDPOINT_IP` without P2P tunnels) & Local Static MAC on VNI-mapped VLAN — `(b)`**
    - **Vendor TD2+ `bcm_*` APIs**: For P2MP FDB `ENDPOINT_IP`: dynamically create/refcount `VxlanRemoteVtep` on demand from `ENDPOINT_IP`. For local static MAC on VNI-mapped VLAN (`SwitchBCM56860HW.cpp:2492-2500`): look up the access VP for `(vpn, port/trunk)` in `m_vpnAccessVps` and call `bcm_l2_addr_add()` with `vid = vpn`, `port = access_vp`.
    - **SONiC consequence**: Static MACs on a VNI-mapped VLAN are ignored in HW (`.cpp:2496`); P2MP-mode VXLAN without P2P tunnel creation fails to forward remote MACs.
    - **Effort**: **S** (local static MAC on VNI VLAN) / **M** (P2MP dynamic VTEP).
    - **Testable on 1-switch + 1-PC lab**: **Yes** — add static MAC on VXLAN-mapped VLAN and verify in `bsh "l2 show"` and packet forwarding.

26. **VXLAN Switch Attrs (`SAI_SWITCH_ATTR_VXLAN_DEFAULT_PORT`, `_DEFAULT_ROUTER_MAC`, `SWITCH_TUNNEL` UDP sport) & VXLAN `ENCAP_TTL_MODE` UNIFORM / P2MP inheritance — `(b)`**
    - **Vendor TD2+ `bcm_*` APIs**:
      - `bcm_switch_control_set(unit, bcmSwitchVxlanUdpDestPortSet, port)` (already used in `hwbcm_vxlan_init`, just wire from `hwSetSwitch` and update `s_udp_port`).
      - `bcm_l2_station_add()` for `VXLAN_DEFAULT_ROUTER_MAC`.
      - `bcm_switch_control_set(unit, bcmSwitchVxlanEntropyEnable, ...)` / `bcmSwitchVxlanUdpSrcPortSelect` + `init.udp_src_port` for `SWITCH_TUNNEL` user-defined UDP sport.
      - For TTL uniform mode: `bcm_tunnel_initiator_t.flags |= BCM_TUNNEL_INIT_USE_OUTER_TTL` (or `ttl = 0` copy-inner on TD2+; verify in SDK); fix `_vxlan.cpp:962` to inherit `m_vtepTtl`/`m_vtepDscp`.
    - **SONiC consequence**: Custom VXLAN UDP port, router MAC, or source port range configured via `CONFIG_DB` (`switchorch.cpp:39-40,455-485`) are silently ignored.
    - **Effort**: **S**
    - **Testable on 1-switch + 1-PC lab**: **Yes** — verify outer UDP dst/src port and TTL in `tcpdump` on PC.

27. **VXLAN 1D Bridge (`SAI_BRIDGE_TYPE_1D`, `VNI_TO_BRIDGE_IF` / `BRIDGE_IF_TO_VNI`) — `(b)`**
    - **Vendor TD2+ `bcm_*` APIs**: Map 1D bridge OID directly to a `bcm_vxlan_vpn_create()` VFI, and 1D bridge sub-ports (`BRIDGE_PORT_TYPE_SUB_PORT`) to access VPs (`bcm_vxlan_port_add`) in that VFI.
    - **SONiC consequence**: 1D bridge VXLAN mode (`vxlanorch.cpp:43-44`) is unimplemented (standard SONiC EVPN uses 1Q VLAN-based VNIs, so low impact unless 1D bridges are configured).
    - **Effort**: **M**
    - **Testable on 1-switch + 1-PC lab**: **Yes** — tagged sub-ports on 1G link to PC.

28. **IPv6 VXLAN Underlay — `(b)` (TD2+ Hardware Limitation)**
    - **Vendor TD2+ `bcm_*` APIs**: **NOT SUPPORTED in TD2+ silicon!** Verified in OpenBCM SDK `src/bcm/esw/trident2/vxlan.c:5152` (`if (info->type != bcmTunnelTypeVxlan) return BCM_E_PARAM;`). Should return `SAI_STATUS_NOT_SUPPORTED` instead of silent `SAI_STATUS_SUCCESS`.
    - **SONiC consequence**: Silently appears created in SW DB while dropping/failing all IPv6-underlay VXLAN traffic.
    - **Effort**: **S** (to return `SAI_STATUS_NOT_SUPPORTED`).
    - **Testable on 1-switch + 1-PC lab**: **Yes** (verify proper rejection).

### 2.7 IPinIP Tunnels (`tunneldecaporch.cpp`, `muxorch.cpp`) Gaps
29. **`SAI_TUNNEL_TYPE_IPINIP` (create/set/remove tunnel, term table P2P/P2MP/MP2MP, decap/encap TTL/DSCP/ECN modes, QoS maps, encap next hop) — `(b)` / `(c)`**
    - **Vendor TD2+ `bcm_*` APIs**:
      - Decap: `bcm_tunnel_terminator_add()` / `update()` / `delete()` (`include/bcm/tunnel.h:595-610`) with `bcmTunnelTypeIp4In4`, `bcmTunnelTypeIp6In4`, `bcmTunnelTypeIp4In6`, `bcmTunnelTypeIp6In6`, `sip`/`sip_mask`/`dip`/`dip_mask`, `vlan` / `ingress_if`, `flags`: `BCM_TUNNEL_TERM_USE_OUTER_TTL` (uniform) vs unset (pipe), `BCM_TUNNEL_TERM_USE_OUTER_DSCP` (uniform) vs `BCM_TUNNEL_TERM_KEEP_INNER_DSCP` (pipe), `BCM_TUNNEL_TERM_ECN_MAP` + `bcm_ecn_map_create()` (`tunnel_term_ecn_map_id`), `bcm_qos_port_map_set()` / `bcm_l3_ingress_t.qos_map_id` for decap QoS maps.
      - Encap: `bcm_l3_intf_create()` + `bcm_tunnel_initiator_set()` (`include/bcm/tunnel.h:526`) + `bcm_l3_egress_create()` for `SAI_NEXT_HOP_TYPE_TUNNEL_ENCAP`.
    - **SONiC consequence**: **Default SONiC IPinIP decap (`TUNNEL` table in `CONFIG_DB`, used by standard SONiC T0/T1/T2 deployments for ECMP/DSCP/ECN decap) is completely non-functional in HW!** Incoming IPinIP packets are never decapsulated (routed/punted as outer IP to loopback instead); `set_tunnel_attribute` is a silent no-op; IPinIP encap routes fail with `SAI_STATUS_FAILURE`.
    - **Effort**: **M** (decap-only: **M**; decap + encap + ECN/QoS maps: **M-L**)
    - **Testable on 1-switch + 1-PC lab**: **Yes** — PC crafts IPv4-in-IPv4 / IPv6-in-IPv4 / IPv4-in-IPv6 / IPv6-in-IPv6 packets with `scapy` or Linux `ipip`/`sit`/`ip6tnl` on the 1G data link and verifies decapsulated inner packet routed back out another VLAN SVI on PC with expected TTL/DSCP/ECN!

### 2.8 Other SONiC 202505 L3/Overlay Orchs Gaps
30. **BFD Offload (`bfdorch.cpp`) — `(b)`**
    - **Vendor TD2+ `bcm_*` APIs**: `bcm_bfd_init()`, `bcm_bfd_endpoint_create()`, `bcm_bfd_endpoint_destroy()`, `bcm_bfd_event_register()`, `bcm_bfd_endpoint_stat_get()` (`include/bcm/bfd.h:583-672`). **Hardware caveat**: on TD2+ ESW, hardware BFD requires loading Broadcom's proprietary **uKernel (ARM Cortex-M3 / MCS) BFD firmware** (`bcm88xxx`/`td2_bfd.srec`) via `bcm_uc_*` / `soc_mcs_*`; without MCS firmware (typical in OpenBCM open-source builds), HW BFD is unavailable and SAI should report `SUPPORTED_IPV4/IPV6_BFD_SESSION_OFFLOAD_TYPE` = empty (`0`) and return `SAI_STATUS_NOT_SUPPORTED` so FRR uses software BFD!
    - **SONiC consequence**: `BfdOrch` silently creates SW-only BFD sessions that never transmit/receive packets and stay `Down` forever!
    - **Effort**: **S** (report `NOT_SUPPORTED` cleanly); **L** (requires closed-source MCS firmware, likely infeasible in OpenBCM).
    - **Testable on 1-switch + 1-PC lab**: **Yes** — verify capability rejection or software FRR BFD session with PC FRR/bfdd.

31. **NAT (`natorch.cpp`) — `(b)`**
    - **Vendor TD2+ `bcm_*` APIs**: `soc_feature_nat` is supported on TD2+ (`feature.c:6652`, requires `dc_enable`): `bcm_switch_control_set(unit, bcmSwitchL3NATEnable, 1)`, `bcm_l3_nat_ingress_add/delete/find()` (`include/bcm/nat.h:254-286`), `bcm_l3_nat_egress_add/delete/destroy()` (`include/bcm/nat.h:104-125`), `bcm_l3_nat_egress_stat_attach/counter_get()`, `bcm_l3_ingress_t.nat_realm_id`.
    - **SONiC consequence**: `NatOrch` queries `SAI_SWITCH_ATTR_AVAILABLE_SNAT_ENTRY` (if vslib returns >0, `gIsNatSupported` becomes true and NAT entries silently do nothing in HW). If NAT is not needed, return `0` for `AVAILABLE_SNAT_ENTRY`/`AVAILABLE_DNAT_ENTRY` and `NOT_SUPPORTED`.
    - **Effort**: **S** (to cleanly advertise unsupported via `AVAILABLE_SNAT_ENTRY = 0`), **L** (full HW NAT implementation).
    - **Testable on 1-switch + 1-PC lab**: **Yes** — route between inside VLAN and outside VLAN on PC 1G link and verify SNAT/DNAT/NAPT rewrite in `tcpdump`.

32. **MPLS (`mplsrouteorch.cpp`, `neighorch.cpp:242-251`) — `(b)/(c)`**
    - **Vendor TD2+ `bcm_*` APIs**: `soc_feature_mpls` supported on TD2+ (`feature.c:6614`): `bcm_mpls_init()`, `bcm_mpls_tunnel_switch_add/delete()` (`include/bcm/mpls.h:1191-1205`) for `INSEG_ENTRY` (pop/swap/PHP), `bcm_mpls_tunnel_initiator_set/clear()` (`include/bcm/mpls.h:1046-1064`) + `bcm_l3_egress_t.mpls_label` for `NEXT_HOP_TYPE_MPLS`, `bcm_l3_ingress_t.flags` (`BCM_L3_INGRESS_ROUTE_DISABLE_MPLS`) for `ADMIN_MPLS_STATE`.
    - **SONiC consequence**: MPLS label routes (`INSEG_ENTRY`) are silent no-ops; IP-over-MPLS routes (`TYPE_MPLS` NH) fail with `SAI_STATUS_FAILURE`.
    - **Effort**: **L** (full MPLS); **S** (return `NOT_SUPPORTED`).
    - **Testable on 1-switch + 1-PC lab**: **Yes** — PC crafts/receives MPLS labeled packets over 2 VLANs on the 1G data link via `scapy` / Linux `mpls_router`.

33. **NVGRE (`nvgreorch.cpp`) — `(b)`**
    - **Vendor TD2+ `bcm_*` APIs**: `soc_feature_l2gre` supported on TD2+ (`feature.c:6637`): `bcm_l2gre_init()`, `bcm_l2gre_vpn_create/destroy()`, `bcm_l2gre_tunnel_initiator_create()`, `bcm_l2gre_tunnel_terminator_create()`, `bcm_l2gre_port_add/delete()` (`include/bcm/l2gre.h`).
    - **SONiC consequence**: NVGRE tunnels silently do nothing in HW.
    - **Effort**: **L** (if needed); **S** (return `NOT_SUPPORTED`).
    - **Testable on 1-switch + 1-PC lab**: **Yes** — PC crafts GRE/NVGRE frames over the 1G link via `scapy` / Linux `gretap`.

34. **PBH / Policy-Based Hashing (`pbhorch.cpp`) — `(b)/(c)`**
    - **Vendor TD2+ `bcm_*` APIs**: Custom RTAG7 hash profiles (`bcm_switch_control_set` + `bcm_field_action_add(unit, entry, bcmFieldActionHashProfile, ...)` / `bcm_udf_*` for GRE key / inner ethertype); TD2+ has limited RTAG7 hash-bin profiles (2 blocks A/B, no arbitrary `FINE_GRAINED_HASH_FIELD` per-ACL-rule masking like TH3/TD4).
    - **SONiC consequence**: `PbhOrch` fine-grained hash fields are silent no-ops; PBH ACL table creation with `GRE_KEY` / `INNER_ETHER_TYPE` fails or is ignored.
    - **Effort**: **M-L** (partial RTAG7 profile support) or **S** (return `NOT_SUPPORTED`).
    - **Testable on 1-switch + 1-PC lab**: **Yes** — verify 2-path VLAN-SVI ECMP hash selection on GRE/IP-in-IP traffic from PC.

35. **SRv6 (`srv6orch.cpp`), TWAMP (`twamporch.cpp`), ICMP Echo Offload (`icmporch.cpp`) — `(b)/(c)` (TD2+ Silicon Limitations)**
    - **Vendor TD2+ `bcm_*` APIs**: **NONE on TD2+!** Verified in OpenBCM 6.5.27: `src/bcm/esw/srv6.c` and `src/bcm/esw/twamp.c` do not exist; `soc_feature_srv6_enable` is not in `soc_features_bcm56860_a0`; no ESW ICMP echo offload exists.
    - **SONiC consequence**: Because `SwitchStateBase::queryAttributeCapability()` (`SwitchStateBase.cpp:4165`) returns `true` for everything, `Srv6Orch` / `TwampOrch` / `IcmpOrch` believe these features are supported and silently create dead SW-only objects (or fail `(c)` on `TYPE_SRV6_SIDLIST` routes).
    - **Effort**: **S** (override `queryAttributeCapability` + return `SAI_STATUS_NOT_SUPPORTED` for `MY_SID_ENTRY`, `SRV6_SIDLIST`, `TWAMP_SESSION`, `ICMP_ECHO_SESSION`, `SAI_SWITCH_ATTR_MAX_TWAMP_SESSION`, `SAI_SWITCH_ATTR_ICMP_ECHO_SESSION_STATE_CHANGE_NOTIFY`).
    - **Testable on 1-switch + 1-PC lab**: **Yes** — verify clean `NOT_SUPPORTED` handling by orchagent.

---

## 3. Robustness Bugs in L3 & VXLAN Implementation

### Critical
1. **Guaranteed Self-Deadlock on LAG (PortChannel) Router Interface Create & Remove**
   - **Location**: `SwitchBCM56860HW_l3.cpp:520-528` (`hwCreateRouterInterface`) and `SwitchBCM56860HW_l3.cpp:746-754` (`hwRemoveRouterInterface`), calling `SwitchBCM56860HW.cpp:3394-3397` (`bcmPortOf`), with non-recursive `mutable std::mutex m_mapMutex` (`SwitchBCM56860HW.h:308`).
   - **Details**: Both functions acquire `std::lock_guard<std::mutex> lock(m_mapMutex);` and then call `bcmPortOf(poid)` inside the loop, which immediately acquires `std::lock_guard<std::mutex> lock(m_mapMutex);` on the same thread! Creating or removing an L3 interface on any PortChannel that already has >=1 member port **deadlocks syncd permanently**!
   - **Fix**: Copy the `m_lagMembers[target]` vector under lock (or look up `m_portToBcm` directly while holding `m_mapMutex`), release lock before calling `bcmPortOf()`.

2. **Destroying User VLANs 3000–3999 & Colliding Port-RIF VLAN Pool**
   - **Location**: `SwitchBCM56860HW_l3.cpp:35-36, 551-579` (create) and `SwitchBCM56860HW_l3.cpp:720-789` (remove).
   - **Details**:
     - On remove, line 726 checks `if (vid >= RIF_VID_MIN && vid <= RIF_VID_MAX)` instead of checking whether the RIF was `SAI_ROUTER_INTERFACE_TYPE_PORT`! If a user creates a normal L2 `Vlan3000` + SVI RIF (`TYPE_VLAN`), removing the SVI RIF calls `hwbcm_vlan_destroy(3000)` and **nukes the live user VLAN 3000 in HW**!
     - On create, lines 551-565 only check `m_rifToVid` for free VIDs in `3000..3999`, ignoring user VLANs in `m_objectHash[SAI_OBJECT_TYPE_VLAN]`. If `Vlan3000` exists, `hwbcm_vlan_create(3000)` fails with `BCM_E_EXISTS` and Port RIF creation fails!

3. **`TUNNEL_ENCAP` / `MPLS` / `SRV6_SIDLIST` Next Hop Silently Succeeds Without Egress -> Crashes/Fails All Routes & NHGs Referencing It**
   - **Location**: `SwitchBCM56860HW_l3.cpp:1139-1145` (`hwCreateNextHop`), `SwitchBCM56860HW_l3.cpp:1348-1352` (`hwCreateNextHopGroupMember`), `SwitchBCM56860HW_l3.cpp:1528-1536` (`hwCreateRoute`).
   - **Details**: `hwCreateNextHop` returns `SAI_STATUS_SUCCESS` claiming `"handled by vxlan side"`, but `_vxlan.cpp` has zero code for `SAI_OBJECT_TYPE_NEXT_HOP` and never inserts into `m_nhToEgr`. Subsequent route or ECMP member creation referencing that NH returns `SAI_STATUS_FAILURE`.

### High
4. **`set_neighbor_entry_attribute` Missing from `SwitchBCM56860HW::set()` -> Stale Neighbor MAC Blackhole**
   - **Location**: `SwitchBCM56860HW.cpp:3158-3189` (no `case SAI_OBJECT_TYPE_NEIGHBOR_ENTRY:`).
   - **Details**: Called by `neighorch.cpp:1212` whenever an existing neighbor's MAC changes; SW DB updates while ASIC egress keeps forwarding to the old MAC indefinitely.

5. **Dynamic MAC Move Short-Circuited -> Neighbor Egress Pinned to Old Port**
   - **Location**: `SwitchBCM56860HW.cpp:1710-1714` vs `SwitchBCM56860HW_l3.cpp:1716`.
   - **Details**: `process_packet_for_fdb_event` returns early if `fdbHash.find(sid) != fdbHash.end()` without checking if the learned port/trunk changed! Neither `SwitchBCM56850::process_packet_for_fdb_event` nor `hwRefreshNeighborsOnLearn()` runs on MAC move, contradicting the comment at `_l3.cpp:1716`.

6. **VXLAN Underlay Never Re-Evaluated on Route/Neighbor/ECMP Removal (`hwVxlanRetryPending()` Missing)**
   - **Location**: `SwitchBCM56860HW_l3.cpp:1013-1112` (`hwRemoveNeighbor`), `:1327-1445` (`hwCreate/RemoveNextHopGroupMember`), `:1652-1696` (`hwRemoveRoute`).
   - **Details**: When the underlay route, neighbor, or ECMP member to a remote VTEP is deleted, `hwVxlanRetryPending()` is never invoked. The VTEP's cloned `BCM_L3_VXLAN_ONLY` egress and network VP remain active pointing to the dead underlay path and continue reporting `OPER_UP`.

7. **VXLAN Underlay Lookup Ignores VRF (`vxlanResolveUnderlayEgress`)**
   - **Location**: `SwitchBCM56860HW_vxlan.cpp:142-232`.
   - **Details**: Scans `m_neighborEgr` (lines 152-176) and `m_routeEgr` (lines 182-218) across **all VRFs** without filtering by the underlay RIF's VRF (`re.vr_id` / `ne.rif_id`). A matching/more-specific tenant VRF prefix hijacks the VTEP's underlay egress!

8. **VXLAN Over Underlay or Access LAG Completely Broken (Missing Per-Member Port Controls)**
   - **Location**: `hwbcm_vxlan.c:904-919` (network VP on trunk) and `hwbcm_vxlan.c:1155-1160` (access VP on trunk).
   - **Details**:
     - When an underlay next hop is a LAG (`egr.flags & BCM_L3_TGID`), `hwbcm_vxlan_port_network_enable(port, 1)` (`bcmPortControlVxlanEnable`) is skipped for all LAG member ports -> **VXLAN decap fails on underlay LAGs**.
     - When an access VLAN member is a LAG (`trunk_id >= 0`), `hwbcm_vxlan_access_port_prepare(port)` (`bcmVlanTranslateIngressEnable`) is skipped for all LAG member ports -> **tagged access VP ingress lookup (`VLAN_XLATE`) fails on access LAGs**.

9. **SW Map / vslib DB / HW Desync on Remove Failure (Early Erase Before SDK Call)**
   - **Location**:
     - `SwitchBCM56860HW_l3.cpp:1073` (`hwRemoveNeighbor` erases `m_neighborEgr` and deletes host entry at `:1065` BEFORE `hwbcm_l3_egress_cpu_replace`/`:1088` or `hwbcm_l3_egress_destroy`/`:1100`).
     - `SwitchBCM56860HW_l3.cpp:1234` (`hwRemoveNextHop` erases `m_nhToEgr` BEFORE `hwbcm_l3_egress_destroy`/`:1242`).
     - `SwitchBCM56860HW_l3.cpp:1413-1414` (`hwRemoveNextHopGroupMember` erases member maps BEFORE `hwbcm_l3_ecmp_set`/`:1425`).
     - `SwitchBCM56860HW_l3.cpp:1687` (`hwRemoveRoute` erases `m_routeEgr` BEFORE checking `hwbcm_l3_route_delete`/`:1689`).
     - `SwitchBCM56860HW_vxlan.cpp:1369-1456` (`hwRemoveTunnelMapEntry` erases `m_mapEntryVidVni`, `m_vniRefs`, `m_vniToVpn`, `m_vidToVni` even when `hwbcm_vxlan_vpn_destroy` fails at `:1432-1437`).
     - `SwitchBCM56860HW.cpp:3135,3191-3200` (`set()` mutates vslib DB first and never rolls it back when `hwSet*` fails).
   - **Details**: Because `SwitchBCM56860HW::remove()` returns early on HW error (skipping `SwitchBCM56850::remove()`), the SAI object stays alive in vslib DB while the C++ HW maps have already deleted it, permanently leaking the HW resource and desyncing future operations.

10. **Table-Full (`BCM_E_FULL`) Always Mapped to `SAI_STATUS_FAILURE` Instead of `SAI_STATUS_TABLE_FULL`**
    - **Location**: All error checks in `SwitchBCM56860HW_l3.cpp` (`:323,618,916,974,996,1205,1374,1390,1558,1605`) and `SwitchBCM56860HW_vxlan.cpp` (`:1052,1311,1743`).
    - **Details**: `hwbcm_l3.c` returns negative `BCM_E_*` (e.g. `BCM_E_FULL = -14`), but C++ handlers convert every `< 0` unconditionally to `SAI_STATUS_FAILURE`. SONiC `handleSaiCreateStatus()` in `routeorch.cpp`/`neighorch.cpp` depends on `SAI_STATUS_TABLE_FULL` to handle `L3_DEFIP` / `L3_ENTRY` / ECMP exhaustion gracefully instead of aborting or misclassifying.

11. **Removing Last ECMP Member Leaves Stale Member Active in HW; Creating Route on Empty NHG Fails**
    - **Location**: `SwitchBCM56860HW_l3.cpp:1429-1435` (`hwRemoveNextHopGroupMember`) and `SwitchBCM56860HW_l3.cpp:1549-1559` (`hwCreateRoute`).
    - **Details**: When removing the last member of an ECMP group, `hwbcm_l3_ecmp_set(ecmp, data, 0)` fails on TD2+ and is ignored (`return SAI_STATUS_SUCCESS`), leaving the removed member still programmed in `L3_ECMP` in HW! Any route still pointing to that NHG keeps forwarding to the deleted next hop. Conversely, creating a route pointing to an empty NHG calls `hwbcm_l3_ecmp_create(nullptr, 0, &ecmp)` which fails on TD2+ instead of using a drop egress.

12. **`resolveVrf()` Silently Falls Back to VRF 0 (Default VRF) on Error -> Cross-VRF Route/RIF Leak**
    - **Location**: `SwitchBCM56860HW_l3.cpp:218-231`.
    - **Details**: If `hwbcm_l3_vrf_create` fails (e.g., >1023 VRFs) or if an unknown VR OID is passed to `hwRemoveRoute(..., allocate=false)`, `resolveVrf()` logs an error/warning and returns **VRF 0**! That installs a tenant VRF's RIF/neighbor/route into the default VRF, or deletes a default-VRF route when removing an unknown-VRF route!

13. **Multiple `NEXT_HOP`s Matching One Neighbor Leak / Corrupt Egress on Partial Failure or Remove**
    - **Location**: `SwitchBCM56860HW_l3.cpp:909-952, 1000, 1075-1097`.
    - **Details**:
      - If `action == SAI_PACKET_ACTION_DROP` (`:909`), `matchingNh` is skipped completely so existing NH egresses keep punting to CPU instead of dropping.
      - If >1 `NEXT_HOP` matches `(rif, ip)`, `:921-952` updates all of them, but only stores `matchingNh[0]` in `m_neighborEgr[serialized]` (`:948-951`). On `hwRemoveNeighbor`, `hwRefreshNeighborsOnLearn`, `hwPuntNeighborsOnAge`, or `hwReresolveNeighborsOnVlan`, `matchingNh[1..N]` are NEVER reverted/updated! Also no rollback if replace #2 fails after replace #1 succeeded.

### Medium
14. **Port RIF Creation Rollback Leaves Port PVIDs Pointing to Destroyed VLAN**
    - **Location**: `SwitchBCM56860HW_l3.cpp:583-618`.
    - **Details**: On failure in the member-port loop (`:587,596`) or in `hwbcm_l3_intf_create` (`:609`), rollback calls `hwbcm_vlan_destroy(vid)` without restoring `hwbcm_port_pvid_set(p, defVid)` for ports whose PVID was already changed to `vid`.

15. **User VLAN RIF Destroy Does Not Reset VLAN VRF / `ingress_if` / Disable Routing in HW**
    - **Location**: `hwbcm_l3.c:304-325` (`hwbcm_l3_intf_destroy`).
    - **Details**: Destroys `bcm_l3_ingress` and `bcm_l3_intf`, but never updates `bcm_vlan_control_vlan_set()` to clear `vc.vrf = 0`, `vc.ingress_if = 0`, and re-set `BCM_VLAN_IP4_DISABLE | BCM_VLAN_IP6_DISABLE`. After removing an SVI from a live L2 VLAN, the VLAN retains a dangling `ingress_if` (in RIOT mode) and stale `vrf`.

16. **RIOT Overlay L3 Interface MTU Not Updated on `set(ROUTER_INTERFACE_ATTR_MTU)`**
    - **Location**: `SwitchBCM56860HW_l3.cpp:651-663`.
    - **Details**: `hwSetRouterInterface` updates `it->second` (the VLAN L3 intf) via `hwbcm_l3_intf_mtu_set`, but does not update the RIOT overlay L3 intf (`hwbcm_vxlan_vpn_l3_intf(vpn)`) for VNI-mapped VLAN SVIs.

17. **`hwbcm_vxlan_network_port_add_impl` Leaks VFI Membership & Underlay `uvid` on Flood-Add Failure**
    - **Location**: `hwbcm_vxlan.c:1068-1073`.
    - **Details**: If `hwbcm_vxlan_vpn_flood_set(vpn_id, vp, 1)` fails (for `mc == 1`), error cleanup calls `bcm_vxlan_port_delete` and clears `s_vp[vp]`, without removing the VFI membership added at `:955` (`hwbcm_vxlan_vfi_member_set(..., 0)`) or the `bcm_vlan_gport_add(uvid, vp)` entry (in non-mode-3 builds).

18. **Remote VXLAN FDB Add/Remove Robustness Bugs (`hwFdbOnTunnel`)**
    - **Location**: `SwitchBCM56860HW_vxlan.cpp:1728-1744` (add) and `:1753-1763` (remove).
    - **Details**:
      - On add: `vtep.macs[key] = m` is inserted BEFORE `hwbcm_vxlan_l2_addr_add`; if HW add fails, `SwitchBCM56860HW::create` rolls back vslib DB, but `vtep.macs[key]` keeps the ghost MAC and reinstalls it on future VTEP bringup.
      - On remove: `hwReresolveNeighborsOnVlan(vid)` is NOT called! Any RIOT SVI neighbor pointing to that remote MAC's overlay egress stays pointing to the remote VTEP VP instead of reverting to CPU punt.

19. **P2P VXLAN Tunnels Ignore `m_vtepTtl` / `m_vtepDscp` Stored on P2MP Local VTEP Tunnel**
    - **Location**: `SwitchBCM56860HW_vxlan.cpp:962-999, 1040`.
    - **Details**: `m_vtepTtl` and `m_vtepDscp` are saved at `:998-999` on P2MP tunnel create, but never used as defaults at `:962,972` when P2P tunnels are created without per-P2P TTL/DSCP attributes.

20. **`hwCreateTunnelMapEntry` Allows Silently Colliding Two Different VLANs onto the Same VNI**
    - **Location**: `SwitchBCM56860HW_vxlan.cpp:1282-1301`.
    - **Details**: When `m_vniToVpn.count(vni)` is already true (`:1291`), it assumes it is the reverse-direction map entry for the same VLAN without checking that `vid` matches the existing VLAN of `vni`. A misconfigured second VLAN mapped to an existing VNI silently increments `m_vniRefs[vni]` and returns `SUCCESS` without programming HW.

21. **VXLAN Underlay ECMP Only Uses First Member (`members[0]`)**
    - **Location**: `hwbcm_vxlan.c:586-605` (`hwbcm_vxlan_egress_source_get`).
    - **Details**: Clones only `members[0]` of an ECMP group into `BCM_L3_VXLAN_ONLY` egress; no underlay ECMP load-balancing for VXLAN traffic, and combined with Bug #6, if `members[0]` is removed from the ECMP group, the tunnel keeps blackholing until some unrelated trigger runs `hwVxlanRetryPending()`.

22. **`hwbcm_vxlan_init` vs `hwbcm_l3_lazy_init` Ordering Hazard for RIOT L3 Ingress Mode**
    - **Location**: `hwbcm_vxlan.c:239, 1520` vs `hwbcm_l3.c:72-85`.
    - **Details**: `hwbcm_l3_lazy_init` enables `bcmSwitchL3IngressMode = 1` when `hwbcm_riot_enabled()`, but `hwbcm_vxlan_init` only sets `bcmSwitchL3EgressMode = 1`. If `hwbcm_vxlan_vpn_l3_bind` were ever called before `hwbcm_l3_lazy_init`, it would fail with `BCM_E_UNAVAIL`.

23. **Unchecked Return Values / Resource Leaks on Teardown**
    - **Location**:
      - `SwitchBCM56860HW_l3.cpp:40-41` (`g_cpuEgr`, `g_dropEgr` globals never destroyed or reset on cold switch destroy/recreate).
      - `SwitchBCM56860HW_l3.cpp:775-776` (`hwbcm_port_pvid_set`, `hwbcm_vlan_port_remove` return values unchecked in `hwRemoveRouterInterface`).
      - `SwitchBCM56860HW_vxlan.cpp:127` (`m_vxlanStationId` created in `vxlanEnsureInit`, `hwbcm_vxlan_station_delete` never called on teardown).
      - `hwbcm_l3.c:117-142, 782-788` (`s_ecmp_ids` heap array never freed on reset).
      - `hwbcm_vxlan.c:1552-1556` (`bcm_l2_addr_add` failure for RIOT router MAC -> CPU is only warned, marking `s_vpn_l3[vfi].bound = 1` even though ARP replies to the SVI gateway will be lost).

### Low
24. **O(FDB * Neighbors) Scaling Bottleneck on Main SAI API Thread**
    - **Location**: `SwitchBCM56860HW_l3.cpp:1919-1970` (`neighborTarget` deserializes every FDB entry linearly), called inside `hwReresolveNeighborsOnVlan` (`:1983-2037`), plus linear scan of `m_nhToEgr` with string IP serialization in `hwCreateNeighbor` (`:886-907`), and linear scan of `s_ecmp_ids` in `hwbcm_l3.c:157-167`.
    - **Details**: Holds global SAI API mutex while doing O(N*M) string deserialization on every VLAN/VXLAN/FDB bulk event.

25. **Uncaught `sai_deserialize_*` Exceptions in `SwitchBCM56860HW_l3.cpp`**
    - **Location**: `SwitchBCM56860HW_l3.cpp:806, 1029, 1457, 1666, 1724, 1800, 1922, 1986`.
    - **Details**: Unlike `_vxlan.cpp:158-167`, `_l3.cpp` does not guard `sai_deserialize_*` with `try/catch`; a malformed serialized key would abort `syncd`.

26. **Unchecked Non-Locking Access to `m_lagMembers` Across Files**
    - **Location**: `SwitchBCM56860HW.cpp:2731, 2875` writes `m_lagMembers` without `m_mapMutex`, while `SwitchBCM56860HW_l3.cpp:520, 746` locks `m_mapMutex` to read `m_lagMembers` (causing Bug #1 deadlock while providing zero actual protection if any other thread reads `m_lagMembers`; note the link thread only reads `m_bcmToPort`/`m_bcmIfName`, not `m_lagMembers`).

27. **Inconsistent Default RIF MTU & Unchecked `HWBCM_VXLAN_MAX_VFI` Masking**
    - **Location**: `SwitchBCM56860HW_l3.cpp:450` (defaults to `1514`) vs `SwitchBCM56860HW_l3.cpp:2119` (defaults to `9100`); `hwbcm_vxlan.c:129,1446` (`HWBCM_VXLAN_MAX_VFI` is `8192` while `hwbcm_vxlan_vfi_of()` masks `vpn_id & 0xfff`, max 4095).

*(Domain 2 audit complete.)*
