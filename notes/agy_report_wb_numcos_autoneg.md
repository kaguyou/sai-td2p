# Warm-Boot Bug Fixes: `NUM_COS == 0` & 1G Copper Autoneg Flap

## Bug A: `NUM_COS` becomes 0 on every warm init

### Root Cause
1. `sdk-6.5.27-wb/src/soc/esw/trident2.c:16094-16120` (`_soc_trident2_mmu_init`): `_fwd_ctrl_lock[unit]` and `_soc_td2_mmu_traffic_ctrl[unit]` (`sizeof(_soc_td2_mmu_traffic_ctrl_t) == _SOC_TD2_MAX_PORT == 106` bytes) are only allocated inside `soc_mmu_init()`.
2. On cold boot, `soc_mmu_init()` allocates `_soc_td2_mmu_traffic_ctrl[unit]`, so during warm sync `bcm_td2_cosq_sync()` (`src/bcm/esw/trident2/cosq.c:1416`) -> `soc_td2_mmu_traffic_ctrl_wb_sync()` (`src/soc/esw/trident2.c:16034-16053`) writes 106 bytes (`thdo_drop_bmp[]`, all zeros) and advances `*scache_ptr` by 106 bytes before line `1419` writes `NUM_COS(unit)` (`8`).
3. On warm init, `hwbcm_init_ex[warm]` (`sairedis/vslib/hwbcm/hwbcm_core.c:1979`) skips `soc_mmu_init()` to avoid resetting TD2+ LLS, leaving `_fwd_ctrl_lock[unit]` and `_soc_td2_mmu_traffic_ctrl[unit]` `NULL`.
4. `bcm_td2_cosq_init()` (`src/bcm/esw/trident2/cosq.c:8054`) initially sets `NUM_COS(unit) = 8`, then line `8424` calls `bcm_td2_cosq_reinit()`. At line `1008`, `soc_td2_mmu_traffic_ctrl_wb_restore()` (`src/soc/esw/trident2.c:16055-16074`) hits `if (mmu_traffic_ctrl == NULL || ...) return SOC_E_NONE;` (line `16065`) and **returns early without advancing `*scache_ptr`**.
5. Line `1016` (`NUM_COS(unit) = *u32_scache_ptr++;`) then reads `thdo_drop_bmp[0..3]` (`0`) instead of the synced `NUM_COS(unit)` (`8`), and shifts all subsequent v1.6–v1.8 cosq scache fields (`scache_pbmp_size`, `fabric_pbmp`, `curr_merger_index`, endpoints, `pool_map`) back by 106 bytes. Subsequent warm syncs also failed to advance `*scache_ptr`, and post-warm calls to `_soc_td2_port_thdo_rx_enable_set()` had a `NULL` mutex/struct.

### Fix Summary
- **SDK-wb (`~/homelab/openbcm-src/sdk-6.5.27-wb/src/soc/esw/trident2.c:16034-16097`):**
  - `soc_td2_mmu_traffic_ctrl_wb_restore()`: allocate `_fwd_ctrl_lock[unit]` and `_soc_td2_mmu_traffic_ctrl[unit]` in SW only (zero HW writes) when `NULL`, restore `thdo_drop_bmp` from scache, and advance `*scache_ptr`.
  - `soc_td2_mmu_traffic_ctrl_wb_sync()`: if `_soc_td2_mmu_traffic_ctrl[unit]` is `NULL`, zero-fill `sizeof(_soc_td2_mmu_traffic_ctrl_t)` bytes and still advance `*scache_ptr` so scache offsets can never desync.
- **hwbcm (`sairedis/vslib/hwbcm/hwbcm_core.c:2038-2056`):**
  - Log `NUM_COS(unit)` after warm `bcm_init`, and add a SW-only guard (no `bcm_cosq_config_set`, zero HW writes) restoring clamped `spn_BCM_NUM_COS` if a pre-fix scache ever synced `0`.

## Bug B: 1G copper ports flap after kexec warm-reboot

### Fix Summary
- **hwbcm (`sairedis/vslib/hwbcm/hwbcm_core.c:2758-2774` in `hwbcm_port_speed_set`):**
  - Removed the `&& g_desired_speed[port] == speed_mbps` condition on the early-return guard. Whenever `autoneg` is requested and the port already has AN enabled and link up, record `g_desired_speed[port] = speed_mbps`, preserve `g_sgmii_applied[port]` as marked by `hwbcm_sgmii_warm_probe()`, and return `0` without calling `bcm_port_autoneg_set(1)` or `bcm_port_interface_set(SGMII)`.
  - Forced-speed path (`autoneg == 0`) is unchanged.

## Commits (`warmboot-phase4`)
- Bug A (hwbcm log/guard paired with `sdk-6.5.27-wb` patch): `16c9d22` — `warmboot: log/guard NUM_COS after warm cosq reinit (pairs with sdk-wb fix)`
- Bug B: `8d046dd` — `hwbcm: skip autoneg reprogram when AN is already on and link is up`

## Build Verification
- Rebuilt `~/homelab/openbcm-src/sdk-6.5.27-wb` (`trident2.o` -> `libsoc_esw.a`, `bcm.user`) cleanly.
- Synced `hwbcm_core.c` into `~/homelab/sonic-sairedis-p4/vslib/hwbcm/hwbcm_core.c`.
- Ran `SAI_TD2P_DIR=$PWD SAIREDIS_DIR=~/homelab/sonic-sairedis-p4 ./deploy/build_wb.sh --no-meta --force-archive` — **ALL CHECKS PASSED**.
- Verified patched `soc_td2_mmu_traffic_ctrl_wb_restore` disassembly and both new `hwbcm_core.c` strings inside `deploy/build/libsaivs.so.0.0.0`.
- **SHA-256 (`deploy/build/libsaivs.so.0.0.0`):**
  `57d9891ffac2abee0c67899ea1982c72858d2a50c17dd69c16835a37ffd7d134`
