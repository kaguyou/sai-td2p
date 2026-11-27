# Domain 6 Audit Report: Cross-Cutting Robustness, Concurrency, Error-Handling, Table-Full & Crash Audit (`SwitchBCM56860HW` + `hwbcm`)

## Executive Summary — Top Critical & High Findings

| # | Cat | Severity | Location | Summary |
|---|---|---|---|---|
| 1 | 5/6 | **Critical** | `SwitchBCM56860HW_l3.cpp:520-528, 746-754` | **Permanent self-deadlock of `syncd`** on non-recursive `m_mapMutex` when creating/removing a PORT-type RIF on a LAG with >=1 member |
| 2 | 4 | **Critical** | All `SwitchBCM56860HW*.cpp` (`SAI_STATUS_TABLE_FULL` = 0 occurrences) | `BCM_E_FULL` (-6) / `BCM_E_RESOURCE` (-14) on L3_DEFIP/ALPM, L3_ENTRY, L3_NEXT_HOP, L3_ECMP, L2X, VXLAN return `SAI_STATUS_FAILURE` -> triggers `handleSaiFailure()` in `orchagent` (`gOrchUnhealthy=true`, syncd dump, permanent task drop instead of retry) |
| 3 | 2 | **Critical** | `SwitchBCM56860HW.cpp:3135, 3191-3200` | `SwitchBCM56860HW::set()` updates `m_objectHash` BEFORE `hwSet*` and NEVER rolls back on HW failure, permanently desynchronizing SAI DB, `get()`, warm-boot saves, and ASIC |
| 4 | 2/3 | **Critical** | `SwitchBCM56860HW.cpp:2906, 2952-2954, 3007-3031` | `SwitchBCM56860HW::create()` rollback on `hwAclBindOnCreate` / `hwCoppOnTrapCreate` failure calls `SwitchBCM56850::remove()` only — NEVER calls `hwRemove*`, leaking ASIC objects & C++ shadow maps |
| 5 | 5/6 | **Critical** | `hwbcm/hwbcm_core.c:556-620, 710-762, 804-837` | Diagnostic shell socket thread hijacks process-wide fd 1/2 via `dup2(client, 1/2)` (`SIGPIPE` kills `syncd` on early client disconnect since `syncd` does not ignore `SIGPIPE`), calls `hwbcm_vxlan_*` & SDK diag commands without `m_apimutex`/VXLAN lock, and uses detached thread with closed-fd `accept()` race |
| 6 | 3 | **High** | `SwitchBCM56860HW_l3.cpp:551-579, 726-786` | `hwRemoveRouterInterface` destroys user VLANs in `3000..3999` because it checks `vid` range without checking `TYPE == PORT`; `hwCreateRouterInterface` does not check `m_objectHash[VLAN]` when allocating port-RIF VLANs |
| 7 | 2/3 | **High** | `SwitchBCM56860HW.cpp:2873-2885`, `SwitchBCM56860HW_l3.cpp:1073-1106, 1234-1248, 1293-1322, 1413-1440, 1685-1693`, `SwitchBCM56860HW_vxlan.cpp:1369-1456, 1728-1744`, `SwitchBCM56860HW_acl.cpp:1166-1214, 1477-1501, 1947-1977, 2056-2072`, `SwitchBCM56860HW_qos.cpp:934-972, 2141-2150, 2844-2890` | Systematic pattern of erasing/mutating C++ shadow maps BEFORE HW delete/modify without rollback on failure -> permanent HW leaks and retry no-ops |
| 8 | 5 | **High** | `hwbcm/hwbcm_core.c:237-244, 481-520, 2774, 2824-2825`, `hwbcm_core.c:122-125, 256-257, 524-535`, `hwbcm_l2.c:377-443` | Unlocked cross-thread data races between SDK `bcmLINK`/`bcmL2X` threads and SAI API thread (`g_desired_speed`, `g_sgmii_applied`, `g_sgmii_applied_at`, `g_link_cb`/`cookie`, `g_l2_cb`/`cookie`, `setvbuf(stderr)` UB) |
| 9 | 6 | **High** | `hwbcm/hwbcm_vxlan.c:1444-1449`, `hwbcm/hwbcm_qos.c:88-91, 613, 867, 878, 1010-1020` | Negative `vpn_id` (`-1 & 0xfff == 4095`) bypasses `hwbcm_vxlan_vfi_of` bounds check and aliases `s_vpn_l3[4095]`; 32-bit unsigned wrap in `qos_cells()` & signed overflow in `bcmCosqControl*Bytes` / `qos_kbits()` |
| 10 | 1 | **High** | `hwbcm/hwbcm_l2.c:126-127, 166`, `SwitchBCM56860HW_l3.cpp:590, 599, 615` | Dangerous unchecked returns on `vlan_stg_forward`, `hwbcm_vlan_cpu_add`, and error-path `hwbcm_vlan_destroy` without port/PVID cleanup |

---

## Category 1: Unchecked `bcm_*` / `soc_*` / `hwbcm_*` Return Values

### 1.1 STP FORWARD & CPU VLAN Membership Failures Silently Ignored
- **Location**: `hwbcm/hwbcm_l2.c:126-127, 166`
- **Severity**: **High**
- **Mechanism**: `hwbcm_vlan_create()` calls `(void)vlan_stg_forward(vid, -1);` and `(void)hwbcm_vlan_cpu_add(vid);`; `hwbcm_vlan_port_add()` calls `(void)vlan_stg_forward(vid, port);`. The file's own comments note that failing STP FORWARD drops all ingress traffic on the VLAN/port and failing CPU VLAN membership drops all ARP/IP2ME/control-plane traps, yet failures are discarded and `0` is returned.
- **Impact**: Silent blackholing of L2/L3 traffic or control-plane punts on newly created VLANs/ports while SONiC believes creation succeeded.
- **Fix**: Check return values of `vlan_stg_forward` and `hwbcm_vlan_cpu_add`; on failure log error, roll back `bcm_vlan_destroy` / `bcm_vlan_port_remove`, and return `rv`.

### 1.2 Unchecked `hwbcm_vlan_destroy` / PVID Cleanup in Port-RIF Error & Remove Paths
- **Location**: `SwitchBCM56860HW_l3.cpp:590, 599, 615` (error paths) and `775-776` (`hwRemoveRouterInterface`)
- **Severity**: **High** (590/599/615), **Medium** (775-776)
- **Mechanism**: In `hwCreateRouterInterface`, lines 581-583 add member ports and set PVID to internal RIF VLAN `vid`. If `hwbcm_port_pvid_set` (line 590), `hwbcm_l3_intf_create` (line 599), or `hwbcm_l3_vrf_create` (line 615) fails, rollback calls bare `hwbcm_vlan_destroy(vid)` unchecked WITHOUT removing ports from `vid` or restoring their PVID to default VLAN 1! In `hwRemoveRouterInterface`, `hwbcm_port_pvid_set(p, defVid)` and `hwbcm_vlan_port_remove(vid, p)` at lines 775-776 are unchecked before `hwbcm_vlan_destroy(vid)`.
- **Impact**: `bcm_vlan_destroy` fails with `BCM_E_BUSY`/leaves ports with dangling PVID pointing to a destroyed internal VLAN (3000..3999), silently dropping untagged ingress frames on that port.
- **Fix**: In rollback helper, iterate added ports, restore PVID to `defVid` and call `hwbcm_vlan_port_remove(vid, p)` (logging errors) before `hwbcm_vlan_destroy(vid)`.

