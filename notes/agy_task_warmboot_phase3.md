# Task for agy: warm boot Phase 3 in the isolated trees (same rules as Phase 1)

Continue from your Phase 1/2 commits on branch `warmboot` (`notes/agy_report_warmboot_phase1.md` §3 "What Is Left").
Implement Phase 3: `.hwstate` persistence + recovery for the remaining subsystems — VXLAN/EVPN (VPNs, network/access VPs,
tunnels, VTEP state, RIOT SVI bindings / overlay intfs, overlay egresses), ACL (groups, entries per binding, counters + offsets, ranges),
CoPP (group, entries, policers, stat ids, pending to-me traps), QoS (maps, pools, PG/queue snapshots, scheduler/WRED handles),
stats baselines, mirror sessions/destinations, hash settings — so that after a warm syncd restart every SAI object can still be
set/removed without BCM_E_NOT_FOUND and nothing in hardware is rewritten.
Build with `deploy/build_wb.sh` until ALL CHECKS PASSED, commit on `warmboot`, and write `notes/agy_report_warmboot_phase3.md`
(implemented / left / sha256 / hardware test steps). IMPORTANT: do not overwrite `deploy/build/libsaivs.so.wb-phase2` (the lead is
testing it on the switch); your build output `deploy/build/libsaivs.so.0.0.0` may be overwritten.
Same DO NOTs as Phase 1: no switch access, no changes to /home/kaguyou/homelab/sai-td2p or /home/kaguyou/homelab/sonic-sairedis, no push.
