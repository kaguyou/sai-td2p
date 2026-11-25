# agy report: warm-boot fallback empty-ASIC fix

## 1. `syncd` & SONiC behaviour found

Citations in `~/homelab/sonic-sairedis` and `sai-td2p-merge/deploy/ref`:

- **Warm start selection (`syncd/Syncd.cpp:246-302`):** `performStartupLogic()` forces `m_startType = SAI_START_TYPE_WARM_BOOT` whenever `STATE_DB` `WARM_RESTART_ENABLE_TABLE|system` (or `|syncd`) is `true` (`syncd/syncd_main.cpp:39-43`), unless `SAI_KEY_WARM_BOOT_READ_FILE` (`/var/warmboot/sai-warmboot.bin`) does not exist (`:261-273`, which downgrades to `COLD_BOOT` and runs `HardReiniter::hardReinit()` at `:4996-5018`). `syncd` only checks `access("/var/warmboot/sai-warmboot.bin", F_OK)`; it does not know about our sibling `.scache` / `.hwstate` files.
- **Warm `create_switch` failure (`syncd/Syncd.cpp:5276-5284`, `5727-5767`):** `performWarmRestartSingleSwitch()` calls `SWSS_LOG_THROW` if `create_switch` returns non-success. `Syncd::run()` catches the exception (`:5753`), sends `SWITCH_SHUTDOWN_REQUEST` for `SAI_NULL_OBJECT_ID` on Redis pubsub (`:5400-5430`, missed if `orchagent` is not subscribed yet), replaces `Select` with **only** `m_restartQuery` (`:5759-5761`), and checks `m_commandLineOptions->m_disableExitSleep` (`:5765`).
- **`syncd` parks forever instead of exiting:** On Broadcom, `syncd_init_common.sh:239-326` does **not** pass `-S` (`--disableExitSleep`; defaults `false` in `syncd/CommandLineOptions.cpp:19`). So a `create_switch` failure leaves `syncd` parked forever ignoring `ASIC_STATE`, while `orchagent` either hangs or calls `quick_exit(EXIT_FAILURE)` (`sonic-swss/orchagent/notifications.cpp:55-80`).
- **Restart loop risk:** Even if `syncd` exited (e.g., via `apiInitialize` failing -> `abort()` at `Syncd.cpp:209-215`), `WARM_RESTART_ENABLE_TABLE|system` stays `true` until `warmboot-finalizer` clears it after reconciliation, and `/var/warmboot/sai-warmboot.bin` is still present, so `syncd` would re-enter `SAI_START_TYPE_WARM_BOOT` on every restart and loop.

## 2. Decision: Option (b) — cold SDK init + full hardware replay from `m_objectHash`

- **Why not (a):** With stock SONiC `syncd` (we only ship `libsaivs.so`), returning failure from `create_switch` parks `syncd` permanently instead of triggering a cold restart, and `WARM_RESTART_ENABLE_TABLE|system` cannot be cleared from SAI. (Even upstream Broadcom SAI hits this exact park/crash bug if warm `brcm_sai_create_switch` fails; upstream only gets a clean cold fallback via `HardReiniter` when the single warm-boot file is absent at `Syncd.cpp:267`.)
- **Why (b):** `vslib` has already loaded the complete pre-shutdown SAI DB into `m_objectHash` (preserving all RIDs that `syncd` verifies in `SaiSwitch::helperDiscover()` / `checkWarmBootDiscoveredRids()`). Cold-initializing the SDK and replaying every restored object into hardware inside `SwitchBCM56860HW` achieves the same end state as `HardReiniter::hardReinit()`, requires zero changes to `syncd` or `WARM_RESTART_ENABLE_TABLE`, works for both missing/corrupt sidecars and runtime warm-SDK-attach / `hwStateLoad` failures, and lets `orchagent` + `warmboot-finalizer` finish cleanly.
- Normal true-warm and cold paths are unchanged.

## 3. Diff summary