### 1.3 KNET PVID Filter Re-Create Failure Silently Leaves VLAN Tags Unstripped
- **Location**: `hwbcm/hwbcm_core.c:2943` (and `3341`)
- **Severity**: **Medium**
- **Mechanism**: `hwbcm_port_pvid_set()` destroys the old KNET PVID filter (`knet_filter_destroy_quiet` at line 2938), then calls `(void)knet_pvid_filter_create(HWBCM_UNIT, slot, vid);` at line 2943. If filter creation fails (e.g., KNET filter table full), the old filter is already gone and `0` is returned. Similarly `(void)knet_vfi_filter_create(...)` at line 3341 is unchecked.
- **Impact**: Packets punted from that port arrive in Linux with an unexpected 802.1Q tag (PVID) and are dropped by the Linux network stack (breaking ARP/BGP/LACP on routed/access ports).
- **Fix**: Check return value of `knet_pvid_filter_create` / `knet_vfi_filter_create` and log `HWBCM_LOGE` + propagate error (or restore previous filter on failure).

### 1.4 CoPP Intentionally Swallows HW Programming Errors and Returns `SAI_STATUS_SUCCESS`
- **Location**: `SwitchBCM56860HW_copp.cpp:415-420, 523-530, 681, 751-756, 763-768`
- **Severity**: **Medium**
- **Mechanism**: `coppProgramTrap` (`415-420`) returns `SAI_STATUS_SUCCESS` even when `hwbcm_copp_trap_apply` returns `< 0`; `hwCreatePolicer` (`523-530`) and `hwSetPolicer` (`681`) log and return `SAI_STATUS_SUCCESS`; `hwCreateHostifTrap` (`751-756`) and `hwSetHostifTrap` (`763-768`) swallow `hwbcm_trap_set` failure and return `SAI_STATUS_SUCCESS`.
- **Impact**: Control-plane traps run unpoliced or with wrong CPU queue / dropped packets while SONiC `copporch` believes CoPP is programmed.
- **Fix**: Translate `rv` (returning `SAI_STATUS_INSUFFICIENT_RESOURCES` on `-6`/`-14`) or distinguish unsupported trap types explicitly from real HW failures (`BCM_E_FULL`/`BCM_E_RESOURCE`).

### 1.5 Unchecked VXLAN Cleanup & Flood Updates
- **Location**: `SwitchBCM56860HW_vxlan.cpp:376, 407, 524, 534, 546, 574, 1419`
- **Severity**: **Medium**
- **Mechanism**: Bare unchecked calls to `hwbcm_vxlan_egress_destroy`, `hwbcm_vxlan_l2_addr_delete`, and `hwbcm_vxlan_vpn_flood_set`. At line 1419 (`hwRemoveTunnelMapEntry`), if `hwbcm_vxlan_vpn_flood_set(vpn, vtep.vpMc, 0)` fails, `vtep.floodVpns.erase(vpn)` still executes.
- **Impact**: Stale multicast replication entries / leaked egress objects in ASIC while C++ shadow maps think they are removed.
- **Fix**: Check return codes; at minimum log `SWSS_LOG_ERROR` on non-`NOT_FOUND` errors and avoid clearing tracking state on `BCM_E_BUSY`.

### 1.6 Unchecked ACL Refresh Calls on Dynamic Topology Changes
- **Location**: `SwitchBCM56860HW_acl.cpp:2124, 2156, 2200`
- **Severity**: **Medium**
- **Mechanism**: `hwAclObjectRemoved`, `hwAclLagMembersChanged`, and `hwAclMirrorSessionUpdated` ignore the `sai_status_t` returned by `aclRefreshTable` / `aclProgramEntry`.
- **Impact**: Adding a member to an ACL-bound LAG or updating a mirror session can silently fail TCAM expansion, leaving the new LAG member unfiltered by ingress/egress ACLs.
- **Fix**: Log `SWSS_LOG_ERROR` with table/entry OID on non-success and propagate error where caller (`hwCreateLagMember`/`hwSetMirrorSession`) can fail or roll back.

### 1.7 Other Unchecked Calls (Lower Risk / Init / Warm-Restore)
- `SwitchBCM56860HW.cpp:286, 334` (**Medium**): `(void)hwbcm_stats_init();` and `(void)qosBuildHwMap(...)` unchecked during switch init.
- `SwitchBCM56860HW_wb.inc.cpp:1957, 1971, 2002` (**Medium**): `(void)hwbcm_l3_warm_restore`, `(void)hwbcm_vxlan_warm_restore`, `(void)hwbcm_copp_warm_restore` ignore error returns — partial warm restore continues silently with inconsistent C tables.
- `SwitchBCM56860HW.cpp:638, 752, 769, 821, 831, 841, 1680` (**Low**): `hwbcm_port_name` unchecked (uninitialized `pname` if it failed, though `pname[0]='\0'` is initialized at line 637); `set_internal` unchecked in `buildPortMaps`; `hwbcm_l2_addr_delete` unchecked in `process_packet_for_fdb_event` when dropping unknown port.
- `hwbcm/hwbcm_core.c:2943, 3594`, `hwbcm/hwbcm_l2.c:268` (**Low**): `(void)bcm_stk_my_modid_get(...)` unchecked (defaults to modid 0 on single-chip TD2+, harmless in practice).
- *Harmless/intentional cleanup ignores verified*: `hwbcm_acl.c:646-649, 710`, `hwbcm_copp.c:939, 955-956`, `hwbcm_qos.c:191, 224, 258`, `hwbcm_l3.c:285, 292, 294, 533`, `hwbcm_core.c:391-399, 2413, 2548-2567` (teardown/error-unwind paths).

---

## Category 2: Error Paths Leaving SAI DB (`m_objectHash`) & HW (ASIC + Shadow Maps) Out of Sync

