# Parity Fix Batch 1 Report (`parity-fix-1`)

- **Branch**: `parity-fix-1` (18 commits on top of `main`, 1 commit per item)
- **Warm-boot binary**: `deploy/build/libsaivs.so.0.0.0` (`151573016` bytes, `ALL CHECKS PASSED`)
- **SHA-256**: `bd9589ea59036e8094bdc00f9746db59eca5c18c7db27a71ec4dce2b5d143def`
- **Warm init rule**: Zero hardware writes added to warm-boot init; `.hwstate` format unchanged.

---

## Group A — Crash / Hang

### 1. `domain6 5.1/6.1`: Self-deadlock on `m_mapMutex` (LAG RIF)
- **Status**: fixed
- **Commit**: `45fb3f6`
- **Summary**: `hwCreateRouterInterface` and `hwRemoveRouterInterface` now look up `m_portToBcm` directly while holding `m_mapMutex` instead of calling `bcmPortOf()` (which re-locks non-recursive `m_mapMutex`). Audited all other 8 `m_mapMutex` holders — no other nested lock exists.
- **On-box test**:
  ```bash
  sudo config portchannel add PortChannel0001
  sudo config portchannel member add PortChannel0001 Ethernet0
  sudo config interface ip add PortChannel0001 10.99.1.1/30
  sudo config interface ip remove PortChannel0001 10.99.1.1/30
  sudo config portchannel member del PortChannel0001 Ethernet0
  sudo config portchannel del PortChannel0001
  ```
  - **Before**: `syncd` deadlocks permanently on `create`/`remove` of the LAG router interface.
  - **After**: Commands complete cleanly; `bsh "l3 intf show"` shows the LAG RIF created and removed without hanging.

### 2. `domain6 5.2/6.2`: Diag shell socket thread hardening
- **Status**: fixed
- **Commit**: `98f0b75`
- **Summary**: Installed `SIGPIPE` ignore in `hwbcm_init_ex`; replaced process-wide `dup2(client, 1/2)` with per-thread `bslmgmt_redir_hook_set` + `send(..., MSG_NOSIGNAL)`; added 5 s `SO_RCVTIMEO`/`SO_SNDTIMEO`; made `shell_sock_main` joinable with `poll()` + `shutdown()` + `pthread_join()` before `close()`; added recursive `s_vxlan_lock` in `hwbcm_vxlan.c`. Preserves `deploy/bsh` wire protocol (`<cmd>\n` -> output + `[hwbcm rc=N]\n`).
- **On-box test**:
  ```bash
  # 1) Normal bsh protocol works
  deploy/bsh "ps"
  # 2) Abrupt client disconnect mid-output must not kill syncd or silence logs
  python3 -c 'import socket; s=socket.socket(socket.AF_UNIX); s.connect("/run/hwbcm-shell.sock"); s.sendall(b"d chg\n"); s.close()'
  pidof syncd && deploy/bsh "l3 intf show"
  # 3) Clean restart joins shell thread cleanly
  sudo systemctl restart syncd
  ```
  - **Before**: Abrupt disconnect triggered `SIGPIPE` (killing `syncd`) or redirected `syncd` stdout/stderr onto closed/client fds; `deinit` raced `accept()`.
  - **After**: `syncd` PID unchanged, syslog/stderr intact, `bsh` continues answering, clean shutdown/restart.

### 3. `domain5 HIGH 3`: Stack buffer overflow in `VirtualSwitchSaiInterface::clearStats`
- **Status**: fixed
- **Commit**: `dd77cf4` (updated `../sonic-sairedis-merge/vslib/VirtualSwitchSaiInterface.cpp` and `sairedis/vslib.patch`)
- **Summary**: Replaced fixed `uint64_t counters[128]` with `std::vector<uint64_t>` and bounds-checked `counter_ids` (`NULL`) and `number_of_counters <= 4096`.
- **On-box test**:
  ```bash
  sonic-clear counters
  # Or invoke clear_port_stats with >128 counter IDs via sairedis/py-sai test
  ```
  - **Before**: Clearing >128 counters overflowed stack buffer and crashed `syncd` (`SIGABRT` / stack smashing).
  - **After**: Succeeds (or returns `SAI_STATUS_INVALID_PARAMETER` if `>4096` or `NULL`), no stack corruption.

