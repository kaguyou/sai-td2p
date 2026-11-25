# sdk-6.5.27-wb source patches

The warm-boot SDK copy (~/homelab/openbcm-src/sdk-6.5.27-wb, built with BCM_WARM_BOOT_SUPPORT)
differs from sdk-6.5.27 only in these two files. Apply with `patch -p1` from the SDK root
(paths are sdk-6.5.27/... vs sdk-6.5.27-wb/..., so use `patch -p1 -d sdk-6.5.27-wb`).

- trident2.c: keep EPC_LINK_BMAP on warm in _soc_trident2_misc_init (8593a21);
  soc_td2_mmu_traffic_ctrl_wb_restore fix so cosq reinit reaches NUM_COS (16c9d22, 2026-11-25:
  NUM_COS was 0 after every warm init -> KNET Case B netif_create PARAM, cosq stats failing).
- link.c: wbtrace syslog in _soc_link_update / soc_link_fwd_set (debug only).