### 2.1 `SwitchBCM56860HW::set()` NEVER Rolls Back `m_objectHash` on HW Failure
- **Location**: `SwitchBCM56860HW.cpp:3135, 3191-3200`
- **Severity**: **Critical**
- **Mechanism**: `set()` unconditionally executes `SwitchBCM56850::set(objectType, serializedObjectId, attr)` at line 3135 FIRST, then dispatches to `hwSet*`. If `hwSet*` returns an error, lines 3191-3200 log `"(virtual state already updated)"` and return the error WITHOUT restoring the previous attribute value in `m_objectHash`.
- **Impact**: Every failed `set()` (port speed/MTU/admin, route nexthop change, ACL entry/bind, QoS map/profile, mirror session, VLAN/bridge port) corrupts `m_objectHash`. Subsequent `get()` returns the failed value, warm-boot saves persist the unapplied value, and any `hwSet*` that re-reads `m_objectHash` (ACL, QoS, CoPP, Mirror, Route) programs the wrong attribute on later updates.
- **Fix**: Snapshot the old attribute (existence + `SaiAttrWrap` shared_ptr) before calling `SwitchBCM56850::set()`; if `hwStatus != SAI_STATUS_SUCCESS`, restore the old `SaiAttrWrap` (or erase the attribute key if newly inserted) in `m_objectHash`.

### 2.2 `SwitchBCM56860HW::create()` Leaks HW & Shadow Maps When Post-Create Hook Fails
- **Location**: `SwitchBCM56860HW.cpp:2906, 2952-2954, 3007-3031`
- **Severity**: **Critical**
- **Mechanism**: `create()` calls `SwitchBCM56850::create()` (2906), then `hwCreate*`, then for PORT/LAG/VLAN/RIF calls `hwAclBindOnCreate()` (3007-3015) and for HOSTIF_TRAP calls `hwCoppOnTrapCreate()` (2952-2954). If `hwAclBindOnCreate` or `hwCoppOnTrapCreate` fails AFTER `hwCreate*` succeeded, lines 3022-3029 ONLY call `SwitchBCM56850::remove()` — they NEVER call `hwRemove*`!
- **Impact**: Permanently leaks ASIC trunk (`bcm_trunk_create`), VLAN (`bcm_vlan_create`), L3 intf+IIF+dedicated VLAN (`bcm_l3_intf_create` + `bcm_l3_ingress_create`), port mapping, AND leaves orphaned entries in `m_lagToTrunk`/`m_rifToIntf`/`m_rifToVid`/`m_portToBcm` pointing at a removed SAI OID.
- **Fix**: Track `bool hwCreated = (hwStatus == SAI_STATUS_SUCCESS);` before post-create hooks; if a post-create hook fails, invoke the matching `hwRemove*` before `SwitchBCM56850::remove()`.

### 2.3 `SwitchBCM56860HW::remove()` Strips ACLs Before `hwRemove*` Succeeds
- **Location**: `SwitchBCM56860HW.cpp:3055-3125`
- **Severity**: **High**
- **Mechanism**: Line 3055 calls `hwAclObjectRemoved(oid)` (which erases `m_aclBindings` and deletes/reprograms IFP/EFP rules in HW) BEFORE `hwRemove*` runs. If `hwRemove*` fails (e.g., `BCM_E_BUSY` removing a referenced RIF/VLAN/LAG), `SwitchBCM56850::remove()` is skipped, returning failure to SONiC.
- **Impact**: Object remains alive in SONiC and ASIC, but all ACL rules bound to it have already been stripped from HW and `m_aclBindings`.
- **Fix**: Run `hwRemove*` first (or detach ACLs only after `hwRemove*` succeeds; for ports/LAGs/RIFs/VLANs, ACLs only reference port bitmaps/VIDs and do not block `hwRemove*`).

### 2.4 Shadow Maps Erased/Mutated BEFORE HW Call Fails (Widespread Pattern)
- **Severity**: **High**
- **Locations & Mechanisms**:
  1. `SwitchBCM56860HW.cpp:2873-2885` (`hwRemoveLagMember`): erases `m_lagMemberInfo` & `m_lagMembers` BEFORE `applyTrunkMembers(lag)`. On failure, DB & ASIC still have member, shadow map does not; next retry silently returns `SAI_STATUS_SUCCESS` as no-op!
  2. `SwitchBCM56860HW_l3.cpp:1073-1106` (`hwRemoveNeighbor`): deletes host entry AND erases `m_neighborEgr` BEFORE `hwbcm_l3_egress_cpu_replace` / `hwbcm_l3_egress_destroy`. If destroy fails (`BCM_E_BUSY`), neighbor stays in DB while host route is deleted and egress is permanently leaked on retry.
  3. `SwitchBCM56860HW_l3.cpp:1234-1248` (`hwRemoveNextHop`): erases `m_nhToEgr` BEFORE `hwbcm_l3_egress_destroy` -> permanent `L3_NEXT_HOP` leak on `BCM_E_BUSY` + retry.
  4. `SwitchBCM56860HW_l3.cpp:1293-1322` (`hwRemoveNextHopGroup`): erases `m_nhgMemberToNh*` BEFORE `hwbcm_l3_ecmp_destroy`.
  5. `SwitchBCM56860HW_l3.cpp:1413-1440` (`hwRemoveNextHopGroupMember`): erases `m_nhgMemberToNhg`/`m_nhgMemberToNh` BEFORE `hwbcm_l3_ecmp_set`.
  6. `SwitchBCM56860HW_l3.cpp:1685-1693` (`hwRemoveRoute`): erases `m_routeEgr` BEFORE checking `hwbcm_l3_route_delete` result.
  7. `SwitchBCM56860HW_vxlan.cpp:1369-1456` (`hwRemoveTunnelMapEntry`): erases `m_mapEntryVidVni`, `m_vniRefs`, tears down VTEPs & access VPs, and erases `m_vniToVpn`/`m_vidToVni` even if `hwbcm_vxlan_vpn_destroy` fails.
  8. `SwitchBCM56860HW_vxlan.cpp:1728-1744` (`hwFdbOnTunnel`): inserts `vtep.macs[key]` BEFORE `hwbcm_vxlan_l2_addr_add`; does NOT erase on failure -> phantom MAC re-programmed on next VTEP rebuild.
  9. `SwitchBCM56860HW_acl.cpp:1477-1501` (`hwRemoveAclTable`), `1947-1951` (`hwCreateAclTableGroupMember`), `1972-1977` (`hwRemoveAclTableGroupMember`), `2056-2072` (`hwAclBind`): mutate shadow maps / delete entries before group destroy or `aclRefreshTable` without rollback.
  10. `SwitchBCM56860HW_qos.cpp:934-972, 1023, 1043, 2141-2150, 2844-2855, 2881-2890`: mutate `m_qosPort` / `m_qosPools` / `m_qosPgPending` before HW calls without rollback.
  11. `SwitchBCM56860HW_mirror.cpp:756-814` (`hwRemoveMirrorSession`) & `673-677` (`hwSetMirrorSession`): detaches all port & ACL mirrors BEFORE `hwbcm_mirror_dest_destroy`; on double failure during `hwSetMirrorSession`, sets `destGport = -1` while `m_port*Mirrors` still reference the session.
- **Impact**: Retries from SONiC `orchagent` silently no-op or fail differently while HW resources leak permanently or traffic diverges from ASIC_DB.
- **Fix**: Only erase/commit C++ shadow maps AFTER the corresponding `hwbcm_*` HW operation returns `0` (or restore saved entry on failure).

