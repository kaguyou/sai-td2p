# sdk-6.5.27-wb source patches

The warm-boot SDK copy (~/homelab/openbcm-src/sdk-6.5.27-wb, built with BCM_WARM_BOOT_SUPPORT)
differs from sdk-6.5.27 only in these files. Apply with `patch -p1` from the SDK root
(paths are sdk-6.5.27/... vs sdk-6.5.27-wb/..., so use `patch -p1 -d sdk-6.5.27-wb`).

- trident2.c: keep EPC_LINK_BMAP on warm in _soc_trident2_misc_init (8593a21);
  soc_td2_mmu_traffic_ctrl_wb_restore fix so cosq reinit reaches NUM_COS (16c9d22, 2026-11-25:
  NUM_COS was 0 after every warm init -> KNET Case B netif_create PARAM, cosq stats failing).
- link.c: wbtrace syslog in _soc_link_update / soc_link_fwd_set (debug only).
- trident2/cosq.c, include/soc/trident2.h, trident2.c: soc_td2_lls_wb_restore (2026-11-26). The warm init
  skips soc_mmu_init, so the SW-only LLS state built by soc_td2_lls_init was missing
  (_td2_cpu_port_lls_config, _td2p_dyn_sched_unit_data, _td2_invalid_ptr, CPU_ARM_*_QUEUE_BITMAP,
  port_lls_l{0,1,2}_bmap) and any scheduler get/set after a warm boot crashed syncd in
  soc_td2_sched_hw_index_get. The restore rebuilds it read-only from the hardware parent tables.
  After changing SDK sources: rebuild the SDK (podman run ... sdkbuild-img:ready make ... bcm, see
  notes/agy_report_warmboot_phase0.md), then delete vslib/libsaivs.la and vslib/.libs/libsaivs.so* in the
  sairedis build tree before deploy/build_wb.sh --force-archive, or the old .so is kept (no link dependency
  on the merged archive).
