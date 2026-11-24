# Task for agy: warm boot Phase 1 (+ as much of Phase 2 as fits) in ISOLATED trees

Design: `notes/design_warmboot.md` (yours). Phase 0 done: warm-boot SDK at `/home/kaguyou/homelab/openbcm-src/sdk-6.5.27-wb`,
`/home/kaguyou/homelab/sai-td2p/sdk_cflags_wb.txt`, `/home/kaguyou/homelab/as5812-bdeshim/sdk_link_list_wb.txt`, report
`/home/kaguyou/homelab/sai-td2p/notes/agy_report_warmboot_phase0.md` (§8 = how to link).

Work ONLY in these isolated trees (made for you; the lead keeps building/deploying from the originals in parallel):
- `/home/kaguyou/homelab/sai-td2p-wb` (git worktree, branch `warmboot`; you may `git commit` here, never push)
- `/home/kaguyou/homelab/sonic-sairedis-wb` (copy of the build tree)
The podman container `saibuild` mounts `/home/kaguyou/homelab` at `/w`, so both are visible at `/w/sai-td2p-wb`, `/w/sonic-sairedis-wb`.

Do:
1. Create `sai-td2p-wb/deploy/build_wb.sh` = copy of `deploy/build.sh` changed to build `/w/sonic-sairedis-wb/vslib`, link the wb SDK
   (sdk_link_list_wb.txt, sdk-6.5.27-wb paths, sdk_cflags_wb.txt for the hwbcm C flags), and write outputs to `sai-td2p-wb/deploy/build/`.
   It must pass the same checks (ALL CHECKS PASSED) — and additionally confirm the warm-boot SDK symbols are linked.
2. Implement design Phase 1 (hwbcm warm init / warm shutdown / file-backed scache / KNET recover / sgmii state), and Phase 2 if time allows,
   in `sai-td2p-wb/sairedis/vslib/...`; copy changed files into `sonic-sairedis-wb/vslib/...` before building. Keep cold boot behaviour
   identical when no warm-boot request is present (that is what runs in production).
3. Build with build_wb.sh until clean. Commit on branch `warmboot` with clear messages.
4. Report `sai-td2p-wb/notes/agy_report_warmboot_phase1.md`: what is implemented, what is left, build sha256, and the exact hardware test
   steps the lead must run (the lead deploys; you never touch the switch).
Do NOT: touch `/home/kaguyou/homelab/sai-td2p` (the original worktree), `/home/kaguyou/homelab/sonic-sairedis`, the original SDK;
run deploy/install.sh or anything against the switch (no ssh/scp to 192.168.52.1/192.168.0.2, no /dev/ttyUSB0); git push.