### 2.5 Partial Multi-Step HW Programming Without Rollback
- **Severity**: **High**
- **Locations & Mechanisms**:
  1. `SwitchBCM56860HW_l3.cpp:921-997` (`hwCreateNeighbor`): rewrites existing `matchingNh` CPU egresses in-place to the new MAC (`941-953`), then calls `hwbcm_l3_host_add` (`987`). If rewriting NH `k` fails or `hwbcm_l3_host_add` fails (`BCM_E_FULL`), already-rewritten NHs `0..k-1` are NOT reverted back to CPU punt!
  2. `SwitchBCM56860HW_l3.cpp:227-231` (`resolveVrf`): when `hwbcm_l3_vrf_create` fails (VRF table full), SILENTLY falls back to VRF `0` (default VRF) instead of returning an error — **breaking tenant VRF isolation**!
  3. `SwitchBCM56860HW_acl.cpp:1166-1214` (`aclProgramEntry`): when `newCounter == E.counter`, replaces variants one-by-one; if variant `i` fails (`BCM_E_FULL`), variants `0..i-1` are already installed in HW while `i..N-1` are missing, AND it retires old variants `>= i` and overwrites `E.hw`!
  4. `SwitchBCM56860HW.cpp:881-893` (`hwCreatePort`): calls `qosOnPortMapped` before the `hwSetPort` attribute loop; on failure calls `unmapPort` only, skipping `qosOnPortRemoved` / `mirrorOnPortRemoved`.
  5. `SwitchBCM56860HW.cpp:2331-2347, 2357-2419` (`hwCreateVlanMember` / `hwRemoveVlanMember`): LAG member VLAN add/remove loops stop mid-way without undoing earlier members or VXLAN access VPs.
  6. `hwbcm/hwbcm_l3.c:312-322` (`hwbcm_l3_intf_destroy`): calls `bcm_l3_ingress_destroy` BEFORE `bcm_l3_intf_delete`; if `intf_delete` fails (`BCM_E_BUSY`), the ingress interface (IIF) is already destroyed.
  7. `hwbcm/hwbcm_qos.c:283-290, 572-619`: zeroes/clears old ingress QoS map or PG buffer limits BEFORE programming new ones; failure mid-way leaves port/PG zeroed in HW.
  8. `hwbcm/hwbcm_vxlan.c:337-344, 1284-1301`: unbinds L3, flushes L2, and clears `s_vp[]` bookkeeping even if `bcm_vxlan_vpn_destroy` / `bcm_vxlan_port_delete` fails.
- **Fix**: Return error from `resolveVrf` instead of falling back to VRF 0; build all new ACL variants before removing old ones; reverse order in `hwbcm_l3_intf_destroy` (or check refs first); unwind earlier loop iterations on partial failure.

### 2.6 Missing `SAI_OBJECT_TYPE_NEIGHBOR_ENTRY` in `SwitchBCM56860HW::set()` Dispatch
- **Location**: `SwitchBCM56860HW.cpp:3140-3189`
- **Severity**: **Medium**
- **Mechanism**: `set()` handles `ROUTE_ENTRY`, `FDB_ENTRY`, `PORT`, `SWITCH`, `VLAN`, `BRIDGE_PORT`, etc., but has NO case for `SAI_OBJECT_TYPE_NEIGHBOR_ENTRY` (no `hwSetNeighbor`). Calling `set_neighbor_entry_attribute` (e.g., MAC change or `PACKET_ACTION` change) updates `m_objectHash` and returns `SAI_STATUS_SUCCESS` without touching ASIC `L3_NEXT_HOP` or `L3_ENTRY`.
- **Impact**: Stale neighbor MAC / packet action in ASIC if `neighorch` updates a neighbor attribute in place.
- **Fix**: Add `SAI_OBJECT_TYPE_NEIGHBOR_ENTRY` case in `SwitchBCM56860HW::set()` dispatching to a `hwSetNeighbor()` that updates the egress object / host entry.

---

## Category 3: Resource Leaks on Remove / Error / Re-Programming

### 3.1 Port-RIF VLAN Range (`3000..3999`) Destroys & Collides with User VLANs
- **Location**: `SwitchBCM56860HW_l3.cpp:551-579, 726-786`
- **Severity**: **High**
- **Mechanism**:
  - `hwRemoveRouterInterface` (line 726) checks `if (vid >= RIF_VID_MIN && vid <= RIF_VID_MAX)` WITHOUT verifying the RIF's `SAI_ROUTER_INTERFACE_ATTR_TYPE == SAI_ROUTER_INTERFACE_TYPE_PORT`! Removing an SVI (`VLAN`-type RIF) on any user VLAN 3000..3999 resets all its ports' PVIDs and executes `hwbcm_vlan_destroy(vid)` on the live user VLAN!
  - `hwCreateRouterInterface` (line 553) picks an unused internal port-RIF VLAN in `3000..3999` by checking only `m_rifToVid`, NEVER checking `m_objectHash[SAI_OBJECT_TYPE_VLAN]` — colliding with existing user VLANs in `3000..3999`.
- **Impact**: Destroying an SVI on VLAN 3000..3999 wipes out the customer VLAN in ASIC; creating a port RIF hijacks an active user VLAN 3000..3999.
- **Fix**: Check `type == SAI_ROUTER_INTERFACE_TYPE_PORT` (or store a dedicated `m_portRifVids` set) before destroying `vid`, and check `m_objectHash[SAI_OBJECT_TYPE_VLAN]` when allocating an internal port-RIF VLAN (or allocate from 4094 down excluding user VLANs).

### 3.2 KNET VFI Filter Leaked on Netif Create Error Path
- **Location**: `hwbcm/hwbcm_core.c:3340-3361`
- **Severity**: **Medium**
- **Mechanism**: When `riot_mode` is enabled, line 3341 allocates `slot->vfi_filter_id` via `knet_vfi_filter_create()`. If Filter (b) fails at line 3355, lines 3357-3358 destroy `slot->pvid_filter_id` and `netif.id`, but NEVER call `knet_filter_destroy_quiet(&slot->vfi_filter_id)`.
- **Impact**: Leaks a kernel KNET filter slot per failed `hostif` creation.
- **Fix**: Add `knet_filter_destroy_quiet(&slot->vfi_filter_id);` next to line 3357.

### 3.3 VXLAN `MY_STATION` Entry Never Deleted on VTEP Removal
- **Location**: `SwitchBCM56860HW_vxlan.cpp:119-128, 1090-1097`
- **Severity**: **Medium**
- **Mechanism**: `m_vxlanStationId` allocated by `hwbcm_vxlan_station_add()` is never freed via `hwbcm_vxlan_station_delete(m_vxlanStationId)` when the local VTEP tunnel (`m_vtepTunnel`) is removed or recreated.
- **Impact**: Leaks a `MY_STATION_TCAM` entry every time the local VTEP tunnel is torn down and recreated.
- **Fix**: In `hwRemoveTunnel` when `tunnel_id == m_vtepTunnel`, call `hwbcm_vxlan_station_delete(m_vxlanStationId)` and reset `m_vxlanStationId = -1`.

