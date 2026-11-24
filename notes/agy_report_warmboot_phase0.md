# Warm Boot Phase 0 Report — OpenBCM `sdk-6.5.27-wb` Build

**Date:** 2026-11-24
**Scope:** Build a warm-boot-enabled copy of OpenBCM `sdk-6.5.27` (`/home/kaguyou/homelab/openbcm-src/sdk-6.5.27-wb`) without touching the live `/home/kaguyou/homelab/openbcm-src/sdk-6.5.27` tree, `deploy/build.sh`, `sairedis`, `sonic-sairedis`, or the existing `sdkbuild` / `saibuild` containers.

---

## 1. Summary & Status

- **Warm-boot SDK tree:** `/home/kaguyou/homelab/openbcm-src/sdk-6.5.27-wb` — **built cleanly** (`100` static `.a` archives, `platform_defines.o`, `bcm.user.dbg`, `bcm.user`).
- **Warm-boot flags verified active:** `-DBCM_WARM_BOOT_SUPPORT`, `-DBCM_WARM_BOOT_SUPPORT_SW_DUMP`, `-DSCACHE_CRC_CHECK` (plus `-DALPM_ENABLE`, `-fPIC`, `-DINCLUDE_KNET`).
- **All required warm-boot symbols verified** via `nm -g --defined-only` across the `.a` archives and `bcm.user` (`soc_scache_init`, `soc_scache_recover`, `soc_scache_commit`, `soc_stable_set`, `soc_shutdown`, `_bcm_shutdown`, **45** `_bcm_esw_*_sync` symbols, **231** `*_sw_dump` symbols, `soc_scache_hsbuf_crc32_append`, `soc_knet_wb_cleanup`, ALPM warmboot symbols).
- **Generated build artifacts:**
  - `/home/kaguyou/homelab/sai-td2p/sdk_cflags_wb.txt` (8,085 bytes)
  - `/home/kaguyou/homelab/as5812-bdeshim/sdk_link_list_wb.txt` (1,449 bytes)
- **Original SDK & containers untouched:**
  - `/home/kaguyou/homelab/openbcm-src/sdk-6.5.27` newest `.a` mtime remains `2026-11-20 22:03:29`; its `make/Make.local` has no warm-boot flags.
  - Existing containers `sdkbuild` (Exited) and `saibuild` (Up) were not modified; the ephemeral `sdkbuild-wb` container was started with `--rm` mounting only `sdk-6.5.27-wb`.

---

## 2. Commands Used

1. **Copy SDK tree (preserving permissions/timestamps/patches such as `kcom.h` v18 and `Make.config`):**
   ```bash
   cp -a /home/kaguyou/homelab/openbcm-src/sdk-6.5.27 /home/kaguyou/homelab/openbcm-src/sdk-6.5.27-wb
   ```

2. **Enable warm boot in the copy's `make/Make.local` only** (`/home/kaguyou/homelab/openbcm-src/sdk-6.5.27-wb/make/Make.local`):
   ```makefile
   CFGFLAGS += -DBCM_WARM_BOOT_SUPPORT -DBCM_WARM_BOOT_SUPPORT_SW_DUMP -DSCACHE_CRC_CHECK
   ```

3. **Remove copied non-WB build artifacts for a guaranteed full rebuild:**
   ```bash
   rm -rf /home/kaguyou/homelab/openbcm-src/sdk-6.5.27-wb/build \
          /home/kaguyou/homelab/openbcm-src/sdk-6.5.27-wb/systems/linux/user/x86-smp_generic_64-2_6/bcm.user \
          /home/kaguyou/homelab/openbcm-src/sdk-6.5.27-wb/systems/linux/user/x86-smp_generic_64-2_6/bcm.user.dbg
   ```

