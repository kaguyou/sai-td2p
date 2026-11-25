# agy task: merge warmboot-phase4 into merge-rehearsal (no push, no main)

Repo: ~/homelab/sai-td2p-merge (git worktree, branch `merge-rehearsal`, HEAD ccdd8fc).
Build tree for this worktree: ~/homelab/sonic-sairedis-merge.

## Goal
`merge-rehearsal` = sai-completion + warmboot (already merged) + **warmboot-phase4** (fa3a98b),
building both the cold and the warm-boot libsaivs from this one checkout (ccdd8fc made that possible).
This becomes the candidate for main later. The lead tests it on the switch; you do not.

## Steps
1. `git merge warmboot-phase4` into merge-rehearsal. Resolve conflicts keeping phase-4 behaviour
   (008e52e mmu/bcm_init skip, 7061de7 kexec, b7216bf knet_mode cache, 43e79c5 COS_CTRL_RX_1 fix +
   dma-reset default). Keep ccdd8fc's cold/warm dual build working.
2. Copy the vslib sources into ~/homelab/sonic-sairedis-merge/vslib the same way the previous
   merge did (sairedis/vslib/* -> sonic-sairedis-merge/vslib, Makefile.am source list).
   DO NOT run sairedis/sync_from_sairedis.sh (it deletes sai-td2p/sairedis/vslib).
3. Regenerate `sairedis/vslib.patch` with `git diff` inside ~/homelab/sonic-sairedis-merge.
4. Build both variants:
   - warm: `SAI_TD2P_DIR=$PWD SAIREDIS_DIR=~/homelab/sonic-sairedis-merge ./deploy/build_wb.sh --no-meta --force-archive`
   - cold: the cold build path ccdd8fc documents (deploy/build.sh / README).
   Record sha256 of each libsaivs.so.0.0.0 and confirm `strings` shows the COS_CTRL_RX_1 fix log text
   ("CMIC_CMC0_CH1_COS_CTRL_RX") in the warm one.
5. Commit on merge-rehearsal (no AI co-author lines needed). **Do not push. Do not touch main or the
   other worktrees (sai-td2p, sai-td2p-wb, sai-td2p-p4).**

## Report
Write notes/agy_report_merge_phase4.md: conflicts and how you resolved each, commits, build commands,
both sha256, anything that diverges from warmboot-phase4 in the warm build. Keep it short.