### 3.4 VXLAN VFI Membership Leaked When Network VP Flood Add Fails
- **Location**: `hwbcm/hwbcm_vxlan.c:955, 1068-1073`
- **Severity**: **Medium**
- **Mechanism**: Line 955 calls `hwbcm_vxlan_vfi_member_set(vpn_id, port_gport, 1)` for a network VP. If `hwbcm_vxlan_vpn_flood_set` fails at line 1068, line 1070 deletes the VP (`bcm_vxlan_port_delete`), but never undoes `hwbcm_vxlan_vfi_member_set(..., 0)`.
- **Impact**: Stale VFI port-bitmap membership remains in HW.
- **Fix**: In the line 1068 error path, call `hwbcm_vxlan_vfi_member_set(vpn_id, port_gport, 0)` if `hwbcm_vxlan_vfi_member_count(...) == 0`.

### 3.5 DROP Neighbor Allocates Dedicated `L3_NEXT_HOP` Instead of Sharing `g_dropEgr`
- **Location**: `SwitchBCM56860HW_l3.cpp:909-917`
- **Severity**: **Medium**
- **Mechanism**: Unlike routes (which share `g_dropEgr`), `hwCreateNeighbor` with `SAI_PACKET_ACTION_DROP` calls `hwbcm_l3_egress_drop_create` per neighbor (allocating a new `L3_NEXT_HOP` entry for every drop neighbor) and skips updating any `matchingNh` egresses.
- **Impact**: Unnecessarily consumes `L3_NEXT_HOP` hardware entries under neighbor churn/blackholing.
- **Fix**: Reuse shared `g_dropEgr` (skipping destroy in `hwRemoveNeighbor` when `egr == g_dropEgr`) or properly update matching NH egresses.

### 3.6 QoS Maps Destroyed While Still Attached to Removed Port
- **Location**: `SwitchBCM56860HW_qos.cpp:343-348`
- **Severity**: **Medium**
- **Mechanism**: `qosUnmapPort()` releases map refcounts via `qosHwMapRelease()` -> `hwbcm_qos_map_destroy()` without first detaching the ingress/egress QoS maps from the port (`hwbcm_qos_port_ingress_maps_set(bcm, 0, 0)` / egress `0`).
- **Impact**: `bcm_qos_map_destroy` may fail with `BCM_E_BUSY` (leaking hardware QoS map profile slots) or leave the port pointing to a freed map index.
- **Fix**: Detach ingress/egress QoS maps from `bcm` before calling `qosHwMapRelease()`.

### 3.7 CoPP Permanently Disables Stats for All Subsequent Traps on Single TCAM-Full Failure
- **Location**: `hwbcm/hwbcm_copp.c:950-960`
- **Severity**: **Medium**
- **Mechanism**: If `bcm_field_entry_install()` fails for ANY reason (including `BCM_E_FULL`/`BCM_E_RESOURCE` when the IFP slice is full), lines 950-960 assume FlexCounter attachment caused it, detach/destroy the stat, and permanently set `g_have_stats = 0` for the lifetime of `syncd`.
- **Impact**: A transient IFP TCAM-full event permanently breaks CoPP drop/hit counters for all traps until reboot.
- **Fix**: Only clear `g_have_stats` when `bcm_field_entry_stat_attach` itself returns `BCM_E_UNAVAIL`/`BCM_E_RESOURCE`, not on unrelated `bcm_field_entry_install` failures.

*(Also note all create-rollback leaks in Cat 2.2 and shadow-map-erased-before-destroy leaks in Cat 2.4.)*

---

## Category 4: Hardware Table Full Behaviour (`BCM_E_FULL` = -6 / `BCM_E_RESOURCE` = -14)

*(Verified from SDK `include/shared/error.h`: `_SHR_E_FULL = -6`, `_SHR_E_RESOURCE = -14`, `_SHR_E_MEMORY = -4`.)*

### 4.1 Complete Absence of `SAI_STATUS_TABLE_FULL` Across Entire Custom SAI + `orchagent` Impact
- **Location**: Entire `sairedis/vslib/SwitchBCM56860HW*.cpp` (`grep SAI_STATUS_TABLE_FULL` -> **0 matches**)
- **Severity**: **Critical**
- **Verified SONiC `orchagent` behaviour** (`sonic-swss/orchagent/saihelper.cpp:589-598`):
  ```cpp
  case SAI_STATUS_TABLE_FULL:
  case SAI_STATUS_INSUFFICIENT_RESOURCES:
  case SAI_STATUS_NO_MEMORY:
  case SAI_STATUS_NV_STORAGE_FULL:
      return task_process_status::task_need_retry;
  default:
      handleSaiFailure(true); // sets gOrchUnhealthy=true, logs sai-operation-failure,
                              // triggers SAI_REDIS_NOTIFY_SYNCD_INVOKE_DUMP!
      return task_process_status::task_failed; // DROPPED PERMANENTLY, no retry!
  ```
  `routeorch.cpp`, `neighorch.cpp`, `nhgorch.cpp`, `aclorch.cpp` all call `handleSaiCreateStatus()` / `handleSaiSetStatus()`. Returning `SAI_STATUS_FAILURE` instead of `SAI_STATUS_TABLE_FULL` / `SAI_STATUS_INSUFFICIENT_RESOURCES` marks `orchagent` unhealthy, triggers a syncd state dump, and **permanently drops the route/neighbor/NHG/FDB** without retrying when space frees up!

