# agy task: two warm-boot bugs found on the switch 2026-11-25 (kexec warm-reboot test)

Repo: ~/homelab/sai-td2p-p4 (branch warmboot-phase4). Build tree ~/homelab/sonic-sairedis-p4.
Warm SDK: ~/homelab/openbcm-src/sdk-6.5.27-wb (our patched copy; rebuild with the podman command in
notes/ or deploy/build_wb.sh). Build: `SAI_TD2P_DIR=$PWD SAIREDIS_DIR=~/homelab/sonic-sairedis-p4 ./deploy/build_wb.sh --no-meta --force-archive`.
Do not push. Do not touch main or the other worktrees. Do not run sairedis/sync_from_sairedis.sh.
The lead tests on the switch; you only build. Commit each fix separately on warmboot-phase4.

## Bug A: NUM_COS is 0 after EVERY warm init (cold = 8)
Evidence (syslog, every warm init incl. plain syncd warm restarts):
- cold: `stats: init num_cos 8 ...`, `qos: non-ETS mode, 8 cos per port reachable via bcm_cosq`
- warm: `stats: init num_cos 10 ...` (fallback, bcm_cosq_config_get failed/0), `qos: ... 0 cos per port`
- diag `cos show` -> `COS: ERROR: Invalid parameter`
- after a host kexec (KNET Case B) every `bcm_knet_netif_create` fails `Invalid parameter`
  (src/bcm/esw/knet.c checks `netif->cosq < max_port_cos_queues` from bcm_esw_cosq_config_get =
  _TD2_NUM_COS = NUM_COS(unit)) -> 0/66 netdevs recreated, CPU path dead.
- also `bcm_cosq_stat_get(...): Invalid parameter` for queue counters after warm.
NUM_COS is restored from scache in src/bcm/esw/trident2/cosq.c reinit (~line 1016, only if
recovered_ver >= BCM_WB_VERSION_1_5) and set on cold at ~8054 from spn_BCM_NUM_COS.
Find out WHY it stays 0 on our warm path (reinit not called for TD2+? different TD2+ cosq file?
version check? our hwbcm_init_ex order — soc_mmu_init skipped, bcm_attach does _bcm_esw_init,
bcm_init not repeated?). Fix at the root, prefer a minimal SDK-wb patch or a hwbcm-side fix with no
hardware writes (bcm_cosq_config_set reprograms COS_MAP/VOQ_COS_MAP — do NOT call it on warm).
Acceptance (lead checks on the box): warm logs show `num_cos 8` and `8 cos per port`, `cos show` works.

## Bug B: 1G copper ports flap after a kexec warm-reboot
After kexec, orchagent re-sends port config (`doPortTask: Set port EthernetX autoneg to on`, speed).
hwbcm_port_speed_set (hwbcm/hwbcm_core.c ~2727) has an early return for "same request, AN on, link up"
but it requires `g_desired_speed[port] == speed_mbps`, which is 0 in a fresh syncd process ->
bcm_port_autoneg_set(1) is re-run on the BCM84858 copper PHY -> link drops (ports 44 and 47, 1G SGMII,
stayed down; PC lost carrier). The warm path already logs `wb sgmii: port N already UP at 1000 Mbps
(SGMII), marked applied`.
Fix: when autoneg is requested and the port already has AN on and link up, do not reprogram anything
(regardless of g_desired_speed); just record g_desired_speed[port] = speed_mbps and keep
g_sgmii_applied as marked by the warm probe. Keep the forced-speed path unchanged.

## Report
notes/agy_report_wb_numcos_autoneg.md: root cause of A (file/line), both diffs summarised, commits,
sha256 of the new deploy/build/libsaivs.so.0.0.0. Short.