4. **Build in a dedicated ephemeral container (`localhost/sdkbuild-img:ready`) mounting ONLY the copy:**
   ```bash
   podman run --rm --name sdkbuild-wb \
     -v /home/kaguyou/homelab/openbcm-src/sdk-6.5.27-wb:/src/sdk-6.5.27-wb:z \
     localhost/sdkbuild-img:ready \
     make -C /src/sdk-6.5.27-wb/systems/linux/user/x86-smp_generic_64-2_6 -j12 bcm
   ```
   *(Target `bcm` in `systems/linux/user/x86-smp_generic_64-2_6/Makefile` invokes `systems/linux/user/common/Makefile` with `override-target=unix-user`, `platform=x86-smp_generic_64-2_6`, `kernel_version=2_6`, `LINUX_MAKE_SHARED_LIB=0`, building all static libraries under `build/unix-user/x86-smp_generic_64-2_6/` and linking `bcm.user` without requiring kernel headers.)*

5. **Fix missing link output directory (see §4) and finish link:**
   ```bash
   mkdir -p /home/kaguyou/homelab/openbcm-src/sdk-6.5.27-wb/build/linux/user/x86-smp_generic_64-2_6
   podman run --rm --name sdkbuild-wb \
     -v /home/kaguyou/homelab/openbcm-src/sdk-6.5.27-wb:/src/sdk-6.5.27-wb:z \
     localhost/sdkbuild-img:ready \
     make -C /src/sdk-6.5.27-wb/systems/linux/user/x86-smp_generic_64-2_6 -j12 bcm
   ```

---

## 3. Build Time

| Stage | Timestamps (`+09:00`) | Elapsed |
|---|---|---|
| `cp -a sdk-6.5.27 sdk-6.5.27-wb` (3.0 GiB) | `12:40:15` → `12:40:27` | **12 s** |
| Clean compile & archive (`-j12`, all 100 `.a` libs + `platform_defines.o`) | `12:42:26` → `12:52:45` | **10m 19s** (619 s) |
| Final link (`bcm.user.dbg`, `objcopy --strip-debug` → `bcm.user`, symlinks) | `12:53:10` → `12:53:32` | **22 s** |
| **Total SDK build time** | | **10m 41s** (641 s) |

---

## 4. Build Errors and Fixes

- **Compiler errors:** **0** (all C sources compiled cleanly with `-DBCM_WARM_BOOT_SUPPORT -DBCM_WARM_BOOT_SUPPORT_SW_DUMP -DSCACHE_CRC_CHECK`).
- **Link-step error (1, trivial):**
  ```
  /usr/bin/ld: cannot open output file /src/sdk-6.5.27-wb/build/linux/user/x86-smp_generic_64-2_6/bcm.user.dbg: No such file or directory
  ```
  - **Cause:** On a completely clean `build/` tree, `systems/linux/user/common/Makefile:728` (`$(BCM_TARGET)` rule for the `else` branch where `LINK_LIBS` is empty) does not run `mkdir -p $(@D)` before invoking `$(LINKER)` (unlike the `$(BCM)` rule at line 742 which does).
  - **Fix:** Ran `mkdir -p /home/kaguyou/homelab/openbcm-src/sdk-6.5.27-wb/build/linux/user/x86-smp_generic_64-2_6` and re-ran the `make -j12 bcm` command. Link and strip succeeded immediately with exit code `0` (only the benign static glibc `gethostbyname` warning from `libtest.a(flexport.o)`).

---

## 5. Warm-Boot Symbol Verification (`nm -g --defined-only`)

### Required Core Symbols

| Symbol | Archive | Type | Also in `bcm.user`? |
|---|---|---|---|
| `soc_scache_init` | `libsoccommon.a` | `T` | Yes |
| `soc_scache_recover` | `libsoccommon.a` | `T` | Yes |
| `soc_scache_commit` | `libsoccommon.a` | `T` | Yes |
| `soc_stable_set` | `libsoccommon.a` | `T` | Yes |
| `soc_shutdown` | `libsoc_esw.a` | `T` | Yes |
| `_bcm_shutdown` | `libbcm_common.a` | `T` | Yes |

### Additional Warm-Boot / Scache / CRC / KNET / ALPM / SW_DUMP Symbols Verified