- **Table-by-Table Audit**:
  | Hardware Table | SDK Call | SAI Handler & Line | Current Return on `BCM_E_FULL`(-6)/`BCM_E_RESOURCE`(-14) | Expected SAI Status |
  |---|---|---|---|---|
  | `L3_DEFIP` / ALPM | `bcm_l3_route_add` | `SwitchBCM56860HW_l3.cpp:1602-1606` | `SAI_STATUS_FAILURE` (**BROKEN**) | `SAI_STATUS_TABLE_FULL` |
  | `L3_ENTRY` (host) | `bcm_l3_host_add` | `SwitchBCM56860HW_l3.cpp:987-997` | `SAI_STATUS_FAILURE` (**BROKEN**) | `SAI_STATUS_TABLE_FULL` |
  | `L3_NEXT_HOP` | `bcm_l3_egress_create` | `SwitchBCM56860HW_l3.cpp:913-917, 971-975, 1202-1206` | `SAI_STATUS_FAILURE` (**BROKEN**) | `SAI_STATUS_TABLE_FULL` / `INSUFFICIENT_RESOURCES` |
  | `L3_ECMP` / `ECMP_GROUP` | `bcm_l3_egress_ecmp_create/set` | `SwitchBCM56860HW_l3.cpp:1369-1391, 1554-1559` | `SAI_STATUS_FAILURE` (**BROKEN**) | `SAI_STATUS_TABLE_FULL` / `INSUFFICIENT_RESOURCES` |
  | `L2X` (FDB) | `bcm_l2_addr_add` | `SwitchBCM56860HW.cpp:2520-2526, 2553-2559` & `SwitchBCM56860HW_vxlan.cpp:1739-1744` | `SAI_STATUS_FAILURE` (**BROKEN**) | `SAI_STATUS_TABLE_FULL` |
  | `L3_IIF` / `EGR_L3_INTF` | `bcm_l3_intf_create` | `SwitchBCM56860HW_l3.cpp:597-601, 613-618` | `SAI_STATUS_FAILURE` (**BROKEN**) | `SAI_STATUS_TABLE_FULL` / `INSUFFICIENT_RESOURCES` |
  | `VRF` | `hwbcm_l3_vrf_create` (`BCM_E_FULL`) | `SwitchBCM56860HW_l3.cpp:227-231` | **Silently uses VRF 0!** (**BROKEN**) | `SAI_STATUS_TABLE_FULL` |
  | `VXLAN` VPN/VP/Tunnel | `hwbcm_vxlan_*` | `SwitchBCM56860HW_vxlan.cpp:753-758, 1042-1053, 1307-1312` | `SAI_STATUS_FAILURE` (**BROKEN**) | `SAI_STATUS_TABLE_FULL` / `INSUFFICIENT_RESOURCES` |
  | `TRUNK` / `HOSTIF` / `VLAN` | `hwbcm_trunk_create`, `knet_netif`, `vlan` | `SwitchBCM56860HW.cpp:2002-2007, 2206-2211, 2622-2627` | `SAI_STATUS_FAILURE` (**BROKEN**) | `SAI_STATUS_TABLE_FULL` / `INSUFFICIENT_RESOURCES` |
  | `IFP` / `EFP` / `VFP` | `bcm_field_*` | `SwitchBCM56860HW_acl.cpp:245-258` | `SAI_STATUS_INSUFFICIENT_RESOURCES` (**OK for retry**, though `-6` is ideally `TABLE_FULL`) | `SAI_STATUS_TABLE_FULL` (`-6`) / `INSUFFICIENT_RESOURCES` (`-14`) |
  | `CoPP` IFP/policer | `hwbcm_copp_*` | `SwitchBCM56860HW_copp.cpp:415-420, 523-530` | `SAI_STATUS_SUCCESS` (**Swallowed!**) | `SAI_STATUS_INSUFFICIENT_RESOURCES` |
  | `Mirror` / `QoS` | `hwbcm_mirror_*`, `hwbcm_qos_*` | `SwitchBCM56860HW_mirror.cpp:55`, `SwitchBCM56860HW_qos.cpp:1357, 1456, 2411` | `SAI_STATUS_INSUFFICIENT_RESOURCES` (**OK**) | `SAI_STATUS_INSUFFICIENT_RESOURCES` |

- **Fix**: Add a shared helper `static sai_status_t bcmRvToSai(int rv)` mapping `-6` (`BCM_E_FULL`) -> `SAI_STATUS_TABLE_FULL`, `-14` (`BCM_E_RESOURCE`) -> `SAI_STATUS_INSUFFICIENT_RESOURCES`, `-4` (`BCM_E_MEMORY`) -> `SAI_STATUS_NO_MEMORY`, `-8` (`BCM_E_EXISTS`) -> `SAI_STATUS_ITEM_ALREADY_EXISTS`, `-7` (`BCM_E_NOT_FOUND`) -> `SAI_STATUS_ITEM_NOT_FOUND`, `-10` (`BCM_E_BUSY`) -> `SAI_STATUS_OBJECT_IN_USE`, `-16` (`BCM_E_UNAVAIL`) -> `SAI_STATUS_NOT_SUPPORTED`, and use it across all `SwitchBCM56860HW*.cpp` files.

---

## Category 5: Thread Safety & Locking

### Verified Thread & Locking Architecture in `syncd`
1. **Main SAI API thread** + **`syncd` FlexCounter polling thread(s)**: Verified in `sonic-sairedis/vslib/Sai.cpp` that ALL SAI entry points (`create`, `remove`, `set`, `get`, `getStats`, `getStatsExt`, `clearStats`, `bulk*`, `flushFdbEntries`, `objectTypeGetAvailability`) acquire `MUTEX()` (`std::recursive_mutex m_apimutex`). Thus FlexCounter `getStatsExt` is serialized with SAI CRUD APIs. *(Only `apiUninitialize()` clears `m_vsSai` after stopping internal vslib threads.)*
2. **`vslib` EventQueue thread** (`SaiEventQueue.cpp:83, 93`), **FDB Aging thread** (`SaiFdbAging.cpp:49`), **Unittest thread** (`SaiUnittests.cpp:264`): all acquire `m_apimutex`.
3. **`SwitchBCM56860HW::m_linkThread`** (`linkThreadMain`): takes `m_linkMutex` and `m_mapMutex`, pushes to thread-safe `EventQueue`.
4. **SDK `bcmLINK` thread** (`hwbcm_linkscan_handler` -> `linkCallbackC`).
5. **SDK `bcmL2X` thread** (`hwbcm_l2_addr_handler` -> `l2CallbackC`).
6. **SDK `bcmCNTR` counter DMA thread** (internal SDK locking).
7. **Diagnostic shell socket thread** (`shell_sock_main` in `hwbcm/hwbcm_core.c:743-762`) — **NO `m_apimutex`!**

### 5.1 CRITICAL: Instant Self-Deadlock on `m_mapMutex` When Creating/Removing a LAG Router Interface
- **Location**: `SwitchBCM56860HW_l3.cpp:520-528` (`hwCreateRouterInterface`) and `746-754` (`hwRemoveRouterInterface`), calling `bcmPortOf()` at `SwitchBCM56860HW.cpp:3397`
- **Severity**: **Critical**
- **Mechanism**: `m_mapMutex` is a non-recursive `std::mutex` (`SwitchBCM56860HW.h:308`).
  ```cpp
  // SwitchBCM56860HW_l3.cpp:520-528 (and identical at 746-754):
  std::lock_guard<std::mutex> lock(m_mapMutex);
  auto mit = m_lagMembers.find(portId);
  if (mit != m_lagMembers.end()) {
      for (auto poid : mit->second) {
          int b = bcmPortOf(poid); // <-- ACQUIRES m_mapMutex AGAIN -> SELF-DEADLOCK!
  ```