### 4. `domain6 6.3, 6.4 (= domain4 R4)`: Negative `vpn_id` & `qos_cells()`/`qos_kbits()` overflow
- **Status**: fixed
- **Commit**: `abdbcb2`
- **Summary**: Rejected `vpn_id < 0` before `& 0xfff` in `hwbcm_vxlan_vfi_of()`; switched `qos_cells()` to 64-bit math saturated at `INT_MAX / HWBCM_QOS_CELL_BYTES`; guarded `qos_kbits()` against `uint64_t` multiplication overflow.
- **On-box test**:
  ```bash
  # Configure a buffer profile with huge size (e.g. 4294967295) via CONFIG_DB BUFFER_PROFILE
  redis-cli -n 4 HSET "BUFFER_PROFILE|test_big" pool "[BUFFER_POOL|ingress_lossy_pool]" size 4294967295 static_th 4294967295
  sudo grep -i "qos:" /var/log/syslog | tail -n 10
  redis-cli -n 4 DEL "BUFFER_PROFILE|test_big"
  ```
  - **Before**: `qos_cells(0xffffffff)` wrapped 32-bit unsigned to `0` cells / negative signed `int` passed to `bcm_cosq_control_set`.
  - **After**: Saturated cleanly (rejected by SDK with clean error, never wrapped to 0 or negative bytes).

### 5. `domain3 HIGH 7`, `domain6 6.5/6.6`: NULL list pointers, unchecked `getAttr()`, `.hwstate` resize bounds
- **Status**: fixed
- **Commit**: `dadf543`
- **Summary**: Guarded `list == nullptr` across `SwitchBCM56860HW.cpp` and `SwitchBCM56860HW_acl.cpp`; guarded `->getAttr() == nullptr` across core/L3/VXLAN/ACL; bounded every `.hwstate` vector allocation in `SwitchBCM56860HW_wb.inc.cpp` against `tr.remaining() / elemBytes`.
- **On-box test**:
  ```bash
  # Truncated/corrupt .hwstate test (on a test warm-boot cycle):
  # Corrupt a count field in /var/warmboot/hwbcm.hwstate to 0x7fffffff, restart syncd warm
  sudo grep -i "hwstate" /var/log/syslog | tail -n 10
  ```
  - **Before**: Corrupt `.hwstate` threw `std::bad_alloc` / OOM-killed `syncd`; NULL list pointers in ACL/hash/flush dereferenced NULL.
  - **After**: Corrupt `.hwstate` fails load cleanly and falls back to cold boot; NULL lists are rejected with `SAI_STATUS_INVALID_PARAMETER` or skipped safely.

---

## Group B — Error Mapping and Rollback

