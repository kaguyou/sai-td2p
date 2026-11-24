# Task for agy: harden warm boot phase 4 code (no switch access)

Repo/worktree: ~/homelab/sai-td2p-p4 (branch warmboot-phase4). Build tree: ~/homelab/sonic-sairedis-p4.
Build: `SAI_TD2P_DIR=~/homelab/sai-td2p-p4 SAIREDIS_DIR=~/homelab/sonic-sairedis-p4 nice -n 19 deploy/build_wb.sh --no-meta --force-archive`
(copy edited files from sairedis/vslib/ into ~/homelab/sonic-sairedis-p4/vslib/ first, as deploy/README.md says).
Plan / runbook / design: notes/plan_warmboot_phase4.md, notes/runbook_warmboot_phase4.md, notes/design_warmboot.md.

DO NOT: touch the switch (the lead is testing on it), push, modify any other worktree/tree (~/homelab/sai-td2p, sai-td2p-wb,
sai-td2p-merge, sonic-sairedis, sonic-sairedis-wb, sonic-sairedis-merge, openbcm-src/*), use bare git stash.

## Hardware facts so far (2026-11-24, lead)
- syncd warm restart with the "keep KNET running" deinit (warmboot_knet_shutdown_mode=1): data plane is now HITLESS
  (warm init 5.8 s, EPC_LINK_BMAP and links untouched), but CPU RX dies: /proc/bcm/knet/stats Rx0 packets frozen after the
  restart (Tx still increments), switch stops answering ARP. Happens with use_rx_skb=0 (kernel "Fatal error: Incomplete chain"
  on Rx0 while syncd is down) and also with use_rx_skb=1 (now loaded that way; no kernel error, Rx0 still frozen).
- First kexec-mode test (touch /host/warmboot/kexec-pending, then warm restart syncd) segfaulted syncd inside
  hwbcm_warm_deinit_ex: SOC_KNET_MODE(unit) after soc_shutdown (SOC_CONTROL freed). Fixed in commit HEAD (knet_mode cached).
  The lead is re-running that test now with this fix (lib libsaivs.so.p4b).

## Tasks
1. Audit every warm path in hwbcm_core.c and SwitchBCM56860HW*.cpp / _wb.inc.* for the same class of bug: any SDK call or macro that
   reads SOC_CONTROL/BCM_CONTROL/SOC_INFO/SOC_PERSIST after _bcm_shutdown/soc_shutdown (deinit tail, destructor re-sync,
   warm sync after deinit, plugin marker code), double shutdown, use of freed g_* state, and paths where the destructor re-sync runs after
   deinit already tore the unit down. Fix them (keep behaviour otherwise identical), with file:line notes.
2. Find why CPU RX stays dead after a syncd-restart warm init in "live" mode (mode=1/persist=1, KNET kept running) with use_rx_skb=1:
   read the SDK user side (src/soc/common/dma.c, cmicm_dma.c, knet.c, src/bcm/esw/rx.c, init.c) and the kernel module
   (systems/linux/kernel/modules/bcm-knet/bcm-knet.c: rx_skb path, bkn_rxtick, api chains, KCOM DMA_INFO / WB_CLEANUP handling,
   interrupt mask handling) in ~/homelab/openbcm-src/sdk-6.5.27-wb. Which step of warm deinit or warm init stops RX0 or leaves its IRQ
   masked / channel unconfigured? Give file:line evidence and propose the fix. If the "dma-reset" state (mode=0/persist=1 at init after a
   kexec-mode deinit) would be the robust answer for plain syncd restarts too, implement a switch so that a plain syncd warm restart uses
   kexec-mode deinit + dma-reset init by default (keep the old live mode behind an env var HWBCM_WB_KEEP_KNET=1).
3. Build until ALL CHECKS PASSED, save as deploy/build/libsaivs.so.p4c, commit on warmboot-phase4 (message ends with
   "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>" is NOT required for agy; use a plain message), and write
   notes/agy_report_p4_audit.md: findings, changes, sha256 of libsaivs.so.p4c, exact retest steps.