| Symbol / Family | Archive | Type / Count |
|---|---|---|
| `soc_scache_alloc`, `soc_scache_ptr_get`, `soc_scache_handle_used_set`, `soc_scache_detach` | `libsoccommon.a` | `T` |
| `soc_stable_size_set`, `soc_stable_used_get`, `soc_switch_stable_register` | `libsoccommon.a` | `T` |
| `soc_scache_hsbuf_crc32_append` (`-DSCACHE_CRC_CHECK`) | `libsoccommon.a` | `T` |
| `soc_knet_wb_cleanup` (`warmboot_knet_shutdown_mode=1`) | `libsoccommon.a` | `T` |
| `soc_control_defip_scache_sync` | `libsoccommon.a` | `T` |
| `soc_td2p_flexport_scache_sync` | `libsoc_trident2p.a` | `T` |
| `portmod_port_warmboot_db_restore` | `libsoc_portmod.a` | `T` |
| `soc_alpm_warmboot_*` (`lpm_reinit`, `pivot_add`, `prefix_insert`, `bucket_bitmap_set`, etc.) | `libsoc_trident2.a` | **12** symbols (`T`) |
| `*_sw_dump` (`-DBCM_WARM_BOOT_SUPPORT_SW_DUMP`, e.g., `_bcm_cosq_sw_dump`, `_bcm_esw_collector_sw_dump`) | `libbcm_esw.a` et al. | **231** symbols |

### `_bcm_esw_*_sync` Symbols Found (**45 total**)

- **`libbcm_esw.a` (42):**
  `_bcm_esw_collector_sync`, `_bcm_esw_cosq_sync`, `_bcm_esw_extender_sync`, `_bcm_esw_fcoe_sync`, `_bcm_esw_field_scache_sync`, `_bcm_esw_flowtracker_sync`, `_bcm_esw_global_meter_policer_sync`, `_bcm_esw_hash_sync`, `_bcm_esw_ifa_sync`, `_bcm_esw_ipfix_sync`, `_bcm_esw_ipmc_sync`, `_bcm_esw_l2_mac_move_mon_info_sync`, `_bcm_esw_l2_wb_sync`, `_bcm_esw_l2gre_sync`, `_bcm_esw_l3_sync`, `_bcm_esw_link_sync`, `_bcm_esw_mim_sync`, `_bcm_esw_mirror_sync`, `_bcm_esw_mpls_sync`, `_bcm_esw_multicast_sync`, `_bcm_esw_niv_sync`, `_bcm_esw_oam_sync`, `_bcm_esw_port_sync`, `_bcm_esw_qos_sync`, `_bcm_esw_range_scache_sync`, `_bcm_esw_rx_sync`, `_bcm_esw_stat_sync`, `_bcm_esw_stg_sync`, `_bcm_esw_stk_sync`, `_bcm_esw_subport_sync`, `_bcm_esw_svm_pkt_compressed_attr_maps_sync`, `_bcm_esw_switch_agm_scache_sync`, `_bcm_esw_switch_encap_scache_sync`, `_bcm_esw_switch_hash_scache_sync`, `_bcm_esw_switch_scache_sync`, `_bcm_esw_switch_tflow_scache_sync`, `_bcm_esw_telemetry_sync`, `_bcm_esw_trill_sync`, `_bcm_esw_trunk_sync`, `_bcm_esw_udf_scache_sync`, `_bcm_esw_vlan_sync`, `_bcm_esw_vxlan_sync`
- **`libflexctr.a` (2):**
  `_bcm_esw_stat_flex_compressed_attr_flags_sync`, `_bcm_esw_stat_flex_sync`
- **`libtrx.a` (1):**
  `_bcm_esw_virtual_sync`

---

## 6. Archive & Binary Sizes (`sdk-6.5.27` vs. `sdk-6.5.27-wb`)

- **All `.a` archives in `build/unix-user/x86-smp_generic_64-2_6/` (100 files):**
  - Original `sdk-6.5.27`: `148,946,128` bytes (**142.05 MiB**)
  - Warm-boot `sdk-6.5.27-wb`: `151,315,380` bytes (**144.31 MiB**)
  - Delta: **`+2,369,252` bytes (+2.26 MiB, +1.59%)**