- **Trigger**: Creating or removing ANY `SAI_ROUTER_INTERFACE_TYPE_PORT` RIF whose `PORT_ID` is a LAG containing at least 1 member (standard SONiC PortChannel L3 interface!).
- **Impact**: **100% deterministic permanent hang of `syncd`** while holding `Sai::m_apimutex`, freezing all SAI APIs, FlexCounters, link events, and FDB learning until watchdog kills `syncd`.
- **Fix**: Copy `mit->second` into a local vector under `m_mapMutex`, release the lock (scoped block), and then call `bcmPortOf(poid)` — or look up `m_portToBcm` directly while holding `m_mapMutex`.

### 5.2 CRITICAL: Diagnostic Shell Socket Thread — Unlocked Concurrent VXLAN/SDK Mutations, fd 1/2 Hijack, Detached-Thread Close Race
- **Location**: `hwbcm/hwbcm_core.c:556-633, 710-762, 804-837`
- **Severity**: **Critical**
- **Mechanism**:
  1. `hwbcm_shell_hw_cmd()` (`556-620`) calls `hwbcm_vxlan_access_port_add2` and `hwbcm_vxlan_port_delete` directly from `shell_sock_main` WITHOUT holding `Sai::m_apimutex` or any mutex in `hwbcm_vxlan.c` (`s_vp[]`, `s_acc_intf[]`, `s_vpn_l3[]` have ZERO locks!) -> **data race & table corruption** with concurrent SAI VXLAN/VLAN/FDB calls.
  2. `sh_process_command` (`633`) runs arbitrary SDK diag commands concurrently with SAI API calls and `hwbcm_deinit` (`g_shell_lock` only serializes shell clients against each other, NOT against SAI APIs or `hwbcm_deinit` — note `hwbcm_deinit` does not even take `g_shell_lock` before tearing down the unit!).
  3. `shell_sock_serve` (`726-737`) calls `dup2(client, 1); dup2(client, 2);` — redirecting process-wide stdout/stderr of ALL threads in `syncd` into the client socket (see Cat 6.1 for fatal `SIGPIPE` crash).
  4. `shell_sock_start` (`804`) spawns a `PTHREAD_CREATE_DETACHED` thread; `shell_sock_stop` (`819-837`) closes `g_shell_sock_fd` without `pthread_join`, racing with `accept(g_shell_sock_fd, ...)` on fd reuse and allowing `shell_sock_serve` to run past `hwbcm_deinit()`.
  5. `shell_sock_serve` has no `SO_RCVTIMEO` on `client`, so an idle client blocks `shell_sock_main` forever.
- **Impact**: Heap/table corruption in `hwbcm_vxlan.c`, `SIGPIPE` crash of `syncd`, UAF during warm/cold shutdown.
- **Fix**: Make `shell_sock_main` joinable (`shutdown(fd, SHUT_RDWR)` + `pthread_join` in `shell_sock_stop`), set `SO_RCVTIMEO`, protect `hwbcm_vxlan.c` and `hwbcm_deinit` with locks (or remove mutating `hw` shell commands), and never `dup2` process-wide fd 1/2 (use a pipe/bsl sink hook or ensure `SIGPIPE` is ignored via `MSG_NOSIGNAL`/`SIG_IGN` and document stdout capture hazard).

### 5.3 HIGH: Unlocked Data Races Between SDK `bcmLINK` / `bcmL2X` Threads and SAI API Thread
- **Locations**:
  - `hwbcm/hwbcm_core.c:237-244, 481-520, 2774, 2824-2825`: `hwbcm_linkscan_handler` (SDK `bcmLINK` thread) reads `g_desired_speed[port]` and reads/writes `g_sgmii_applied[port]` & `g_sgmii_applied_at[port]` WITHOUT `g_lock`, while `hwbcm_port_speed_set` / `hwbcm_core_state_reset` (SAI thread) write them under `g_lock`.
  - `hwbcm/hwbcm_core.c:256-257, 524-535, 2414-2415, 2550-2551`: `g_link_cb` and `g_link_cookie` read non-atomically in `hwbcm_linkscan_handler` while modified in `hwbcm_link_callback_register` / `hwbcm_deinit` (can invoke new `cb` with stale/NULL `cookie` or dereference torn/freed `this` pointer during shutdown!).
  - `hwbcm/hwbcm_l2.c:377-443`: `hwbcm_l2_addr_handler` (SDK `bcmL2X` thread) reads `g_l2_cb` at line 383 and `g_l2_cookie` at line 411 without synchronization while `hwbcm_l2_callback_register` / `hwbcm_l2_warm_reset` write them.
  - `hwbcm/hwbcm_core.c:90, 122-125`: `hwbcm_log()` is called from ALL threads (SAI, `bcmLINK`, `bcmL2X`, shell) and races on `g_stderr_unbuffered` + calls `setvbuf(stderr, NULL, _IONBF, 0)` concurrently and after prior stderr I/O (C11/POSIX Undefined Behavior).