- `sairedis/vslib/SwitchBCM56860HW.cpp` (`warm_boot_initialize_objects`):
  - If `hwInitialize(true)` succeeds but `buildPortMaps(false)` / `hwWarmRecoverKnet()` / `hwStateLoad()` / `qosBuildHwMap(true)` fails, tears down warm state via `hwTeardownForColdFallback()` and enters the cold-fallback replay instead of aborting `create_switch`.
  - Replaces the old empty-ASIC fallback (`buildPortMaps(true)` + `qosBuildHwMap(false)`) with `hwColdFallbackReplay()`.
- `sairedis/vslib/SwitchBCM56860HW_wb.inc.h`, `SwitchBCM56860HW_wb.inc.cpp`:
  - `hwTeardownForColdFallback()`: stops link thread, runs `hwbcm_deinit()` + module `warm_reset()` calls, clears all C++ handle maps.
  - `hwColdFallbackReplay()`: consumes stale `.dma_idle`, clears warm-shutdown flags, calls `buildPortMaps(false)` (preserving stored port `SPEED`/`MTU` in `m_objectHash`), then replays every subsystem in dependency order:
    1. Switch globals (`SRC_MAC_ADDRESS`, `FDB_AGING_TIME`, hash seeds; skips `RESTART_WARM` / `PRE_SHUTDOWN` / `UNINIT_DATA_PLANE_ON_REMOVAL`) + `HASH` fields
    2. Port pause/flow-control, `SPEED` (+ `AUTO_NEG_MODE`), `MTU`, `PORT_VLAN_ID` (before port-RIF PVID override)
    3. `HOSTIF` (KNET netdevs)
    4. `VLAN`
    5. `LAG`, `LAG_MEMBER`
    6. `VLAN_MEMBER`, `BRIDGE`, non-tunnel `BRIDGE_PORT` FDB learning mode
    7. L3: `VIRTUAL_ROUTER`, `ROUTER_INTERFACE`, `NEIGHBOR_ENTRY`, `NEXT_HOP`, `NEXT_HOP_GROUP`, `NEXT_HOP_GROUP_MEMBER`, `ROUTE_ENTRY` (+ `hwCoppOnRoute`)
    8. VXLAN: `TUNNEL` (P2MP local VTEP pass first, then P2P remote VTEPs), `TUNNEL_MAP`, `TUNNEL_MAP_ENTRY`, `TUNNEL_TERM_TABLE_ENTRY`, tunnel `BRIDGE_PORT`
    9. `FDB_ENTRY` (static local + remote EVPN MACs + RIOT neighbor re-resolve) + `hwVxlanRetryPending()`
    10. CoPP: `POLICER`, `HOSTIF_TRAP_GROUP`, `HOSTIF_TRAP` (+ `hwCoppOnTrapCreate` + `hwCoppRefresh`)
    11. QoS / buffers: `qosBuildHwMap(false)`
    12. Mirror: `MIRROR_SESSION` + port ingress/egress mirror bindings
    13. ACL: `ACL_RANGE`, `ACL_TABLE`, `ACL_TABLE_GROUP`, `ACL_TABLE_GROUP_MEMBER`, `ACL_COUNTER`, bindings (`PORT`/`LAG`/`VLAN`/`RIF`/`SWITCH`), `ACL_ENTRY`
    14. Port `ADMIN_STATE` last + live `OPER_STATUS`/`OPER_SPEED` refresh.
  - Non-fatal per-object errors are logged and counted without aborting the replay.

## 4. On-box test (`hwstate`-delete failure path)

1. Install `deploy/build/libsaivs.so.0.0.0` (`deploy/install.sh --stop`) and verify a normal cold start + normal `warm-reboot` first.
2. On the switch, delete any stale `.dma_idle`, start a background watcher that deletes `.hwstate` as soon as the final destructor writes `.dma_idle` (after final `hwWarmSync()`, before `kexec`), then run `warm-reboot`:
   ```bash
   sudo rm -f /host/warmboot/sai-warmboot.bin.dma_idle
   ( while [ ! -s /host/warmboot/sai-warmboot.bin.dma_idle ]; do sleep 0.05; done
     sudo rm -f /host/warmboot/sai-warmboot.bin.hwstate
     sync
     logger -t wb-test "deleted sai-warmboot.bin.hwstate after dma_idle appeared" ) &
   sudo /home/admin/sai-td2p/wr/warm-reboot -v
   ```
