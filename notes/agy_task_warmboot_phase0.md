# Task for agy: warm boot Phase 0 — build a warm-boot-enabled copy of the OpenBCM SDK (do not touch the current one)

Context: `notes/design_warmboot.md` (your design). Phase 0 = SDK rebuilt with warm boot support.
The current SDK `/home/kaguyou/homelab/openbcm-src/sdk-6.5.27` and its build output are used by the live SAI build
(`/home/kaguyou/homelab/sai-td2p/deploy/build.sh` links `as5812-bdeshim/sdk_link_list.txt` archives from it). Leave them untouched.

Do:
1. `cp -a /home/kaguyou/homelab/openbcm-src/sdk-6.5.27 /home/kaguyou/homelab/openbcm-src/sdk-6.5.27-wb` (3 GB, disk has room).
2. In the COPY only, add to `make/Make.local`: `CFGFLAGS += -DBCM_WARM_BOOT_SUPPORT -DBCM_WARM_BOOT_SUPPORT_SW_DUMP -DSCACHE_CRC_CHECK`.
3. Rebuild the SDK in the copy the same way the original was built. Find how: `sai-td2p/DESIGN.md` (SDK build section), the memory note
   says it was built inside a Debian 12 podman container (image `sdkbuild-img:ready`, container name `sdkbuild`); check `podman ps -a`,
   `podman images`, `sai-td2p/hwtest/build.sh`, `as5812-bdeshim/` for the exact make command (target unix-user, platform x86-smp_generic_64-2_6).
   If you start a container, mount the copy, not the original. Do not modify or remove the existing `sdkbuild` / `saibuild` containers' state;
   creating a new container for this is fine.
4. Verify in the copy's build output that warm boot code is in: `nm` the archives for `soc_scache_init`, `soc_scache_recover`,
   `soc_scache_commit`, `soc_stable_set`, `soc_shutdown`, `_bcm_shutdown`, and a few `_bcm_esw_*_sync` symbols.
5. Produce `sai-td2p/sdk_cflags_wb.txt` (same format as `sai-td2p/sdk_cflags.txt`, from the copy's build) and
   `as5812-bdeshim/sdk_link_list_wb.txt` (same as `sdk_link_list.txt` but pointing at the copy).
6. Report to `sai-td2p/notes/agy_report_warmboot_phase0.md`: commands used, build time, symbols found, any build errors and fixes,
   size of the archives, and what Phase 1 should change in `deploy/build.sh` to link against the wb copy (describe only; do not edit build.sh).
Do NOT: edit anything under the original sdk-6.5.27, sai-td2p/sairedis, sonic-sairedis, deploy/build.sh; run deploy/build.sh or install;
ssh/scp to the switch (192.168.52.1 / 192.168.0.2) or /dev/ttyUSB0; git commit/push.