### 6. `domain6 4.1`: Shared `BCM_E_*` -> SAI status mapper
- **Status**: fixed
- **Commit**: `088fb1f`
- **Summary**: Added `HWBCM_E_*` constants matching OpenBCM `include/shared/error.h` (note: verified `_SHR_E_MEMORY = -2`, `_SHR_E_PARAM = -4`, correcting the audit doc's typo for `BCM_E_MEMORY`) and `SwitchBCM56860HW::bcmRvToSai(int rv)` mapping `FULL -> TABLE_FULL`, `RESOURCE/MEMORY -> INSUFFICIENT_RESOURCES`, `EXISTS -> ITEM_ALREADY_EXISTS`, `NOT_FOUND -> ITEM_NOT_FOUND`, `PARAM/PORT -> INVALID_PARAMETER`, `UNAVAIL -> NOT_SUPPORTED`, `BUSY -> OBJECT_IN_USE`, wired across L3/FDB/VXLAN/ACL/VLAN/LAG/Hostif/Port/Switch. Confirmed `hwbcm/*.c` already return raw negative `BCM_E_*`.
- **On-box test**:
  ```bash
  # Trigger an ACL slice-full or invalid-parameter error and check syncd/orchagent syslog status code
  sudo grep -E "SAI_STATUS_TABLE_FULL|SAI_STATUS_INVALID_PARAMETER|SAI_STATUS_NOT_SUPPORTED" /var/log/syslog | tail -n 10
  ```
  - **Before**: Every HW failure returned generic `SAI_STATUS_FAILURE`, preventing `orchagent` retry/resource handling.
  - **After**: Specific SAI status codes (`TABLE_FULL`, `INSUFFICIENT_RESOURCES`, `INVALID_PARAMETER`, `NOT_SUPPORTED`, etc.) are returned.

### 7. `domain6 2.1`: `SwitchBCM56860HW::set()` rollback of `m_objectHash`
- **Status**: fixed
- **Commit**: `099340e`
- **Summary**: Snapshots previous `SaiAttrWrap` in `m_objectHash` before `SwitchBCM56850::set()` and restores (or erases if newly inserted) whenever `hwSet*` fails.
- **On-box test**:
  ```bash
  # Attempt to set an unsupported attribute on a queue (e.g. PFC_DLR_INIT) or invalid speed on a port
  # Read the attribute back via SAI get / redis-cli -n 1 (ASIC_DB)
  ```
  - **Before**: `m_objectHash` retained the rejected attribute value despite `set()` returning error, desyncing subsequent `get()` and warm-boot state from HW.
  - **After**: Failed `set()` leaves `m_objectHash` at the previous value matching HW.

### 8. `domain6 2.2, 2.3`: `create()` HW rollback on post-hook fail; defer ACL strip in `remove()`
- **Status**: fixed
- **Commit**: `1c2ccfa`
- **Summary**: `create()` tracks `hwCreated` and invokes `hwAclObjectRemoved` + matching `hwRemove*` if post-create hooks (`hwAclBindOnCreate` / `hwCoppOnTrapCreate`) fail; `remove()` calls `hwAclObjectRemoved(oid)` only after `hwRemove*` succeeds.
- **On-box test**:
  ```bash
  # Bind an ingress ACL table to a port/VLAN, attempt to remove a still-referenced VLAN/LAG (fails with BUSY/in-use)
  deploy/bsh "fp show"
  ```
  - **Before**: Failed `remove()` already stripped the ACL binding from HW (`fp show` lost the port/VLAN from the FP entry).
  - **After**: Failed `remove()` keeps the ACL binding intact in HW; failed post-create hook tears down the just-created HW object.

### 9. `domain6 2.4, 2.5` (+ `domain1 HIGH 6`, `domain2 HIGH 9, 13`): Shadow-map mutations before HW & partial multi-step rollback
- **Status**: fixed
- **Commit**: `3834926`
- **Summary**: Fixed pre-mutation and partial-failure rollback across `hwbcm_l3.c` (`bcm_l3_intf_delete` before `bcm_l3_ingress_destroy`, added `hwbcm_l3_egress_drop_replace`), core (`hwCreatePort`, `hwCreateVlanMember`, `hwRemoveLagMember`), L3 (`hwCreate/RemoveNeighbor` all matching NHs + host re-add on destroy fail, `hwRemoveNextHop*`, `hwRemoveRoute`), VXLAN (`hwRemoveTunnelMapEntry`, `hwFdbOnTunnel`), ACL (`aclProgramEntry` extra variants first, counter cleanup, group members, bind), QoS (`qosHwSetPort/Switch`, `hwSetSchedulerGroup/Queue/IPG`), and Mirror (`hwRemoveMirrorSession`).
- **On-box test**:
  ```bash
  # Create a VLAN RIF + neighbor + route; try removing RIF while route/egress still references it
  deploy/bsh "l3 intf show"
  deploy/bsh "l3 ingress show"
  ```
  - **Before**: Failed RIF removal left `L3_IIF` destroyed while `L3_INTF` remained; failed NH/route/QoS/mirror ops left C++ shadow maps out of sync with ASIC tables.
  - **After**: Failed deletes leave both `L3_IIF` and `L3_INTF` intact and preserve shadow maps for clean retry.

### 10. `domain6 2.6 (= domain2 HIGH 4)`: `NEIGHBOR_ENTRY` missing in `set()` dispatch
- **Status**: fixed
- **Commit**: `fc9710f`
- **Summary**: Added `SwitchBCM56860HW::hwSetNeighbor()` handling `DST_MAC_ADDRESS`, `PACKET_ACTION`, and `NO_HOST_ROUTE` (updating neighbor + all matching `NEXT_HOP` egresses and calling `hwVxlanRetryPending()`) and dispatched `SAI_OBJECT_TYPE_NEIGHBOR_ENTRY` in `set()`.
- **On-box test**:
  ```bash
  # On a routed port/SVI with an active neighbor 10.1.1.2:
  sudo ip neigh replace 10.1.1.2 lladdr 02:aa:bb:cc:dd:ee dev Ethernet0 nud reachable
  deploy/bsh "l3 egress show"
  ```
  - **Before**: ASIC `L3_EGRESS` kept the old MAC address because `set(NEIGHBOR_ENTRY)` fell through without a HW call.
  - **After**: `l3 egress show` immediately shows `02:aa:bb:cc:dd:ee` on the neighbor (and any matching next-hop) egress.

---

## Group C — Wrong Forwarding

### 11. `domain6 3.1 (= domain2 Critical 2)`: Port-RIF VLAN pool 3000..3999 vs user VLANs
- **Status**: fixed
- **Commit**: `af2f797`
- **Summary**: `hwCreateRouterInterface` excludes user-created VLANs in `m_objectHash[SAI_OBJECT_TYPE_VLAN]` when allocating port-RIF VLANs in `3000..3999`; `hwRemoveRouterInterface` and `syncLagMemberPort` check `SAI_ROUTER_INTERFACE_ATTR_TYPE == SAI_ROUTER_INTERFACE_TYPE_PORT` before touching PVIDs or destroying the VLAN.
- **On-box test**:
  ```bash
  sudo config vlan add 3000
  sudo config vlan member add 3000 Ethernet4
  sudo config interface ip add Vlan3000 10.30.0.1/24
  sudo config interface ip add Ethernet0 10.30.1.1/30
  deploy/bsh "vlan show"
  sudo config interface ip remove Vlan3000 10.30.0.1/24
  deploy/bsh "vlan show"
  ```
  - **Before**: Port-RIF on `Ethernet0` collided with user `Vlan3000`; removing `Vlan3000` SVI destroyed HW VLAN 3000.
  - **After**: Port-RIF allocates VLAN 3001 (skipping 3000); removing `Vlan3000` SVI leaves HW VLAN 3000 and its L2 member `Ethernet4` intact.

### 12. `domain2 Critical 3`: Non-IP next-hop types (`TUNNEL_ENCAP` / `MPLS` / `SRV6`) silently succeeded
- **Status**: fixed
- **Commit**: `42c7b25`
- **Summary**: `hwCreateNextHop` returns `SAI_STATUS_NOT_SUPPORTED` for any `SAI_NEXT_HOP_ATTR_TYPE != SAI_NEXT_HOP_TYPE_IP`. Verified working VXLAN RIOT SVI uses `TYPE_IP` and is unaffected.
- **On-box test**:
  ```bash
  # Attempt to create an MPLS or TUNNEL_ENCAP next hop (or inspect via orchagent / sairedis test)
  # Existing VXLAN EVPN L3 SVI traffic continues unaffected:
  show vxlan tunnel
  ```
  - **Before**: Returned `SAI_STATUS_SUCCESS` with no HW egress, causing any subsequent route pointing to it to fail obscurely.
  - **After**: Fails immediately with `SAI_STATUS_NOT_SUPPORTED`; VXLAN RIOT SVI routing unchanged.

### 13. `domain2 HIGH 5 (= domain1 HIGH 3)`: Dynamic MAC move events dropped
- **Status**: fixed
- **Commit**: `f1243fd`
- **Summary**: `process_packet_for_fdb_event` now detects when an existing dynamic FDB entry moves to a different bridge port, updates `m_objectHash` and `m_fdb_info_set`, emits `SAI_FDB_EVENT_MOVE`, and calls `hwRefreshNeighborsOnLearn()`.
- **On-box test**:
  ```bash
  # With Ethernet4 and Ethernet8 in Vlan100 (and optional SVI neighbor on Vlan100):
  # Move host MAC from Ethernet4 to Ethernet8 and send a frame
  show mac
  deploy/bsh "l3 egress show"
  ```
  - **Before**: `show mac` stayed stuck on `Ethernet4` until aging expired; SVI neighbor egress pointed to old port.
  - **After**: `show mac` updates immediately to `Ethernet8` via `SAI_FDB_EVENT_MOVE` and SVI L3 egress moves to `Ethernet8`.

### 14. `domain2 HIGH 11, 12`: Empty ECMP group / last member removal & `resolveVrf()` VRF 0 fallback
- **Status**: fixed
- **Commit**: `6993385`
- **Summary**: `resolveVrf()` now returns `sai_status_t` and fails on unknown/failed VRF creation instead of silently falling back to VRF 0; `hwRemoveNextHopGroupMember` and `hwCreateRoute` point empty ECMP groups at `sharedDropEgress()` instead of ignoring 0-member `ecmp_set` failure.
- **On-box test**:
  ```bash
  # 1) Create a 2-path ECMP route, remove one member, then remove the last member while route remains:
  deploy/bsh "l3 ecmp show"
  # 2) Verify non-default VRF routes stay isolated in their own VRF:
  sudo config vrf add Vrf_test
  sudo config interface vrf bind Ethernet0 Vrf_test
  sudo config interface ip add Ethernet0 10.50.0.1/30
  deploy/bsh "l3 defip show"
  ```
  - **Before**: Removing last ECMP member ignored `ecmp_set` error so HW kept forwarding to the deleted egress; VRF resolution errors leaked routes into VRF 0.
  - **After**: Empty ECMP group points to drop egress (traffic dropped, not misforwarded); VRF creation/lookup errors fail cleanly instead of leaking into VRF 0.

### 15. `domain3 HIGH 3, 5`: Unmapped `ACL_IP_TYPE` & overly broad CoPP traps
- **Status**: fixed
- **Commit**: `7857892`
- **Summary**: `ipTypeOf()` explicitly maps `SAI_ACL_IP_TYPE_ANY` and returns `-1` for unsupported values so `aclParseRule` returns `SAI_STATUS_NOT_SUPPORTED`; `hwbcm_copp.c` qualifies OSPF/OSPFv6/VRRP/VRRPv6/PIM/IGMP/DHCPv6 with their standard multicast DMACs (+ `to_me` for unicast OSPF/OSPFv6/PIM/DHCPv6) so transit unicast packets are never intercepted or dropped.
- **On-box test**:
  ```bash
  # Check installed CoPP FP rules on the box:
  deploy/bsh "fp show group 1"
  # Send transit unicast IP proto 89/112/103/2 or UDP dport 546/547 between two ports in a routed transit subnet
  ```
  - **Before**: `fp show` showed OSPF/VRRP/PIM/IGMP/DHCPv6 entries matching only `EtherType + IpProtocol` (no DMAC / no `DstL3Egress`), trapping/dropping transit traffic; unknown `ACL_IP_TYPE` silently widened rules to ANY.
  - **After**: CoPP entries qualify multicast DMAC or `DstL3Egress == ip2me`, transit packets forward at line rate; unsupported `ACL_IP_TYPE` returns `SAI_STATUS_NOT_SUPPORTED`.

### 16. `domain4 R3, R6`: False `PFC_DLR_INIT` capability & ECN-only WRED drops non-ECT
- **Status**: fixed
- **Commit**: `7ad3b2f`
- **Summary**: Overrode `SwitchBCM56860HW::queryAttributeCapability` to report `create/set/get_implemented = false` for `SAI_QUEUE_ATTR_PFC_DLR_INIT` and `SAI_QUEUE_ATTR_ENABLE_PFC_DLDR` (and `hwSetQueue` returns `SAI_STATUS_NOT_SUPPORTED`); split `enable[]` (drop) from `ecn_enable[]` in `hwbcm_qos_wred_t` so ECN-only profiles leave `WRED_EN=0` and non-TCP drop curves off (`0x1ffff`, 0%).
- **On-box test**:
  ```bash
  # 1) Check orchagent log on boot: selects SW PFCWD handler instead of dead DLR handler
  sudo grep -i "pfc.*dlr\|PfcWd" /var/log/syslog | tail -n 10
  # 2) Configure an ECN-only WRED profile (green_drop_enable=false, ecn=ecn_all) on a queue and inspect HW:
  deploy/bsh "d MMU_WRED_CONFIG_X_PIPE 0 8"
  ```
  - **Before**: `queryAttributeCapability(PFC_DLR_INIT)` returned `true` (dead PFCWD path); ECN-only WRED set `WRED_EN=1` and programmed non-TCP curves, dropping non-ECT UDP packets.
  - **After**: `PFC_DLR_INIT` reports unsupported; ECN-only WRED shows `WRED_EN=0, ECN_MARKING_EN=1` with `NONTCP` profiles at `0x1ffff` (no non-ECT WRED drops).

---

## Group D — Threads & Unchecked Returns

### 17. `domain6 5.3, 5.4 (= domain1 HIGH 1, 7)`: Data races (`bcmLINK` / `bcmL2X` / stats vs SAI thread)
- **Status**: fixed
- **Commit**: `2f95832`
- **Summary**: Added leaf mutex `g_link_lock` for `g_desired_speed[]`, `g_sgmii_applied*`, `g_link_cb`/`g_link_cookie` and `g_knet_lock` for `g_knet[]` in `hwbcm_core.c`; switched `stderr` unbuffering in `hwbcm_log()` to `pthread_once`; added `g_l2_cb_lock` in `hwbcm_l2.c` (copying `cb`+`cookie` atomically, never held across SDK calls); protected `g_bst_on` and `g_num_cos`/`g_num_cpu_cos` reads under `g_lock` in `hwbcm_stats.c`.
- **On-box test**:
  ```bash
  # Flap 1G copper ports and change speed/PVID while polling queue stats & learning MACs:
  for i in 1 2 3 4 5; do
    sudo config interface shutdown Ethernet0; sleep 1
    sudo config interface startup Ethernet0; sleep 1
    show queue counters Ethernet0 >/dev/null
  done
  sudo grep -i "segfault\|assert\|error" /var/log/syslog | tail -n 10
  ```
  - **Before**: Unsynchronized concurrent reads/writes between `bcmLINK`, `bcmL2X`, and SAI threads risked torn callback/cookie pairs, lost SGMII state, and KNET/stats races.
  - **After**: Clean link flaps, MAC learns, and stats polls with zero crashes or races.

### 18. `domain6 1.1–1.7`: Unchecked returns across VLAN / RIF / KNET / CoPP / VXLAN / ACL / WB
- **Status**: fixed
- **Commit**: `751b18a`
- **Summary**:
  - `hwbcm_l2.c` (1.1): Check `vlan_stg_forward` & `hwbcm_vlan_cpu_add` in `hwbcm_vlan_create`/`hwbcm_vlan_port_add`, roll back and return error.
  - `SwitchBCM56860HW_l3.cpp` (1.2): Full port-RIF rollback (restore PVID + remove added ports + destroy VLAN with error logging) and log errors in `hwRemoveRouterInterface`.
  - `hwbcm_core.c` (1.3): Check & propagate `knet_pvid_filter_create` failure in `hwbcm_port_pvid_set`; log warning on `knet_vfi_filter_create` failure (and destroy `vfi_filter_id` on rollback).
  - `SwitchBCM56860HW_copp.cpp` (1.4): `coppApplyTrap`/`coppApplyGroup`/`coppApplyPolicerUsers`, `hwSetPolicer`, `hwCreate/SetHostifTrapGroup`, `hwCoppOnTrapCreate/Set` propagate real HW errors while keeping unsupported trap kinds (`NONE`/`UNAVAIL`) as `SUCCESS`.
  - `SwitchBCM56860HW_vxlan.cpp` (1.5): Check & log `hwbcm_vxlan_egress_destroy`, `hwbcm_vxlan_l2_addr_delete`, `hwbcm_vxlan_vpn_flood_set`; retain `vpn` in `floodVpns` if flood removal fails.
  - `SwitchBCM56860HW_acl.cpp` (1.6): Log errors from `aclRefreshTable` / `aclProgramEntry` in `hwAclObjectRemoved`, `hwAclLagMembersChanged`, `hwAclMirrorSessionUpdated`.
  - `SwitchBCM56860HW.cpp` & `_wb.inc.cpp` (1.7): Check & log `hwbcm_stats_init()` / `qosBuildHwMap()`, and fail `hwStateLoad()` (triggering clean cold fallback) if `hwbcm_{l3,vxlan,copp}_warm_restore()` fails.
- **On-box test**:
  ```bash
  # Normal cold & warm syncd restart: verify zero new errors and warm boot succeeds
  sudo config warm_restart enable swss
  sudo systemctl restart syncd
  sudo grep -E "wb hwstate:|warm_restore|hwbcm_stats_init|qosBuildHwMap" /var/log/syslog | tail -n 15
  ```
  - **Before**: Silent errors in VLAN/RIF/KNET/CoPP/VXLAN/ACL/WB left ports STG-disabled, KNET tags unstripped, CoPP traps unpoliced, or corrupt warm-boot C state with `SAI_STATUS_SUCCESS`.
  - **After**: Errors are logged and propagated; failed warm C restores cleanly fall back to cold init instead of running half-restored.

---

## Next (Out of Scope for Batch 1 — Deferred)

- All **Medium** and **Low** audit findings across `domain1..domain6`.
- Coverage-table gaps (unimplemented optional SAI attributes).
- **Domain 2**: HIGH 6, 7, 8.
- **Domain 3**: HIGH 1, 2, 6, 8.
- **Domain 4**: R1 (buffer pool watermark `XOFF_ROOM_WATERMARK_BYTES` clear probe), R2 (egress UC-only service pool watermark `egr_pool_rep`).
- **Domain 5**: HIGH 1, 2, 4.