- **Linked unique archives from `sdk_link_list_wb.txt` (99 unique `.a` files; excludes empty 8-byte `libdrivers.a`):**
  - Original: `148,946,120` bytes (**142.05 MiB**)
  - Warm-boot: `151,315,372` bytes (**144.31 MiB**)
- **Standalone `bcm.user` / `bcm.user.dbg`:**
  - `bcm.user`: `84,718,488` → `86,097,704` bytes (`+1,379,216` bytes / +1.32 MiB)
  - `bcm.user.dbg`: `84,780,504` → `86,159,888` bytes (`+1,379,384` bytes / +1.32 MiB)

### Top 15 Archive Size Increases

| Archive | Original (bytes) | Warm-Boot (bytes) | Delta (bytes) |
|---|---:|---:|---:|
| `libbcm_esw.a` | 5,566,818 | 6,098,330 | +531,512 |
| `libtrx.a` | 787,224 | 1,199,140 | +411,916 |
| `libbcm.a` | 11,352,020 | 11,713,548 | +361,528 |
| `libtriumph3.a` | 1,488,262 | 1,686,552 | +198,290 |
| `libfirebolt.a` | 946,076 | 1,081,564 | +135,488 |
| `libtriumph2.a` | 715,518 | 821,102 | +105,584 |
| `libtrident2.a` | 1,373,360 | 1,472,846 | +99,486 |
| `libtriumph.a` | 854,994 | 944,574 | +89,580 |
| `libsoccommon.a` | 3,666,672 | 3,748,174 | +81,502 |
| `libflexctr.a` | 452,478 | 523,744 | +71,266 |
| `libtrident.a` | 750,394 | 813,792 | +63,398 |
| `libsoc_esw.a` | 2,916,474 | 2,962,664 | +46,190 |
| `libxgs5.a` | 375,772 | 416,498 | +40,726 |
| `libsoc_phy.a` | 5,599,024 | 5,626,428 | +27,404 |
| `libtrident2plus.a` | 465,226 | 487,442 | +22,216 |

---

## 7. Generated Build Metadata Files

1. **`/home/kaguyou/homelab/sai-td2p/sdk_cflags_wb.txt`**
   - Extracted from the `src/bcm/esw/vlan.c` compile invocation of the `sdk-6.5.27-wb` build in the exact same cleaned format as `sai-td2p/sdk_cflags.txt` (`-MD`/`-MF`/`-g0`/`-DBCM_HIDE_DISPATCHABLE`/`KBUILD_*` stripped, `-DBCM_PLATFORM_STRING="X86"` normalized, all `-I/src/sdk-6.5.27-wb/...` rewritten to `-I/w/openbcm-src/sdk-6.5.27-wb/...` for `saibuild`).
   - Unique flag diff vs. `sdk_cflags.txt` (modulo `sdk-6.5.27` → `sdk-6.5.27-wb`):
     - Added: `-DBCM_WARM_BOOT_SUPPORT`, `-DBCM_WARM_BOOT_SUPPORT_SW_DUMP`, `-DSCACHE_CRC_CHECK`, `-DINCLUDE_KNET`.
     - Removed: none.
   - All 9 unique `-I/w/openbcm-src/sdk-6.5.27-wb/...` paths verified on disk.

2. **`/home/kaguyou/homelab/as5812-bdeshim/sdk_link_list_wb.txt`**
   - Extracted from the `sdk-6.5.27-wb` `bcm.user.dbg` link line (`-Wl,--start-group platform_defines.o ... libsal_appl_plat.a -Wl,--end-group -lnsl -lm -lrt -ldl`).
   - **Important format note:** `as5812-bdeshim/sdk_link_list.txt` stores archive **basenames** relative to `$SDKB` (not full paths), because both `deploy/build.sh` (`$SDKB/$a`) and `hwtest/build.sh` (`$LIBDIR/$w`) prepend the SDK build directory to every non-flag token. The warm-boot build's link list has the **exact same 107 tokens (99 unique `.a` archives + `platform_defines.o` + group/system flags)** as `sdk_link_list.txt`, so `sdk_link_list_wb.txt` preserves that exact basename format for drop-in compatibility.

---

## 8. What Phase 1 Should Change to Link Against the Warm-Boot Copy (Describe Only)

