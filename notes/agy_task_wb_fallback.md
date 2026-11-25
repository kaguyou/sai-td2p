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
