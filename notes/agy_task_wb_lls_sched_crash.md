# agy task: syncd SIGSEGV on scheduler operations after a warm boot (found on the switch 2026-11-26)

Repo: ~/homelab/sai-td2p-merge, branch `qos-merge` (f638547 = main d8ff2dc + qos-sched). Work on a new branch
`wb-lls-sched` from it. Build tree: ~/homelab/sonic-sairedis-merge (copy sairedis/vslib/* there with
`cp -a sairedis/vslib/. ../sonic-sairedis-merge/vslib/` and check `diff -rq` shows no differing file BEFORE
building: that tree was stale once; DO NOT run sairedis/sync_from_sairedis.sh).
SDK: ~/homelab/openbcm-src/sdk-6.5.27-wb (patches tracked in deploy/sdk-wb-patches).
Build: `podman start saibuild; SAI_TD2P_DIR=$PWD SAIREDIS_DIR=~/homelab/sonic-sairedis-merge ./deploy/build_wb.sh --no-meta --force-archive`.
Do not push. Do not ssh to the switch, the lead tests there.

## What happened (lib e9a3ba13, details in notes/2026-11-26_qos_hw_results.md)
Cold boot: scheduler set/get works (DWRR weights, shapers verified with traffic; `cos port show` works).
After a kexec warm-reboot (true warm boot, `soc_mmu_init skipped (MMU/LLS kept from cold boot)`):
1. diag `cos port show PortBitMap=xe39` -> syncd SIGSEGV
   `segfault at 0 ip 00007f7410a57599 ... in libsaivs.so.0.0.0[7f741088c000+1d26000]`
2. after syncd's own warm restart (Case A), SAI path: CONFIG_DB `SCHEDULER|t_w2 {type DWRR, weight 2}` +
   `QUEUE|Ethernet39|6 scheduler t_w2` (-> hwbcm_qos_cos_sched_set -> bcm_cosq_gport_sched_set) -> SIGSEGV
   `segfault at 0 ip 00007ff50cd2e599 ... [7ff50cb63000+1d26000]`
Both: exec-segment offset 0x1cb599; exec LOAD has file offset == vaddr 0x1b8d000, so vaddr 0x1d58599 =
`soc_td2_sched_hw_index_get` + 0x89 (deploy/build/nm-defined.txt: 0x1d58510), i.e. inside the inlined
`_soc_td2_alloc_sched` (src/soc/esw/trident2/cosq.c ~1510-1560, 2483), NULL read.
Core of crash 2: ~/homelab/cores/syncd.1795681823.48.core.gz (binary /usr/bin/syncd is in the syncd container
image on the switch, not on the PC; the library with symbols is deploy/build/libsaivs.so.0.0.0, sha e9a3ba13).
Lead's guess, unverified: a SW-only structure that soc_mmu_init / the LLS init builds on a cold boot
(soc->port_lls_l0/l1/l2_bmap from soc_trident2_lls_bmap_alloc, or the per-port LLS config/state used by
_soc_td2_alloc_sched, or _bcm_td2 cosq node state) is missing after warm init. Find the real NULL.

## Goal
1. Root cause with file:line. Check every other SW structure that the skipped soc_mmu_init / LLS / cosq init
   would have built (same class of bug as NUM_COS and _soc_td2_mmu_traffic_ctrl, see 16c9d22 and
   deploy/sdk-wb-patches) and list which are restored on warm and which are not.
2. Fix so that after a warm boot (kexec and syncd-only restart) scheduler get/set, shaper, WRED, buffer
   profile set and the diag `cos port show` work, with ZERO hardware writes during warm init itself
   (rebuild the SW state from hardware / scache). The cold path must not change.
3. Audit the other QoS entry points used by SwitchBCM56860HW_qos.cpp / hwbcm_qos.c after warm
   (bcm_cosq_gport_bandwidth_set, bcm_port_rate_egress_set, bcm_cosq_gport_discard_set, bcm_cosq_control_set,
   bcm_cosq_gport_sched_get, stat paths) for the same missing-state problem; say which you could only
   review, not run.
4. Build (ALL CHECKS PASSED), commit on `wb-lls-sched`, add the SDK change to deploy/sdk-wb-patches.
5. Give the lead an exact on-box test and the log lines to expect.

## Report
notes/agy_report_wb_lls_sched_crash.md: root cause (file:line), table of SW structures restored/not,
diff summary, commits, sha256 of deploy/build/libsaivs.so.0.0.0, on-box test. Short.