### A. `sai-td2p/deploy/build.sh` (lines 76–80)

Currently (`deploy/build.sh:76-80`):
```bash
SDK=$CT_ROOT/openbcm-src/sdk-6.5.27
SDKB=$SDK/build/unix-user/x86-smp_generic_64-2_6
LINK_LIST=$CT_ROOT/as5812-bdeshim/sdk_link_list.txt
OUT=$CT_ROOT/sai-td2p/deploy/build
MERGED=$OUT/libopenbcm_td2p.a
```

To switch to the warm-boot SDK copy (or support a toggle such as `--wb` / `SDK_WB=1`):
1. Point `SDK` at the warm-boot tree:
   ```bash
   SDK=$CT_ROOT/openbcm-src/sdk-6.5.27-wb
   ```
   (`SDKB` automatically resolves to `$SDK/build/unix-user/x86-smp_generic_64-2_6`.)
2. Point `LINK_LIST` at the warm-boot link list:
   ```bash
   LINK_LIST=$CT_ROOT/as5812-bdeshim/sdk_link_list_wb.txt
   ```
3. **Avoid stale archive reuse:** Either use a distinct merged archive filename (e.g. `MERGED=$OUT/libopenbcm_td2p_wb.a`) **or** run `deploy/build.sh --force-archive` the first time after switching (note: since `sdk-6.5.27-wb/*.a` have newer mtimes than any pre-existing `libopenbcm_td2p.a`, `archive_stale()` will also detect the change automatically when moving forward, though switching back and forth without separate filenames requires `--force-archive`).
4. **Optional post-link sanity checks in `deploy/build.sh`:** Add assertions on `$NM_DEF` verifying `soc_scache_init`, `soc_scache_recover`, `soc_scache_commit`, `soc_stable_set`, `soc_shutdown`, and `_bcm_shutdown` are exported/defined inside `libsaivs.so.0.0.0` once `hwbcm_core.c` references them (note: static archive members are only pulled into `libsaivs.so.0.0.0` once `hwbcm/*.c` references them or calls `bcm_switch_control_set(..., bcmSwitchControlSync, ...)` / `_bcm_shutdown` / `soc_shutdown`).

### B. Companion Compile-Flag Changes Needed in Phase 1 (Outside `deploy/build.sh`)

Linking alone is not enough — `hwbcm/*.c` **must also be compiled** with `-DBCM_WARM_BOOT_SUPPORT` so SDK headers (`include/soc/drv.h`, `include/bcm/init.h`, `include/soc/scache.h`) expose the real `SOC_WARM_BOOT_START`/`SOC_WARM_BOOT_DONE`/`_bcm_shutdown` declarations rather than no-op stubs:
1. **`sonic-sairedis/vslib/Makefile.am`** (and `sai-td2p/sairedis/vslib.patch`, line 38):
   - Change `sdk_cflags.txt` → `sdk_cflags_wb.txt` in `SDK_CFLAGS`:
     ```makefile
     SDK_CFLAGS = $(shell cat $(top_srcdir)/../sai-td2p/sdk_cflags_wb.txt $(top_srcdir)/../sai-td2p/sdk_incs.txt 2>/dev/null)
     ```
   - Also update `sai-td2p/sdk_incs.txt` (or add `sdk_incs_wb.txt` pointing `-I/w/openbcm-src/sdk-6.5.27-wb/...`) so the extra BDE/KNET header dirs (`systems/bde/linux/include`, `systems/bde/linux/user/kernel`) also come from `sdk-6.5.27-wb` rather than `sdk-6.5.27`, then force-recompile `hwbcm/*.o` (`rm -f vslib/hwbcm/*.o` or touch) since `Makefile.am` changes alone may need `automake`/`config.status` or `make -B`.
2. **`sai-td2p/hwtest/build.sh`** (for Phase 1 standalone `hwtest` warm-reload verification):
   - Update `SDK=/w/openbcm-src/sdk-6.5.27-wb`, `/w/sai-td2p/sdk_cflags_wb.txt`, `/w/as5812-bdeshim/sdk_link_list_wb.txt`, and `sdk_incs` paths to match.
