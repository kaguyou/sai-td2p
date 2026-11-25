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

---

## 8. Round 4 — `APPLY_VIEW` failure from non-port bridge ports in `SAI_BRIDGE_ATTR_PORT_LIST`

### Task 1: Root cause verification (`notes/f5a4_full.log.gz`, `notes/f5a4_bridge_views.txt`) & filtering policy
Lead's reading is **100% confirmed** in `orchagent` and `syncd`:
- **`PortsOrch::removeDefaultBridgePorts()` (`sonic-swss/orchagent/portsorch.cpp:856, 1369-1423`):** runs during `orchagent` warm init (`INIT_VIEW`), allocates `vector<sai_object_id_t> bridge_port_list(m_portCount + m_systemPortCount + 1)` (`:1375`), calls `get_bridge_attribute(m_default1QBridge, SAI_BRIDGE_ATTR_PORT_LIST)` (`:1382`), then calls `get_bridge_port_attribute(bp, SAI_BRIDGE_PORT_ATTR_TYPE)` (`:1401`) on every returned OID and calls `remove_bridge_port(bp)` **only** when `attr.value.s32 == SAI_BRIDGE_PORT_TYPE_PORT` (`:1411-1413`).
- **`Syncd::processQuadInInitViewModeGet()` -> `sendGetResponse()` -> `snoopGetResponse()` (`sonic-sairedis/syncd/Syncd.cpp:2886-2974, 3988, 4105-4270`):**
  1. On `SAI_BRIDGE_ATTR_PORT_LIST`, `snoopGetOidList()` -> `snoopGetOid()` (`:4146, 4242`) inserts a dummy `NULL: NULL` entry into `TEMP_ASIC_STATE` for **every** returned bridge port OID (including tunnel bridge port `oid:0x3a000000000cb9`).
  2. On `SAI_BRIDGE_PORT_ATTR_TYPE`, because `TYPE` is `MANDATORY_ON_CREATE | CREATE_ONLY` (not `READ_ONLY`), `snoopGetAttrValue()` (`:4215, 4234`) writes `SAI_BRIDGE_PORT_ATTR_TYPE = SAI_BRIDGE_PORT_TYPE_TUNNEL` onto `TEMP_ASIC_STATE:SAI_OBJECT_TYPE_BRIDGE_PORT:oid:0x3a000000000cb9`.
  3. Because `TYPE != SAI_BRIDGE_PORT_TYPE_PORT`, `PortsOrch` does **not** remove `cb9` from `TEMP_ASIC_STATE` (unlike the 5 `TYPE_PORT` bridge ports, which it removes). Later `VxlanOrch` restores EVPN and creates a new tunnel bridge port with a new floating VID (giving 7 `BRIDGE_PORT`s in `TEMP_ASIC_STATE` vs 6 in `ASIC_STATE`).
- **`ComparisonLogic::matchOids()` & `performObjectSetTransition()` (`sonic-sairedis/syncd/ComparisonLogic.cpp:187-257, 1436-1620, 1915-1932`):**
  - `matchOids()` finds `0x3a000000000cb9` in both views with the same VID and marks both `SAI_OBJECT_STATUS_MATCHED` (`:216-217`).
  - `performObjectSetTransition()` sees `SAI_BRIDGE_PORT_ATTR_BRIDGE_ID` (`MANDATORY_ON_CREATE | CREATE_AND_SET`, OID attr) on `currentView` `cb9` but absent on the poisoned `temporaryView` `cb9` (`beginTempSizeZero` is `false` because `TYPE` was snooped, and `meta->isoidattribute` is `true`), fails at `:1613`, and throws at `:1930`.