3. Expected syslog lines after kexec (`sudo grep -E 'wb[: ]|wb fallback|hwbcm|syncd#' /var/log/syslog`):
   - `wb: warm boot sidecar files missing or invalid (scache_ok=1 hwstate_ok=0); falling back to cold SDK init + full hardware replay (full ASIC reset, data-plane loss; knet verdict 2, host kexec'd or KNET reloaded)`
   - `initializing Broadcom SDK (COLD) with ...` -> `hwbcm_init: done (...)`
   - `wb fallback: consumed /var/warmboot/sai-warmboot.bin.dma_idle`
   - `wb fallback: cold hardware replay complete in <N> ms (ports=72 admin_up=<M> hostif=72 vlans=<...> ... errors=0)`
   - `initialized switch ... in WARM boot mode`
   - `show warm_restart state` -> all services `reconciled`, `WARM_RESTART_ENABLE_TABLE|system` cleared by `warmboot-finalizer`
   - Post-boot sanity: `ip -br link show | grep -c '^Ethernet'` == 72 (or active port count), `show ip route` / SVI ping / BGP / VXLAN / ACL / CoPP / QoS working, and a subsequent normal `warm-reboot` succeeds with `wb: true warm boot completed`.

## 5. Build, commits & sha256

- Build: `SAI_TD2P_DIR=$PWD SAIREDIS_DIR=~/homelab/sonic-sairedis-merge ./deploy/build_wb.sh --no-meta --force-archive` — **ALL CHECKS PASSED**.
- Branch: `wb-fallback`
- Code commit: `7fa0365` (`wb: replay restored SAI DB into hardware on warm-boot cold fallback`)
- `deploy/build/libsaivs.so.0.0.0` sha256: `94772b7cd6f8e3161352f95a2507d7d624c9cce53b68ddcad921ab9cb9970d75`

---

## 6. Round 2 — Root cause of `syncd` exit status 3 & fix

### Root cause (`notes/f5a2_full.log.gz`)
- Every single one of the 5 post-kexec warm-restart crashes (lines `78266`, `81376`, `82681`, `85097`, `87552`) was a **SIGSEGV** at the exact same stale PIE address `0x559cb1615050` (`error 14` = user instruction fetch on unmapped page):
  - `syncd[13194]: segfault at 559cb1615050 ip 0000559cb1615050 ... error 14 in syncd[5632f6b62000+93000]`
  - `dsserve: child /usr/bin/syncd exited status: 139` (128+11) -> `supervisord: exited: syncd (exit status 3; not expected)`.
- Lines `87547`–`87552` show the exact trigger (1.8 ms apart): `vxlanNotifyTunnelOper: remote VTEP 10.1.0.2: tunnel oid:0x2a00000002 oper up reported` -> `segfault at 559cb1615050`.
- **Why:** In `VirtualSwitchSaiInterface::create()` (`vslib/VirtualSwitchSaiInterface.cpp:708-728`), `init_switch()` runs `SwitchBCM56860HW::warm_boot_initialize_objects()` **before** line `728` calls `ss->create(SAI_OBJECT_TYPE_SWITCH, ..., attr_count, attr_list)` to overwrite `m_objectHash` with the new `syncd` process's live notification callback pointers. During `hwColdFallbackReplay()` step 8, `hwCreateBridgePort()` -> `vxlanTryBringUpVtep()` -> `vxlanNotifyTunnelOper()` (`SwitchBCM56860HW_vxlan.cpp:464-490`) read the **stale pre-kexec** `SAI_SWITCH_ATTR_PORT_STATE_CHANGE_NOTIFY` (`0x559cb1615050`) from `m_objectHash` and enqueued `EVENT_TYPE_NOTIFICATION`. `Sai::eventQueueThreadProc()` (`vslib/SaiEventQueue.cpp:70-71, 100-110`) dispatches `EVENT_TYPE_NOTIFICATION` without acquiring `MUTEX()` and immediately jumped to the dead address. (RID discovery / `SaiDiscovery` was fine — `m_objectHash` already preserves all restored RIDs.)

