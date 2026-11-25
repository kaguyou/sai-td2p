# Merge Report: warmboot-phase4 into merge-rehearsal

## Merge & Conflicts
- Merged `warmboot-phase4` (`fa3a98b`) into `merge-rehearsal` (previously `ccdd8fc`).
- **Conflicts: none.** `warmboot-phase4` was branched directly from `ccdd8fc`, so `git merge warmboot-phase4` fast-forwarded cleanly (`ccdd8fc..fa3a98b`), preserving `ccdd8fc`'s dual cold/warm build setup alongside all phase-4 commits (`008e52e`, `7061de7`, `b7216bf`, `43e79c5`, `fa3a98b`).
- Note: after `fa3a98b` was merged, `warmboot-phase4` gained `47db4da` (`deploy: 40G (1x40G) QSFP support in gen_lanemap + qsfp_to_40g.py`), which is not included here because the task targeted `fa3a98b`.

## Commits Included on `merge-rehearsal`
- `008e52e` warmboot: skip soc_mmu_init and the duplicate bcm_init on warm init (WIP, under test)
- `7061de7` warmboot phase 4: SONiC warm-reboot (kexec) support
- `b7216bf` warmboot-phase4: fix NULL deref in kexec-mode warm deinit
- `f8614aa` notes: agy task p4 audit
- `2a4d676` notes: agy task knet rx stall
- `43e79c5` warmboot-phase4: harden warm paths, fix TD2+ CPU RX COS_CTRL_RX_1, default dma-reset
- `fa3a98b` notes: add agy_report_p4_audit.md
- Plus this report commit on `merge-rehearsal`.

## vslib Sync & Patch
- Copied `sairedis/vslib/SwitchBCM56860HW*.{h,cpp}` and `sairedis/vslib/hwbcm/*.{c,h}` into `~/homelab/sonic-sairedis-merge/vslib/`.
- Applied `sairedis/vslib.patch` cleanly to `~/homelab/sonic-sairedis-merge` (bringing in the `vslib/Sai.cpp` `apiUninitialize` timing log from `7061de7`; `vslib/Makefile.am` already listed all sources).
- Regenerated `sairedis/vslib.patch` via `git diff -- vslib` inside `~/homelab/sonic-sairedis-merge`; result was byte-for-byte identical to `fa3a98b`.

## Builds & Verification
Both variants built cleanly against `~/homelab/sonic-sairedis-merge` (cold first, warm second so `deploy/build/libsaivs.so.0.0.0` is left as the warm-boot default).

1. **Cold (`sdk-6.5.27`)**
   - Command:
     ```bash
     SAI_TD2P_DIR=/home/kaguyou/homelab/sai-td2p-merge SAIREDIS_DIR=/home/kaguyou/homelab/sonic-sairedis-merge /home/kaguyou/homelab/sai-td2p-merge/deploy/build.sh --no-meta --force-archive
     ```
   - Result: ALL CHECKS PASSED (148,919,920 bytes, 66,629 defined symbols, 32,449 `bcm_*`, 204 `hwbcm_*`, GLIBC <= 2.34, GLIBCXX <= 3.4.30, CXXABI <= 1.3.9)
   - `sha256`: `fc4182b72b61df243a0d0e843495ffb768b18fe6ab6c3fec9b34c0dfcc8ac883`

2. **Warm (`sdk-6.5.27-wb`)**
   - Command:
     ```bash
     SAI_TD2P_DIR=/home/kaguyou/homelab/sai-td2p-merge SAIREDIS_DIR=/home/kaguyou/homelab/sonic-sairedis-merge /home/kaguyou/homelab/sai-td2p-merge/deploy/build_wb.sh --no-meta --force-archive
     ```
   - Result: ALL CHECKS PASSED (150,414,744 bytes, 67,122 defined symbols, 32,524 `bcm_*`, 204 `hwbcm_*`, 45 `_bcm_esw_*_sync`, all 14 warm-boot symbols defined)
   - `sha256`: `8ad8d5a1fb52e8f07fd2e4dd32ae3449e5bd193a3bc52767272a2a6388bee1d6`
   - `strings` check on warm `libsaivs.so.0.0.0`:
     - `hwbcm_init_ex[warm]: CMIC_CMC%d_CH1_COS_CTRL_RX_0=0x%08x RX_1=0x%08x` (COS_CTRL_RX_1 fix log from `43e79c5`)
     - `CMIC_CMC0_CH1_COS_CTRL_RX_0`, `CMIC_CMC0_CH1_COS_CTRL_RX_1`

## Divergence from `warmboot-phase4` (`fa3a98b`) in Warm Build
- **None in source, patch, or symbol tables:** `sairedis/vslib/*`, `sairedis/vslib.patch`, `deploy/build/nm-defined.txt`, and `deploy/build/nm-undefined.txt` are byte-for-byte identical to `fa3a98b`.
- The `.so` sha256 (`8ad8d5a1...`) differs from `43e79c5`'s `libsaivs.so.p4c` (`380e7769...`) only due to build-tree paths/debug metadata (`sonic-sairedis-merge` vs the p4 build tree).