**Per-type decision for `SwitchBCM56860HW::refresh_bridge_port_list()`:**
- **Physical-port `SAI_BRIDGE_PORT_TYPE_PORT`** (`PORT_ID` in `m_port_list`): **listed** (sorted in `m_port_list` order, identical to upstream `SwitchBCM56850`). Required on cold boot so `removeDefaultBridgePorts()` removes default physical bridge ports; on warm boot `removeDefaultBridgePorts()` removes them (`TYPE == PORT`) from `TEMP_ASIC_STATE` and adds them to `m_initViewRemovedVidSet`, so recreated bridge ports match cleanly.
- **LAG-backed `SAI_BRIDGE_PORT_TYPE_PORT`** (`PORT_ID` is a LAG OID, not in `m_port_list`): **omitted (not listed, no throw).** User-created (never a default switch port); listing it risks `SAI_STATUS_BUFFER_OVERFLOW` against `portsorch.cpp:1375`'s fixed `m_portCount + m_systemPortCount + 1` buffer, pollutes `SaiDiscovery` by following `PORT_ID` into user-created `LAG` objects, and risks hardware deletion if `removeDefaultBridgePorts()` runs outside `INIT_VIEW` (`portsorch.cpp:1238`). Leaving it out lets `ComparisonLogic` match the LAG (`findCurrentBestMatchForLag`) and its LAG bridge port (`findCurrentBestMatchForGenericObject`) normally with zero ASIC ops.
- **`SAI_BRIDGE_PORT_TYPE_SUB_PORT`**: **omitted (not listed, no throw).** User-created; `TYPE != PORT`, so listing it would poison `TEMP_ASIC_STATE` with `TYPE=SUB_PORT` and trigger the exact same `MATCHED` crash as `TUNNEL`, plus count against the `m_portCount + m_systemPortCount + 1` buffer.
- **`SAI_BRIDGE_PORT_TYPE_TUNNEL`, `1D_ROUTER`, `1Q_ROUTER`, `NEXT_HOP_GROUP`**: **omitted (not listed, no throw).** None has `TYPE == PORT`; listing any present in `ASIC_STATE` poisons `TEMP_ASIC_STATE` and crashes `APPLY_VIEW`. (The default `1Q_ROUTER` bridge port already has no `BRIDGE_ID` and was already skipped.)

### Task 2: Warm-start RID/VID & `APPLY_VIEW` comparison check
- **`SaiSwitch` warm start (`SaiSwitch.cpp:705-714, 1181-1244`):** `checkWarmBootDiscoveredRids()` only verifies that every RID returned by `SaiDiscovery::discover()` exists in `RID2VID`; it does not require user-created objects in `RID2VID` (tunnels, tunnel maps, VLANs, RIFs, routes, ACLs, tunnel/LAG bridge ports) to be discovered. `helperSaveDiscoveredObjectsToRedis()` is a no-op when `ASIC_STATE` is non-empty. `populateExistingObjects()` (`ComparisonLogic.cpp:2306-2504`) only inspects `m_discovered_rids`, so `cb9` is not injected into `temporaryView`.
- **`ComparisonLogic` matching (`BestCandidateFinder.cpp:1609-1860`, `ComparisonLogic.cpp:1165-2025`):**
  - `cb9` stays `NOT_PROCESSED` in `currentView` with all 5 attributes (`TYPE=TUNNEL`, `TUNNEL_ID`, `BRIDGE_ID`, `ADMIN_STATE=true`, `FDB_LEARNING_MODE=DISABLE`).
  - `ComparisonLogic` processes parent objects first: `BRIDGE` (`0x39..b3e`, `MATCHED`) and the P2P `TUNNEL` (matched by `findCurrentBestMatchForGenericObject` on `TYPE`, `UNDERLAY_INTERFACE`, `ENCAP_SRC_IP`, `ENCAP_DST_IP`; note the `getSaiAttrFromDefaultValue: serialization type SAI_ATTR_VALUE_TYPE_IP_ADDRESS` error at `f5a4_full.log.gz:22525` is a harmless `nullptr` return from `BestCandidateFinder.cpp:3046` when comparing a P2P tunnel candidate against a P2MP tunnel that lacks `ENCAP_DST_IP`).
  - Once the parent P2P `TUNNEL` is `FINAL` (same RID), `temp_bp` (created via `PortsOrch::addBridgePort`, `portsorch.cpp:6506-6537`) matches `cb9` on all 5 attributes (`TYPE`, `TUNNEL_ID` RID, `BRIDGE_ID` RID, `ADMIN_STATE`, `FDB_LEARNING_MODE`). `performObjectSetTransition()` passes with **zero** ASIC operations — **no remove+create** of the tunnel bridge port and zero data-plane disruption (its RID is preserved and remapped to the new VID).