- **Severity**: **High**
- **Impact**: Torn `cb`/`cookie` -> NULL deref or UAF crash (`SIGSEGV`) during warm/cold shutdown; missed/spurious 1G SGMII link bounce; undefined stdio behavior.
- **Fix**: Protect `g_desired_speed`/`g_sgmii_*`/`g_link_cb`/`g_link_cookie` under a dedicated lightweight spin/mutex (don't hold across `bcm_port_*` or user callback; copy `cb, cookie` locally under lock), same for `g_l2_cb`/`g_l2_cookie`; remove `setvbuf(stderr, ...)` from `hwbcm_log()` (or run once via `pthread_once` at init).

### 5.4 MEDIUM: Unlocked Reads in `hwbcm_stats.c`
- **Location**: `hwbcm/hwbcm_stats.c:179-183, 317`
- **Severity**: **Medium**
- **Mechanism**: `hwbcm_stats_bst_enabled()` reads `g_bst_on` without `g_lock`; `hwbcm_queue_stat_multi_get()` reads `g_num_cos`/`g_num_cpu_cos` before acquiring `g_lock` (currently mitigated only because all SAI callers hold `Sai::m_apimutex`, except `hwbcm_stats_init`/`warm_reset` if called from outside).
- **Fix**: Acquire `g_lock` around `g_bst_on` and move `g_num_cos` checks inside `g_lock`.

---

## Category 6: Crash Vectors (`SIGSEGV`, `SIGABRT`, `SIGPIPE`, Deadlock, OOB, Integer Overflow)

### 6.1 CRITICAL: Self-Deadlock in `hwCreateRouterInterface` / `hwRemoveRouterInterface`
- **Location**: `SwitchBCM56860HW_l3.cpp:520-528, 746-754`
- **Severity**: **Critical** (detailed in 5.1 — freezes `syncd` permanently on any L3 LAG RIF).

### 6.2 CRITICAL/HIGH: `SIGPIPE` Kills `syncd` When Diag Shell Client Disconnects Mid-Command
- **Location**: `hwbcm/hwbcm_core.c:726-733` (and `hwbcm_core.c:127-128`)
- **Severity**: **High** (Critical in operational troubleshooting)
- **Mechanism**: Verified across all of `sonic-sairedis/syncd/` and `sairedis/vslib/`: `syncd` does **NOT** install `signal(SIGPIPE, SIG_IGN)`. When a diagnostic client connects to `/var/run/hwbcm/shell.sock`, `shell_sock_serve` calls `dup2(client, 1); dup2(client, 2);`. If the client closes early (e.g., `bsh "d chg" | head -n 5`, Ctrl-C, or SSH drop), the next `write()` to fd 1 or fd 2 — either by `sh_process_command`/`printf` OR by ANY other thread calling `hwbcm_log()` -> `fprintf(stderr, ...)` — raises unhandled `SIGPIPE`, **instantly terminating `syncd`**!
- **Impact**: Running `bsh <cmd> | head` or pressing Ctrl-C on a long diag dump crashes the entire `syncd` process and triggers a switch data-plane outage/restart!
- **Fix**: Ignore `SIGPIPE` once at init (`signal(SIGPIPE, SIG_IGN);`) AND eliminate process-wide `dup2(client, 1/2)`.

### 6.3 HIGH: Negative `vpn_id` Bypasses Bounds Check in `hwbcm_vxlan_vfi_of()`
- **Location**: `hwbcm/hwbcm_vxlan.c:1444-1449`
- **Severity**: **High**
- **Mechanism**:
  ```c
  static int hwbcm_vxlan_vfi_of(int vpn_id) {
      int vfi = vpn_id & 0xfff;
      return (vfi >= 0 && vfi < HWBCM_VXLAN_MAX_VFI) ? vfi : -1;
  }
  ```
  If an uninitialized/sentinel `vpn_id == -1` is passed, `(-1) & 0xfff == 4095`, which satisfies `0 <= 4095 < 8192` (`HWBCM_VXLAN_MAX_VFI`), silently aliasing `s_vpn_l3[4095]` instead of returning `-1`!
- **Impact**: Corrupts or reads wrong VFI L3 binding slot `s_vpn_l3[4095]` when a sentinel `-1` VPN is passed.
- **Fix**: Check `if (vpn_id < 0) return -1;` before masking `vpn_id & 0xfff`.

### 6.4 HIGH: Integer Wrap-Around & Signed Overflow in `hwbcm_qos.c` Buffer/Rate Math
- **Location**: `hwbcm/hwbcm_qos.c:88-91` (`qos_cells`), `613`, `867`, `878`, `1010-1020` (`qos_kbits`)
- **Severity**: **High**
- **Mechanism**:
  1. `qos_cells(uint32_t bytes)` computes `(bytes + HWBCM_MMU_CELL_BYTES - 1) / HWBCM_MMU_CELL_BYTES` in 32-bit unsigned arithmetic. For `bytes >= 0xffffff31u` (e.g., `UINT32_MAX`), `bytes + 207` wraps to `< 208` -> returns `0` cells instead of saturating!
  2. Lines `613`, `867`, `878` pass `(int)(cells * 208)` into `bcm_cosq_control_set`: if `bytes > INT_MAX` (2 GiB), the signed `int` becomes negative and fails or misprograms MMU thresholds.
  3. `qos_kbits(uint64_t val, int pkts)` at lines `1010-1020` computes `val * 8` in `uint64_t` WITHOUT overflow clamping (unlike `hwbcm_copp.c:511`), wrapping to a tiny rate for `val > UINT64_MAX / 8`, and then casts to `uint32` without capping at `UINT32_MAX`.
- **Impact**: Large SAI buffer/shaper values (common in SONiC for "unlimited" or default max) wrap to `0` or negative values, either dropping all traffic on a queue/PG or failing QoS config.
- **Fix**: Use 64-bit arithmetic in `qos_cells` and saturate to `INT_MAX / 208`; guard `val > UINT64_MAX / 8` and clamp result to `UINT32_MAX` in `qos_kbits`.

### 6.5 MEDIUM: Unchecked `SaiAttrWrap` / `getAttr()` Pointer Dereferences
- **Location**: `SwitchBCM56860HW.cpp:478, 514, 544, 553, 3389, 3559, 3576`; `SwitchBCM56860HW_l3.cpp:1936`; `SwitchBCM56860HW_vxlan.cpp:799`; `SwitchBCM56860HW_acl.cpp:349`
- **Severity**: **Medium**
- **Mechanism**: Direct `it->second->getAttr()->value...` without checking `it->second != nullptr && it->second->getAttr() != nullptr` (contrast with `SwitchBCM56860HW_wb.inc.cpp:2116, 3075` which explicitly guard both).
- **Impact**: `SIGSEGV` if any `m_objectHash` attribute wrapper is ever null (e.g., after partial deserialization/warm-boot recovery).
- **Fix**: Route lookups through `getStoredAttr()` (and add `if (ait->second == nullptr || ait->second->getAttr() == nullptr) return false;` in `getStoredAttr` at `SwitchBCM56860HW.cpp:3389`).

### 6.6 MEDIUM: Unvalidated `list` Pointers When `count > 0` & Unchecked `vec.resize()` in `.hwstate` Loader
- **Location**:
  - `SwitchBCM56860HW.cpp:1114-1116, 3504`, `SwitchBCM56860HW_acl.cpp:634, 647, 775, 890, 1385, 2189`: dereferences `attr->value.*.list[i]` when `count > 0` without checking `list != nullptr` (relies strictly on Meta validation).
  - `SwitchBCM56860HW_wb.inc.cpp:1472, 1487, 1502, 1514, 1614, 1828, 1841, 1863, 1895, 1907`: calls `vec.resize(n)` / `vec.reserve(cnt)` using raw `uint32_t` counts from `.hwstate` BEFORE verifying `n * elemBytes <= tr.remaining()` (mitigated against bitflips by CRC32, but a truncated/crafted file with valid CRC can trigger `std::bad_alloc` -> `SIGABRT`).
- **Fix**: Check `list != nullptr` when `count > 0`; bound every `.hwstate` `resize(n)` against `tr.remaining() / minElemSize`.

### 6.7 Verified Clean Checks (No Crash Bugs Found)
- **`.at()` audit**: All 4 `.at()` calls checked — `SwitchBCM56860HW.cpp:2497`, `SwitchBCM56860HW_vxlan.cpp:1199, 1298` are explicitly guarded by `.find()`/`.count()`; `SwitchBCM56860HW.cpp:1708` (`m_objectHash.at(SAI_OBJECT_TYPE_FDB_ENTRY)`) is safe because `SwitchState` pre-populates every `sai_object_type_t` key in its constructor (`sonic-sairedis/vslib/SwitchState.cpp:55`).
- **`assert()` / `abort()` / `exit()` / `throw`**: Zero occurrences in `SwitchBCM56860HW*` and `hwbcm/*`.
- **FlexCounter concurrency**: Verified `Sai.cpp` acquires `m_apimutex` in `getStats`, `getStatsExt`, `clearStats`, `objectTypeGetAvailability`, `flushFdbEntries`, and all `bulk*` methods.

