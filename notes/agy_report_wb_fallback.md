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