### Task 3: Audit of other `orchagent` warm-init GETs for our EVPN config
Audited every `get_*_attribute` call across `sonic-swss/orchagent/*.cpp`:
- **Default `VLAN` (`removeDefaultVlanMembers`, `portsorch.cpp:1334-1367`):** gets `SAI_VLAN_ATTR_MEMBER_LIST` on `m_defaultVlan` (returns 0 in our config) and unconditionally calls `remove_vlan_member()` on every returned OID, so nothing can remain poisoned in `TEMP_ASIC_STATE`.
- **Switch / Ports / QoS / Buffers / Hash / STP (`main.cpp`, `portsorch.cpp`, `switchorch.cpp`, `bufferorch.cpp`, `routeorch.cpp`, `aclorch.cpp`, `mirrororch.cpp`, `stporch.cpp`):** only queries switch-default `COLDVIDS` objects (`SWITCH`, `CPU_PORT`, `PORT_LIST` + `HW_LANE_LIST`, `DEFAULT_VIRTUAL_ROUTER_ID`, `DEFAULT_1Q_BRIDGE_ID`, `DEFAULT_VLAN_ID`, `ECMP_HASH`/`LAG_HASH`, `DEFAULT_TRAP_GROUP`, `DEFAULT_STP_INST_ID`, queues/IPGs/SGs) or scalar capabilities. All OID-list attrs are `READ_ONLY` (skipped by `snoopGetResponse` at `Syncd.cpp:4184`), and `HW_LANE_LIST` is explicitly skipped (`:4194-4203`). No user objects are returned.
- **VXLAN / EVPN (`vxlanorch.cpp`, `tunneldecaporch.cpp`, `fdborch.cpp`, `intfsorch.cpp`):** **zero** `get_*_attribute` calls. All tunnels, tunnel maps/entries, tunnel term entries, VNIs, RIFs, and FDBs are created fresh in `INIT_VIEW` and matched normally on `APPLY_VIEW`.
- **CoPP (`HOSTIF_TRAP_GROUP` 6 vs 1, `POLICER` 6 vs 0, `HOSTIF_TRAP` 11 vs 1 at `f5a4_full.log.gz:18067-18069, 22518-22520`):** **Confirmed NOT `snoopGetResponse` poisoning — purely "orchagent has not re-created them yet" at `APPLY_VIEW` time.**
  - `CoppOrch::CoppOrch()` (`copporch.cpp:209-214, 370-390`) makes only one SAI GET: `get_switch_attribute(gSwitchId, SAI_SWITCH_ATTR_DEFAULT_TRAP_GROUP)` (the 1 default trap group in temp view), and creates the 1 default `TTL_ERROR` trap (`initDefaultTrapIds()`, `HOSTIF_TRAP` = 1 in temp view). It makes zero GETs on policers, non-default trap groups, or traps.
  - At `18:20:21.172288`, `orchagent` logged `bake: Add warm input: COPP_TABLE, 0` (and `POLICER, 0`) because `coppmgrd` (`coppmgr.cpp:395-403`) wrote `COPP_TABLE` via `ProducerStateTable` (`_COPP_TABLE` staging queue), which `orchagent` only pops in its main event loop **after** `APPLY_VIEW` (not during `Consumer::refillToSync()`). None of the 5 extra trap groups, 6 policers, or 10 traps is in `TEMP_ASIC_STATE` or `MATCHED`, so `APPLY_VIEW` reconciles them cleanly without error and `CoppOrch` re-applies them once `orchagent` enters its main loop.

### Fix (`sairedis/vslib/SwitchBCM56860HW.cpp`)
- Updated `SwitchBCM56860HW::refresh_bridge_port_list()` to remove Pass 2 and return **only** physical-port `SAI_BRIDGE_PORT_TYPE_PORT` bridge ports (whose `PORT_ID` is in `m_port_list`), sorted in `m_port_list` order. All non-physical / user-created bridge ports (`TUNNEL`, `SUB_PORT`, `1D_ROUTER`, `1Q_ROUTER`, `NEXT_HOP_GROUP`, and LAG-backed `TYPE_PORT`) are silently omitted without throwing.
- Expected log line on the `hwstate`-delete test: `recalculated SAI_BRIDGE_ATTR_PORT_LIST: 5 (port=5, omitted=1)` (instead of `6 (port=5, other=1)`), followed by clean `APPLY_VIEW` and `orchagent` warm start state `reconciled`.

### Round 4 build, commit & sha256
- Build: `SAI_TD2P_DIR=$PWD SAIREDIS_DIR=~/homelab/sonic-sairedis-merge ./deploy/build_wb.sh --no-meta --force-archive` — **ALL CHECKS PASSED**.
- Branch: `wb-fallback`
- Code commit: `191f790` (`wb: omit non-physical/user bridge ports from SAI_BRIDGE_ATTR_PORT_LIST`)
- `deploy/build/libsaivs.so.0.0.0` sha256: `3b1688a3ea4cae6d0eb0c9d82f3d4ca7c3a16e9d883c945be7aec83a132fb7c5`
