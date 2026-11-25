# agy task: warm-boot fallback leaves an EMPTY ASIC (found on the switch 2026-11-25)

Repo: ~/homelab/sai-td2p-merge, branch `wb-fallback` (from main 0e11c4f). Build tree: ~/homelab/sonic-sairedis-merge
(copy sairedis/vslib/* there by hand + Makefile.am list; DO NOT run sairedis/sync_from_sairedis.sh).
Build warm variant: `SAI_TD2P_DIR=$PWD SAIREDIS_DIR=~/homelab/sonic-sairedis-merge ./deploy/build_wb.sh --no-meta --force-archive`.
Another agy works in ~/homelab/sai-td2p-p4 (branch qos-sched) at the same time: do not touch that worktree
or ~/homelab/sonic-sairedis-p4. Do not push. The lead tests on the switch; you do not ssh to it.

## What happened
Failure-path test: `.hwstate` sidecar deleted after syncd's PRE_SHUTDOWN sync, then kexec warm-reboot.
After kexec, syncd started with startType WARM (SAI_START_TYPE_WARM_BOOT). Our SAI logged
`warm_boot_initialize_objects: wb: warm boot sidecar files missing or invalid (scache_ok=1 hwstate_ok=0);
falling back to cold SDK init (full ASIC reset, data-plane loss; ...)`, did a cold hwbcm_init_ex, and
returned SUCCESS. syncd then continued its WARM path (it does not re-create objects that are already in
ASIC_DB), so the freshly reset ASIC stayed empty: 0 KNET netdevs (no Ethernet* in the kernel), no routes,
no VLANs, data plane dead, switch unreachable. orchagent was happy ("Create host interface ... oper up").
Code: sairedis/vslib/SwitchBCM56860HW_wb.inc.cpp (warm_boot_initialize_objects and the fallback branch,
grep "falling back to cold SDK init"), SwitchBCM56860HW.cpp (~line 380-395 same messages).

## Goal
A warm boot whose SAI-side state cannot be used must end in a correctly programmed switch.
1. Read how sairedis syncd (~/homelab/sonic-sairedis/syncd/Syncd.cpp, performStartupLogic, onSyncdStart,
   ~line 5527/5820/5928 RESTART_WARM handling, and what happens when create_switch fails on a warm start)
   and SONiC's syncd.sh / swss.sh / warmboot-finalizer react to a failed warm start. Decide between:
   (a) SAI returns failure from create_switch on warm when it cannot do a true warm boot, so syncd exits and
       SONiC does a cold start (check it does not loop: WARM_RESTART_ENABLE_TABLE|system stays true until the
       finalizer clears it — find out and handle, e.g. the SAI/syncd side clearing the flag is NOT acceptable
       unless that is what upstream does);
   (b) SAI does the cold SDK init AND replays every object of the restored SAI DB into hardware (all the
       hw create hooks in dependency order: ports, VLAN, bridge ports, RIF, neighbor, next hop, routes,
       hostif/KNET, ACL, CoPP, QoS, tunnels/VXLAN), so syncd's warm path finds a programmed ASIC.
   Pick the one that is robust and closest to what the Broadcom SAI does; justify it in the report.
2. Implement it. The normal true-warm path and the cold path must not change behaviour.
3. Build (ALL CHECKS must pass), commit on wb-fallback.
4. Give the lead an exact on-box test (the hwstate-delete test: a background script waits for
   /host/warmboot/sai-warmboot.bin.dma_idle then deletes sai-warmboot.bin.hwstate, then
   /home/admin/sai-td2p/wr/warm-reboot -v) with the log lines to expect.

## Report
notes/agy_report_wb_fallback.md: syncd behaviour found (file:line), decision + why, diff summary, commits,
sha256 of deploy/build/libsaivs.so.0.0.0. Short.

## Round 2 (2026-11-25 17:35, lead tested 94772b7c on the box) — NOT passed yet
Logs: notes/f5a2_full.log.gz (syncd/orchagent/swss lines of syslog.1+syslog, clock jumps backwards after kexec).
Sequence (line numbers in the unzipped file):
- 83544 17:08:10 `sidecar files missing ... falling back to cold SDK init + full hardware replay`
- 85093 17:08:52 `hwColdFallbackReplay: ... replay complete in 4397 ms (ports=66 admin_up=66 hostif=66 ...)` — replay itself OK
- 17:08:52 last syncd lines are syncOnLinkMsg newlink for all Ethernet*, then
  `supervisord: WARN exited: syncd (exit status 3; not expected)` at 17:08:55 — syncd DIED ~3 s after the replay.
- 85209 17:09:05 orchagent `Failed to notify syncd INIT_VIEW` -> abort -> swss restart -> syncd starts WARM again
  (85409 17:09:18 fallback again, since hwstate is still missing) -> orchagent timeouts/aborts at 17:10:05 and 17:13:10
  -> finally a cold start at 17:14:54 when the warm flag expired. Total ~6 min of churn; end state OK.
Tasks:
1. Find why syncd exits with status 3 right after a successful replay (look for the ERR/throw just before line ~85200;
   sairedis Syncd warm path: helperDiscover / checkWarmBootDiscoveredRids / RID-VID consistency, onSyncdStart,
   exit codes). A cold ASIC reset may make discovered RIDs differ from the saved ones (default queues, scheduler
   groups, PGs, CPU port, default VLAN/bridge/1Q bridge ports, hostif trap group...). Make the replay keep every RID
   that syncd will compare, or make discovery after fallback report exactly the restored objects.
2. Make a second warm start after a fallback not repeat the fallback forever: after a successful fallback replay,
   (re)write a valid scache/hwstate pair (or otherwise make the next warm start a true warm boot).
3. Rebuild, commit on wb-fallback, append the root cause + fix to notes/agy_report_wb_fallback.md with the new sha256.

## Round 3 (2026-11-25 18:15, lead tested 0eabfdd0 = a79f4b3 on the box) — NOT passed yet, new failure
Logs: notes/f5a3_full.log.gz (syslog from the warm-reboot on; the clock jumps BACK ~3 min after kexec, so read it
in file order, not by timestamp). Round 2 fixes work: `scrubbed 5 stale switch callback pointer(s)`, no segfault,
`persisted fresh scache/hwstate snapshot`, `cold hardware replay complete in 5756 ms (ports=66 ... tunnels=3 vnis=1
tun_bp=1 fdb=1 ... errors=0)`, `initialized switch ... in WARM boot mode`, tunnel oper-up reported after that.
Then:
- `syncd: onSyncdStart: on syncd start took 41.5 sec`
- `ERR syncd: run: Runtime error during syncd init: :- refresh_bridge_port_list: bridge port is missing
  SAI_BRIDGE_PORT_ATTR_PORT_ID, not supported yet, FIXME`
- syncd stays alive but never serves the redis channel -> orchagent `Failed to notify syncd INIT_VIEW` after 60 s
  -> SIGABRT -> swss restart churn.
Cause as far as the lead read it: syncd's warm discovery GETs SAI_BRIDGE_ATTR_PORT_LIST on the default 1Q bridge;
vslib `SwitchBCM56850::refresh_bridge_port_list` (SwitchBCM56850.cpp:467-597, throw at :571) sorts the bridge's
bridge ports by SAI_BRIDGE_PORT_ATTR_PORT_ID and throws for any bridge port without PORT_ID. With EVPN up there is a
TUNNEL bridge port (remote VTEP 10.1.0.2, tun_bp=1) on that bridge.
Tasks:
1. Confirm from the code whether a TRUE warm boot (no fallback) with a remote VTEP up hits the same throw (the
   passing kexec test of 16:19 may have had no tunnel bridge port). Say so in the report either way, and say which
   other callers reach refresh_bridge_port_list at runtime (e.g. orchagent/SaiDiscovery get of PORT_LIST).
2. Fix it in OUR class (override refresh_bridge_port_list in SwitchBCM56860HW; do not edit upstream
   SwitchBCM56850.cpp unless unavoidable): port-type bridge ports sorted by port as today, then every other bridge
   port of that bridge (TUNNEL, SUB_PORT, 1D_ROUTER, 1Q_ROUTER with a bridge id, LAG-backed PORT type whose PORT_ID is
   a LAG oid — check what the size check at :580 does with a LAG) appended in a stable order (oid). No throw, the
   list must contain every bridge port whose BRIDGE_ID is this bridge.
3. Grep the other refresh_* / warm_update_* paths syncd's warm discovery and SaiDiscovery walk (SwitchStateBase.cpp
   refresh_read_only and the BCM56850 overrides) for the same kind of "not supported yet"/THROW on objects we do
   create (tunnels, tunnel maps, VNI bridge ports, RIF on VFI, ACL, mirror, hostif trap groups, scheduler groups,
   buffer/QoS) and fix or report each one, so the next on-box run does not stop at the next throw.
4. A throw during syncd init must not leave a zombie syncd: check Syncd::run's catch (syncd/Syncd.cpp, "Runtime
   error during syncd init") — upstream behaviour is what it is, do not change syncd; just report what it does.
5. Rebuild (ALL CHECKS PASSED), commit on wb-fallback, append "Round 3" (root cause, fix, sha256) to
   notes/agy_report_wb_fallback.md. Same rules as before: do not ssh to the switch, do not install, do not push,
   do not run sairedis/sync_from_sairedis.sh, do not touch ~/homelab/sai-td2p-p4 or ~/homelab/sonic-sairedis-p4.

## Round 4 (2026-11-25 18:40, lead tested f0cf4e10 = fedf6e6 on the box) — NOT passed yet, one step further
Logs: notes/f5a4_full.log.gz (file order, clock jumps back after kexec), notes/f5a4_bridge_views.txt (redis dump of
BRIDGE/BRIDGE_PORT in ASIC_STATE and TEMP_ASIC_STATE + VIDTORID; TEMP was being rebuilt by the next orchagent try
when dumped, the first two TEMP entries are the interesting ones).
Good: `recalculated SAI_BRIDGE_ATTR_PORT_LIST: 6 (port=5, other=1)`, onSyncdStart OK, INIT_VIEW OK, orchagent
`restored`. Then APPLY_VIEW fails (orchagent `Failed to notify syncd APPLY_VIEW -1` -> SIGABRT, loops):
- `ERR syncd: performObjectSetTransition: current attribute is mandatory on create, crate and set, and object MATCHED,
  FIXME oid:0x3a000000000cb9 SAI_BRIDGE_PORT_ATTR_BRIDGE_ID:oid:0x39000000000b3e`
- `ERR syncd: applyView: Exception: processObjectForViewTransition: performObjectSetTransition on MATCHED object
  (oid:0x3a000000000cb9) FAILED! bug?`
- view counts: BRIDGE_PORT current 6, temporary 7.
Lead's reading: 0x3a..cb9 is the TUNNEL bridge port (ASIC_STATE: TYPE=TUNNEL, TUNNEL_ID, BRIDGE_ID=default 1Q,
ADMIN_STATE, LEARNING_MODE). Because PORT_LIST of the default 1Q bridge now contains it, orchagent's warm init
(PortsOrch default bridge port enumeration: get PORT_LIST, then get TYPE of each) touches it in INIT_VIEW mode, so it
lands in TEMP_ASIC_STATE as an "existing" object with only TYPE (`NULL NULL` + `SAI_BRIDGE_PORT_ATTR_TYPE=TUNNEL`).
orchagent then creates its own tunnel bridge port (new VID, hence 7). The comparison matches cb9 by VID and cannot
remove the mandatory BRIDGE_ID -> exception. So listing non-port bridge ports in SAI_BRIDGE_ATTR_PORT_LIST is what
breaks the view comparison.
Tasks:
1. Verify that reading against orchagent (~/homelab/sonic-swss/orchagent/portsorch.cpp, removeDefaultBridgePorts /
   the 1Q bridge PORT_LIST get) and syncd (processQuadInInitViewModeGet, ComparisonLogic). If it is right, change the
   override so SAI_BRIDGE_ATTR_PORT_LIST of a bridge returns ONLY bridge ports that orchagent/syncd can treat as
   default/discoverable ones: TYPE_PORT with a physical port (sorted by port as before); decide from the code whether
   LAG-backed PORT and SUB_PORT ones must be listed or left out for the same reason (they are user-created too) and
   say why. TUNNEL / 1D_ROUTER / 1Q_ROUTER / NEXT_HOP_GROUP bridge ports: not listed, no throw.
   If you find the reading wrong, fix the real cause instead and explain.
2. Check that leaving the tunnel bridge port out of discovery does not break syncd's warm start (RID/VID checks in
   SaiSwitch::helperCheck*/redisUpdate*, "discovered" vs RIDTOVID) or the later comparison (current view cb9 with all
   attrs vs the temp view's newly created tunnel bridge port must match as normal user objects, no remove+create of
   the tunnel bridge port if avoidable; if a remove+create is unavoidable say what the data-plane effect is).
3. Walk the rest of orchagent's warm init GETs the same way for our EVPN config (tunnel, tunnel map, tunnel term,
   VNI, hostif trap group / policer / trap: the log shows trap group 6 vs 1, policer 6 vs 0, trap 11 vs 1 at
   compare time — confirm that is only "orchagent has not re-created them yet" and not the same poisoning).
4. Rebuild (ALL CHECKS PASSED), commit on wb-fallback, append "Round 4" to notes/agy_report_wb_fallback.md with root
   cause, fix, commit, sha256. Same rules: no ssh to the switch, no install, no push, no sync_from_sairedis.sh, do
   not touch ~/homelab/sai-td2p-p4 or ~/homelab/sonic-sairedis-p4.