### Fix
1. **Eliminate stale callback dereference (`SwitchBCM56860HW.cpp`, `SwitchBCM56860HW_wb.inc.cpp`, `SwitchBCM56860HW_vxlan.cpp`, `SwitchBCM56860HW_wb.inc.h`):**
   - `hwWarmScrubStaleCallbacks()` runs at the start of `warm_boot_initialize_objects()` and sets every non-readonly `SAI_ATTR_VALUE_TYPE_POINTER` attribute on `SAI_OBJECT_TYPE_SWITCH` in `m_objectHash` to `NULL` before any warm/fallback hardware work begins.
   - `m_inWarmBootInit` (RAII-guarded) suppresses `vxlanNotifyTunnelOper()` during `warm_boot_initialize_objects()` and records `m_pendingVxlanOperNotify = true`.
   - In `SwitchBCM56860HW::create()` under `SAI_OBJECT_TYPE_SWITCH` (after `SwitchBCM56850::create()` installs the new `syncd` process's live callback pointers), pending tunnel oper-up notifications are emitted for all `vpReady` remote VTEPs.
2. **Prevent repeated fallback on subsequent warm starts (`SwitchBCM56860HW_wb.inc.cpp`):**
   - Step 15 of `hwColdFallbackReplay()` calls `hwWarmSync()` (plus the read path if it differs from the write path) to atomically write a fresh `(.scache, .hwstate, .bootid)` snapshot matching the newly replayed ASIC, then clears `m_warmSynced = false` so future `PRE_SHUTDOWN` / destructor still syncs cleanly. Any subsequent warm start (even an unexpected `syncd` restart without clean warm shutdown) now enters the true warm-boot path.

### Expected new syslog lines on the `hwstate`-delete test
- `wb: scrubbed <N> stale switch callback pointer(s) from restored warm-boot state`
- `wb fallback: persisted fresh scache/hwstate snapshot (/var/warmboot/sai-warmboot.bin) for subsequent warm boots`
- `wb fallback: cold hardware replay complete in <N> ms (... errors=0)`
- `vxlanNotifyTunnelOper: remote VTEP ... oper up reported` (now **after** `initialized switch ... in WARM boot mode`, with zero segfaults)

### Round 2 build, commit & sha256
- Build: `SAI_TD2P_DIR=$PWD SAIREDIS_DIR=~/homelab/sonic-sairedis-merge ./deploy/build_wb.sh --no-meta --force-archive` — **ALL CHECKS PASSED**.
- Branch: `wb-fallback`
- Code commit: `a79f4b3` (`wb: fix stale callback SIGSEGV and persist warm state after cold fallback`)
- `deploy/build/libsaivs.so.0.0.0` sha256: `0eabfdd04968f037879a538b37b01dec24d2f744d5caac455b3a4b2d3c42cb55`

---

## 7. Round 3 — `refresh_bridge_port_list` throw on warm discovery with tunnel/LAG bridge ports

### Root cause (`notes/f5a3_full.log.gz`)
- Round 2 fixes succeeded cleanly (`scrubbed 5 stale switch callback pointer(s)`, `cold hardware replay complete ... (ports=66 ... tunnels=3 vnis=1 tun_bp=1 fdb=1 ... errors=0)`, `persisted fresh scache/hwstate snapshot`, `initialized switch ... in WARM boot mode`, `vxlanNotifyTunnelOper ... oper up reported` with zero segfaults).
- Immediately after `create(SAI_OBJECT_TYPE_SWITCH)` returned, `Syncd::performWarmRestartSingleSwitch()` (`syncd/Syncd.cpp:5294`) constructed `SaiSwitch(..., true)` -> `SaiSwitch::helperDiscover()` (`syncd/SaiSwitch.cpp:52,711`) -> `SaiDiscovery::discover(m_switch_rid)` (`syncd/SaiDiscovery.cpp:249`).
- Walking `SAI_SWITCH_ATTR_DEFAULT_1Q_BRIDGE_ID` -> `SAI_BRIDGE_ATTR_PORT_LIST` entered `SwitchStateBase::get()` -> `refresh_read_only()` (`vslib/SwitchStateBase.cpp:2697-2700`) -> `SwitchBCM56850::refresh_bridge_port_list()` (`vslib/SwitchBCM56850.cpp:467-597`).
- `SwitchBCM56850::refresh_bridge_port_list()` has **two** throws (already flagged by upstream's `// XXX possible issues with vxlan and lag.` at `:473`):
  1. **`:571`:** throws `"bridge port is missing SAI_BRIDGE_PORT_ATTR_PORT_ID, not supported yet, FIXME"` on any bridge port belonging to the bridge without `PORT_ID` — hit immediately by the EVPN remote VTEP's `SAI_BRIDGE_PORT_TYPE_TUNNEL` bridge port (`tun_bp=1`), and equally applicable to `1D_ROUTER`, `1Q_ROUTER` (with `BRIDGE_ID`), and `BRIDGE_PORT_NEXT_HOP_GROUP`.
  2. **`:583`:** throws `"filter by port id failed size on lists is different"` on any `SAI_BRIDGE_PORT_TYPE_PORT` (or `SUB_PORT`) whose `PORT_ID` is a **LAG** OID, because the filter loop at `:563` only scans physical `m_port_list` and omits LAGs.

### Task 1: True warm boot & other runtime callers
- **Confirmed: a TRUE warm boot (no fallback) with a remote VTEP (or LAG bridge port) up hits the exact same throw.** Both true warm boot and cold-fallback replay populate `m_objectHash` from `/var/warmboot/sai-warmboot.bin` and return `SAI_STATUS_SUCCESS` from `create(SWITCH)`, after which `performWarmRestartSingleSwitch()` unconditionally runs `SaiSwitch::helperDiscover()` -> `SaiDiscovery::discover()` -> `get(BRIDGE, default_1q_bridge, SAI_BRIDGE_ATTR_PORT_LIST)` -> `refresh_bridge_port_list()`. The passing 16:19 kexec test did not have a tunnel (or LAG) bridge port on the 1Q bridge.
- **All callers reaching `refresh_bridge_port_list()` at runtime:**
  1. `syncd` `SaiSwitch::helperDiscover()` -> `SaiDiscovery::discover(m_switch_rid)` (`syncd/SaiSwitch.cpp:52,711`, `syncd/SaiDiscovery.cpp:249`) on **every** switch create (cold boot, true warm boot, fallback warm boot, and `SingleReiniter` hard reinit).
  2. `orchagent` (`PortsOrch` default bridge-port enumeration) via `get_bridge_attribute(gDefault1QBridgeId, SAI_BRIDGE_ATTR_PORT_LIST)` during warm restart `INIT_VIEW` (`Syncd::processQuadInInitViewModeGet`, `syncd/Syncd.cpp:2959` calls `m_vendorSai->get()` directly) and during normal `APPLY_VIEW` (`Syncd::processOid` `SAI_COMMON_API_GET`, e.g. on `swss` restart while `syncd` stays up with EVPN/LAG configured).
  3. Any other SAI client or vslib test (`vslib/tests.cpp:441`) calling `get` on `SAI_BRIDGE_ATTR_PORT_LIST`.

### Task 2: Fix (`SwitchBCM56860HW.h`, `SwitchBCM56860HW.cpp`)
- Overrode `SwitchBCM56860HW::refresh_bridge_port_list()` (upstream `SwitchBCM56850.cpp` untouched):
  - Preserves setting `SAI_BRIDGE_PORT_ATTR_BRIDGE_ID = default_1q_bridge_id` on `SAI_BRIDGE_PORT_TYPE_PORT` bridge ports that omit it (using `set_internal()`).
  - Collects every bridge port whose `BRIDGE_ID == bridge_id` in a `std::map<sai_object_id_t, ...>` (ascending OID order), keeping the existing skip of the default `1Q_ROUTER` bridge port that has no `BRIDGE_ID`.
  - **Pass 1:** Emits physical-port `SAI_BRIDGE_PORT_TYPE_PORT` bridge ports sorted in `m_port_list` order (identical to `SwitchBCM56850`).
  - **Pass 2:** Appends every remaining bridge port of that bridge (`TUNNEL`, `SUB_PORT`, `1D_ROUTER`, `1Q_ROUTER` with `BRIDGE_ID`, `BRIDGE_PORT_NEXT_HOP_GROUP`, and LAG-backed `PORT` whose `PORT_ID` is a LAG OID) in stable ascending OID order, with zero throws and exact count parity by construction.
- Verified downstream `syncd` compatibility: `SaiDiscovery::discover()` skips `SAI_BRIDGE_PORT_ATTR_TUNNEL_ID` and `SAI_BRIDGE_PORT_ATTR_RIF_ID` (`SaiDiscovery.cpp:171-172`), handles `NOT_IMPLEMENTED` on `PORT_ID` gracefully (`:188-200`), and `ComparisonLogic::populateExistingObjects()` (`ComparisonLogic.cpp:2394-2458`) treats non-cold-boot-discovered user objects (tunnel BPs, LAG BPs, LAGs) as normal user objects to match against `temporaryView`.

### Task 3: Audit of other `refresh_*` / `warm_update_*` / `SaiDiscovery` paths
Audited all `SWSS_LOG_THROW`, `refresh_read_only()` (`SwitchStateBase.cpp:2553-2761`), `refresh_*`, `warm_update_*` (`SwitchStateBase.cpp:1926-2536`, `SwitchBCM56850.cpp:467-658`), and every RO `OBJECT_ID`/`OBJECT_LIST` attribute in `saimetadata.c` for all object types we create:
- **`TUNNEL` / `TUNNEL_MAP` / `TUNNEL_MAP_ENTRY` / `TUNNEL_TERM_TABLE_ENTRY`:** `SAI_TUNNEL_ATTR_TERM_TABLE_ENTRY_LIST` and `SAI_TUNNEL_MAP_ATTR_ENTRY_LIST` are not in `refresh_read_only()` -> return `SAI_STATUS_NOT_IMPLEMENTED` (no throw; `SaiDiscovery` skips `NOT_IMPLEMENTED` and also skips `BRIDGE_PORT_ATTR_TUNNEL_ID`).
- **VNI / 1D `BRIDGE` & `BRIDGE_PORT`:** `refresh_bridge_port_list` was the only thrower (both `:571` and `:583` fixed above).
- **`ROUTER_INTERFACE` (including RIF on VFI):** no RO OID/OBJLIST attributes; `SaiDiscovery` skips `BRIDGE_PORT_ATTR_RIF_ID`. No throw.
- **ACL (`ACL_TABLE`, `ACL_TABLE_GROUP`, `MEMBER`, `ENTRY`, `COUNTER`, `RANGE`):** `refresh_acl_table_entries()` / `refresh_acl_table_counters()` use safe `findObjects()`; `ENTRY_LIST` / `MEMBER_LIST` / `CHAIN_GROUP_LIST` return `NOT_IMPLEMENTED` (no throw); `ACL_COUNTER` stats handled in `SwitchBCM56860HW::get()`.
- **`MIRROR_SESSION`:** no RO OID/OBJLIST attributes; stored `MONITOR_PORT`/`POLICER` walked cleanly if bound to a port. No throw.
- **`HOSTIF` / `HOSTIF_TRAP` / `HOSTIF_TRAP_GROUP` / `POLICER`:** no RO OID/OBJLIST attributes. No throw.
- **`SCHEDULER_GROUP` / `QUEUE` / `INGRESS_PRIORITY_GROUP` / `BUFFER_POOL` / `BUFFER_PROFILE` / `SCHEDULER` / `WRED` / `QOS_MAP`:** `refresh_scheduler_groups()`, `refresh_qos_queues()`, `refresh_ingress_priority_group()` are no-ops returning `SUCCESS`; `refresh_queue_pause_status()` returns `SUCCESS`; `warm_update_queues()` (`SwitchBCM56850.cpp:599`), `warm_update_scheduler_groups()`, `warm_update_ingress_priority_groups()`, `warm_update_cpu_queues()`, `warm_update_switch()` only iterate `m_port_list`/`m_cpu_port_id` with `CHECK_STATUS` (no throw). `PORT_POOL_LIST` returns `NOT_IMPLEMENTED` (skipped).
- **`LAG` / `LAG_MEMBER`:** `SAI_LAG_ATTR_PORT_LIST` returns `NOT_IMPLEMENTED` (skipped by `SaiDiscovery`); LAG bridge ports on `default_1q_bridge` are now handled without throwing by our `refresh_bridge_port_list()` override.
- **`VLAN` / `VLAN_MEMBER`:** `refresh_vlan_member_list()` (`SwitchStateBase.cpp:2241-2294`) uses `.at(SAI_VLAN_MEMBER_ATTR_VLAN_ID)` which is `MANDATORY_ON_CREATE` on every valid `VLAN_MEMBER`; no throw.
- **`PORT`:** `refresh_port_list()`, `refresh_port_serdes_id()`, `SwitchBCM56860HW::refresh_port_oper_speed()` all use safe `.find()` / `getStoredAttr` with no throws.
- **Conclusion:** `SwitchBCM56850::refresh_bridge_port_list()` (`:571` and `:583`) were the **only** runtime-reachable `"not supported yet"`/THROW sites across `SwitchStateBase`, `SwitchBCM56850`, and `SwitchBCM56860HW`.

### Task 4: Why an init throw leaves a zombie `syncd` (`syncd/Syncd.cpp:5753-5767`)
- `Syncd::run()` catches `std::exception` thrown during `onSyncdStart()` (`:5753`), logs `Runtime error during syncd init: ...` (`:5755`), and calls `sendShutdownRequestAfterException()` (`:5757`, sending `SWITCH_SHUTDOWN_REQUEST` for `oid:0x0` since `m_switches` is still empty — missed if `orchagent` is not yet subscribed).
- It then replaces the event-loop `Select` with **only** `m_restartQuery` (`:5759-5763` — dropping `m_selectableChannel` so `ASIC_STATE` / `INIT_VIEW` requests are never read) and checks `if (m_commandLineOptions->m_disableExitSleep) runMainLoop = false;` (`:5765-5766`).
- Because `m_disableExitSleep` defaults to `false` (`syncd/CommandLineOptions.cpp:19`) and Broadcom's `syncd_init_common.sh` does not pass `-S`, `syncd` enters `while (runMainLoop)` parked on `m_restartQuery` forever instead of exiting. `supervisord` sees `syncd` alive, while `orchagent` times out waiting 60 s for `INIT_VIEW` and `SIGABRT`s in a loop. (Upstream `syncd` behaviour left unchanged.)

### Round 3 build, commit & sha256
- Build: `SAI_TD2P_DIR=$PWD SAIREDIS_DIR=~/homelab/sonic-sairedis-merge ./deploy/build_wb.sh --no-meta --force-archive` — **ALL CHECKS PASSED**.
- Branch: `wb-fallback`
- Code commit: `fedf6e6` (`wb: override refresh_bridge_port_list for tunnel/LAG/non-port bridge ports`)
- `deploy/build/libsaivs.so.0.0.0` sha256: `f0cf4e1059f8b83c7177bfd34f3e71323499683852613efabb27f36348dc36b9`
