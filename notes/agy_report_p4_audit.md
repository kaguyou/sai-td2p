# Phase 4 Warm-Boot Audit & TD2+ CPU RX Fix Report

**Date:** 2026-11-24  
**Branch:** `warmboot-phase4` (`~/homelab/sai-td2p-p4`)  
**Code commit:** `43e79c5`  
**Build artifact:** `deploy/build/libsaivs.so.p4c` (copy of `deploy/build/libsaivs.so.0.0.0`)  
**Size:** `150414768` bytes (144 MiB)  
**SHA-256:** `380e7769ac0cac547a8edc81708116fa778f780bfdcc59a239fb8222b42793d4`  
**Build status:** `ALL CHECKS PASSED` (`deploy/build_wb.sh --no-meta --force-archive`)

---

## 1. Task 1 — Warm-Path Lifecycle & Post-`soc_shutdown` Audit

Audited every warm-init, warm-sync, warm-deinit, cold-fallback, destructor, diag-shell, callback, and plugin path across:
- `sairedis/vslib/hwbcm/hwbcm_core.c`
- `sairedis/vslib/hwbcm/hwbcm_l2.c` (and verified all other `hwbcm_*_warm_reset()` implementations)
- `sairedis/vslib/SwitchBCM56860HW.cpp`
- `sairedis/vslib/SwitchBCM56860HW_wb.inc.cpp` / `SwitchBCM56860HW_wb.inc.h`
- `deploy/platform/warm-reboot_plugin`

### Findings & Fixes

| # | Location | Bug / Risk | Fix Applied |
|---|---|---|---|
| 1 | `hwbcm_core.c:2436-2474` (`hwbcm_warm_deinit_ex`) | **Post-`soc_shutdown` KCOM hang/timeout:** The `bkn_rxtick` re-check/re-abort loop (`for (tries = 0; tries < 6; tries++)`) ran *after* `soc_shutdown(unit)` and called `soc_knet_hw_reset(unit, -1)` whenever packet DMA re-armed (which happens with `use_rx_skb=1`). Because `soc_shutdown(unit)` frees `SOC_CONTROL(unit)`, `soc_knet_handle_rx()` (`sdk-6.5.27-wb/src/soc/common/knet.c:236`) immediately rejects every KCOM reply with `if (!SOC_UNIT_VALID(unit)) return SOC_E_UNIT;`, so any re-abort after `soc_shutdown` always timed out (`SOC_E_TIMEOUT`) and marked `.dma_idle` as `busy`. | Moved the `bkn_rxtick` re-check/re-abort loop *before* `soc_shutdown(unit)`. Verified `soc_shutdown` with `warmboot_knet_shutdown_mode=1` does not re-arm KNET packet DMA (`bkn_knet_wb_cleanup`, `bcm-knet.c:9206-9243`, only frees `api_dcb_chain`/`api_dcb_list` and clears `api_active`). |
| 2 | `hwbcm_core.c:2382` (`hwbcm_warm_deinit_ex`) | `SOC_KNET_MODE(unit)` dereferences `SOC_CONTROL(unit)` without checking unit validity first. | Guarded with `knet_mode = (SOC_UNIT_VALID(unit) && SOC_KNET_MODE(unit)) ? 1 : 0;`. |
| 3 | `hwbcm_core.c:404-410` (`hwbcm_riot_enabled`), `hwbcm_core.c:2279` (`hwbcm_warm_sync`) | `hwbcm_riot_enabled()` calls `soc_feature(HWBCM_UNIT, soc_feature_riot)` which dereferences `SOC_CONTROL(unit)`. Note: it is also called during warm/cold init (`hwbcm_core.c:2109`, `:2235`) *before* `g_inited = 1`, so guarding on `!g_inited` would skip `riot_cpu_egress_vlan0_fix()` on cold boot. `hwbcm_warm_sync()` checked only `!g_inited`. | `hwbcm_riot_enabled()` checks `if (!SOC_UNIT_VALID(HWBCM_UNIT)) return 0;` (safe before init, during init after attach, and after `soc_shutdown`). `hwbcm_warm_sync()` checks `if (!g_inited || !SOC_UNIT_VALID(HWBCM_UNIT))`. |
| 4 | `hwbcm_core.c:707-711`, `816-834`, `3661-3676` (`shell_sock_serve`, `shell_sock_stop`, `hwbcm_shell`) | **Race with `soc_shutdown`:** `shell_sock_stop()` closed the listening fd and returned without waiting for an in-flight `shell_sock_serve()` diag command to finish, allowing `hwbcm_shell_cmd()` to access `SOC_CONTROL(unit)` concurrently with `_bcm_shutdown`/`soc_shutdown`. `hwbcm_shell()` checked `g_inited` outside `g_shell_lock` and did not check `SOC_UNIT_VALID`. | `shell_sock_stop()` sets `g_shell_sock_stop = 1`, closes the listen fd, then locks/unlocks `g_shell_lock` to drain any active command before shutdown proceeds. Both `shell_sock_serve()` and `hwbcm_shell()` re-check `!g_inited || !SOC_UNIT_VALID(HWBCM_UNIT)` (and `g_shell_sock_stop` in the socket path) under `g_shell_lock`. |
| 5 | `hwbcm_core.c:2328-2354`, `2503`, `2561` (`hwbcm_core_state_reset`) | **Stale `g_*` globals across deinit/re-init:** `hwbcm_warm_deinit_ex()` and `hwbcm_deinit()` only cleared `g_knet` (in warm deinit), `g_fp_count`, and `g_warm_boot`, leaving `g_desired_speed`, `g_sgmii_applied*`, `g_switch_mac*`, `g_ip2me_*`, `g_trap_l2cache_idx`, `g_hash_*`, `g_knet_state`, and `g_epc_saved_valid` stale. | Added `hwbcm_core_state_reset()` and called it on both `hwbcm_warm_deinit_ex()` and `hwbcm_deinit()`. |
| 6 | `hwbcm_core.c:2085-2107` (`hwbcm_init_ex` warm error tails) & `2295-2297` (`hwbcm_warm_sync`) | Late warm-init failures in `bcm_linkscan_enable_set` / `bcm_linkscan_register` leaked the scache buffer (`hwbcm_scache_cleanup()` missing). `hwbcm_warm_sync()` did not update `g_scache_path` when called with a non-empty override path, so a subsequent `hwbcm_warm_deinit_ex()` could derive `.dma_idle` from a stale/empty `g_scache_path`. | Added `hwbcm_scache_cleanup()` on both error returns in `hwbcm_init_ex[warm]`, and updated `g_scache_path` in `hwbcm_warm_sync()`. |
| 7 | `hwbcm_l2.c:433-444` (`hwbcm_l2_warm_reset`) | Called `bcm_l2_addr_unregister()` unconditionally if `g_l2_cb_registered` was set, even if the unit was already torn down. | Guarded `bcm_l2_addr_unregister()` with `if (SOC_UNIT_VALID(HWBCM_UNIT))`. |
| 8 | `SwitchBCM56860HW.cpp:90-185`, `285-305`, `1218`, `1311` | **Destructor callback/eventfd race & partial-init leak:** `~SwitchBCM56860HW()` closed `m_linkEventFd` *before* `hwWarmSync()` and `hwbcm_warm_deinit_ex()`/`hwbcm_deinit()` unregistered `linkCallbackC`/`l2CallbackC`; an in-flight callback could write to a closed (or reused) fd. Also `hwInitialize()` set `m_hwReady = true` before `eventfd()` and did not call `hwbcm_deinit()` if `eventfd()` failed. | Kept `m_linkEventFd` open until after SDK deinit unregisters callbacks, added `!self->m_linkRun.load()` early-return guards to `linkCallbackC` and `l2CallbackC`, and moved `m_hwReady = true` after `eventfd()` with `(void)hwbcm_deinit()` on failure. |

### Verified Safe (No Change Needed)

- **Post-`soc_shutdown` register read-back (`cmic_dma_busy` at `hwbcm_core.c:2482`):** `soc_pci_read(unit, addr)` (`sdk-6.5.27-wb/src/soc/common/pci.c:173-201`) uses `CMREAD(unit, addr32)` (static `CMVEC(unit)`) and does not dereference `SOC_CONTROL(unit)` because `IPROC_NO_ATL` is not defined in `sdk_cflags_wb.txt`, and `sysconf_detach(unit)` is never called during warm deinit.
- **Post-deinit `*_warm_reset()` calls in `~SwitchBCM56860HW()` (`SwitchBCM56860HW.cpp:171-176`):** Verified `hwbcm_acl_warm_reset` (`hwbcm_acl.c:857`), `hwbcm_copp_warm_reset` (`hwbcm_copp.c:1383`), `hwbcm_l3_warm_reset` (`hwbcm_l3.c:782`), `hwbcm_qos_warm_reset` (`hwbcm_qos.c:1053`), `hwbcm_stats_warm_reset` (`hwbcm_stats.c:695`), and `hwbcm_vxlan_warm_reset` (`hwbcm_vxlan.c:1719`) only zero local software state and make no SDK calls.
- **Double shutdown / destructor re-sync after deinit:** `~SwitchBCM56860HW()` guards the entire block with `if (m_hwReady)` and clears `m_hwReady = false` at exit; `hwbcm_warm_deinit_ex()` and `hwbcm_deinit()` check `if (!g_inited)` under `g_lock` and clear `g_inited = 0` before unlocking; `hwWarmSync()` checks both `m_hwReady` and `g_inited && SOC_UNIT_VALID`.
- **`deploy/platform/warm-reboot_plugin`:** Reads BAR0 directly via `/sys/bus/pci/devices/*/resource0` mmap with matching CMICm register offsets (`0x31140`, `0x31150`, `0x311c4`, `0x311dc`, `0x312c0`, `0x3161c`), samples 3 times over 0.6 s after taking KNET netdevs down, and falls back to `rmmod` / `kexec -u` + cold reboot.

---

## 2. Task 2 — Why CPU RX Dies After Syncd Warm Restart (TD2+ Root Cause)

### Root Cause (with SDK & Kernel `file:line` Evidence)

CPU RX0 dies after warm init in **both** `live` mode (`mode=1, persist=1`) and `dma-reset` mode (`mode=0, persist=1`) because skipping `soc_mmu_init()` on warm boot leaves `CPU_ARM_RSVD_QUEUE_BITMAP(unit, 0)` all zeros, causing the SDK to clear bit 26 (`0x04000000`, CPU L0.0 node) in `CMIC_CMC0_CH1_COS_CTRL_RX_1`:

1. **Where the bit is normally set (cold boot):**
   - During `soc_init()` (`sdk-6.5.27-wb/src/soc/esw/drv.c:10808-10858`), the SDK populates `NUM_CPU_ARM_COSQ(unit, 0) = 48` and `CPU_ARM_QUEUE_BITMAP(unit, 0)` (bits `0..47`), **but does not populate `CPU_ARM_RSVD_QUEUE_BITMAP`**.
   - On TD2+ (`SOC_IS_TD2P_TT2P`), the **only** init-time function that sets `CPU_ARM_RSVD_QUEUE_BITMAP` is `soc_td2_cpu_port_lls_init()` (`sdk-6.5.27-wb/src/soc/esw/trident2/cosq.c:2111-2122`), which runs inside `soc_mmu_init()`:
     ```c
     if (SOC_IS_TD2P_TT2P(unit)) {
         /* CPU  is L0.0 - bit 58 */
         /* ARM0 is L0.1 - bit 59 */
         /* ARM1 is L0.2 - bit 60 */
         SHR_BITSET(CPU_ARM_RSVD_QUEUE_BITMAP(unit, 0), 58);
         if (l2_cnt[1]) {
             SHR_BITSET(CPU_ARM_RSVD_QUEUE_BITMAP(unit, 1), 59);
         }
         if (l2_cnt[2]) {
             SHR_BITSET(CPU_ARM_RSVD_QUEUE_BITMAP(unit, 2), 60);
         }
     }
     ```
   - Bit 58 maps to word `[1]`, bit `58 - 32 = 26` = `0x04000000` in `CMIC_CMC0_CH1_COS_CTRL_RX_1`. On TD2+, all 48 PCI CPU queues hang under L0.0, so CMIC CMC0 CH1 (KNET RX0) **requires bit 26 (`0x04000000`) to be set** in `CMIC_CMC0_CH1_COS_CTRL_RX_1` (`0x0400ffff` total) in order to drain CPU queues to host DMA.

2. **How warm init clobbers `CMIC_CMC0_CH1_COS_CTRL_RX_1`:**
   - Warm init intentionally skips `soc_mmu_init()` (`hwbcm_core.c:1975-1996`, since `soc_td2_lls_init` resets the LLS scheduler tree and drops data-plane forwarding). Consequently, `CPU_ARM_RSVD_QUEUE_BITMAP(unit, 0)` stays `{0, 0}`.
   - In **`live` mode** (`warmboot_knet_shutdown_mode=1, knet_filter_persist=1`):
     - `soc_dma_init()` skips `soc_knet_hw_init()` (`src/soc/common/dma.c:3443-3446`), `cmicm_dma_chan_config()` skips HW init (`src/soc/common/cmicm_dma.c:231-234`), and `_bcm_modules_init()` skips `bcm_esw_knet_init()` (`src/bcm/esw/init.c:861` / `:983`).
     - Then, after `SOC_WARM_BOOT_DONE(unit)`, `hwbcm_init_ex()` calls `bcm_rx_start(unit, NULL)` (`hwbcm_core.c:2045`).
     - `_bcm_common_rx_start()` (`src/bcm/common/rx.c:1362-1375`) calls `_bcm_common_rx_queue_channel_set(unit, -1, first_rx_chan)` (with `first_rx_chan = 1`).
     - `_bcm_common_rx_queue_channel_set()` (`src/bcm/common/rx.c:2467-2482`) computes:
       ```c
       reg_val = 0;
       reg_val |= CPU_ARM_RSVD_QUEUE_BITMAP(unit, cmc)[1]; /* 0 because soc_mmu_init was skipped */
       if (ix == (uint32)chan_id) {
           reg_val |= ((endq >= 32) && (endq < 64)) ?
               (((uint32)1 << (endq % 32)) - 1) : ...;     /* 0x0000ffff for endq=48 */
           if (SOC_IS_TD2P_TT2P(unit)) {
               soc_pci_write(unit, reg_addr, reg_val);     /* writes 0x0000ffff, CLEARING 0x04000000! */
           }
       }
       ```
   - In **`dma-reset` mode** (`warmboot_knet_shutdown_mode=0, knet_filter_persist=1`):
     - `soc_dma_init()` calls `soc_knet_hw_init()` (`KCOM_M_HW_INIT` -> `bkn_knet_hw_init()` at `bcm-knet.c:8456-8569`, which calls `bkn_dma_abort`, `bkn_clean_dcbs`, `bkn_init_dcbs`, `bkn_dma_init` -> `bkn_rx_refill` + `bkn_rx_restart` at `bcm-knet.c:2507-2514`). At this moment `CMIC_CMC0_CH1_COS_CTRL_RX_1` still holds `0x0400ffff` from the previous boot, so RX0 can briefly receive packets.
     - Then `bcm_attach()` -> `bcm_esw_knet_init()` (`src/bcm/esw/knet.c:291`) calls `_bcm_common_rx_queue_channel_set(unit, -1, 1)` — overwriting `CMIC_CMC0_CH1_COS_CTRL_RX_1` to `0x0000ffff` (clearing `0x04000000`).
     - Finally `bcm_rx_start()` (`src/bcm/common/rx.c:1371`) calls `_bcm_common_rx_queue_channel_set(unit, -1, 1)` again, reinforcing `0x0000ffff`.
   - **Hardware confirmation:** Matches the hardware register dump recorded in `notes/agy_task_knet_rx_stall.md` line 18 (`CMIC_CMC0_CH1_COS_CTRL_RX_0/1 = 0xffffffff/0xffff`, CPU MMU `MC_PERQ_PKT` incrementing while KNET `Rx0` packets stay frozen and TX still works).

### Secondary Factor in `live` Mode (Why `dma-reset` Is Also Safer as Default)

- With `use_rx_skb=0`, leaving KNET RX running while syncd is down exhausts the user-API RX DCB chains and triggers `"Fatal error: Incomplete chain"` in `bcm-knet.c`.
- With `use_rx_skb=1`, `dma-reset` (`soc_knet_hw_reset(-1)` at deinit + `KCOM_M_HW_INIT` at init with `knet_filter_persist=1`) cleanly aborts and reinitializes kernel DCBs, reconnects `bkn_isr`, and keeps all 72 KNET netifs and filters intact (matching Case A zero-netif-mutation recovery) while unifying the syncd-restart and kexec deinit/init code paths.

### Fixes Implemented

1. **TD2+ `CPU_ARM_RSVD_QUEUE_BITMAP` restoration (`hwbcm_core.c:1980-1994`):**
   Inside the warm-init `soc_mmu_init`-skipped branch (before `bcm_attach()` and `bcm_rx_start()`):
   ```c
   if (SOC_IS_TD2P_TT2P(unit)) {
       SHR_BITSET(CPU_ARM_RSVD_QUEUE_BITMAP(unit, 0), 58);
       if (NUM_CPU_ARM_COSQ(unit, SOC_ARM_CMC(unit, 0)) > 0) {
           SHR_BITSET(CPU_ARM_RSVD_QUEUE_BITMAP(unit, 1), 59);
       }
       if (NUM_CPU_ARM_COSQ(unit, SOC_ARM_CMC(unit, 1)) > 0) {
           SHR_BITSET(CPU_ARM_RSVD_QUEUE_BITMAP(unit, 2), 60);
       }
   }
   ```
   This restores the exact bitmap `soc_td2_cpu_port_lls_init()` would have set, without touching the MMU/LLS hardware, fixing **both** `dma-reset` and `live` modes.
2. **Diagnostic log (`hwbcm_core.c:2050-2058`):**
   Logs `hwbcm_init_ex[warm]: CMIC_CMC0_CH1_COS_CTRL_RX_0=0x... RX_1=0x...` right after `bcm_rx_start()` (expect `0xffffffff` and `0x0400ffff`).
3. **Default plain syncd warm restarts to `dma-reset` (`SwitchBCM56860HW_wb.inc.cpp:426-434`, `SwitchBCM56860HW_wb.inc.h:53-62`):**
   `warmDeinitForKexec()` now returns `true` by default for plain syncd warm restarts (aborting KNET DMA cleanly and writing `.dma_idle` so the next warm init runs `mode=0, persist=1` `KCOM_M_HW_INIT` while keeping netifs/filters). Setting `HWBCM_WB_KEEP_KNET=1` in syncd's environment opts back into the old `live` mode (unless `PRE_SHUTDOWN`, `HWBCM_WB_KEXEC=1`, or `<warmboot dir>/kexec-pending` forces kexec mode).

---

## 3. Build Verification

Copied the 5 edited files from `~/homelab/sai-td2p-p4/sairedis/vslib/` to `~/homelab/sonic-sairedis-p4/vslib/` and ran:
```bash
SAI_TD2P_DIR=~/homelab/sai-td2p-p4 SAIREDIS_DIR=~/homelab/sonic-sairedis-p4 \
  nice -n 19 deploy/build_wb.sh --no-meta --force-archive
```
Results:
- Zero compiler warnings in `hwbcm_core.c`, `hwbcm_l2.c`, and `SwitchBCM56860HW*.cpp`.
- `ALL CHECKS PASSED`:
  - Undefined symbols: 443 strong undefined, 0 SDK/hwbcm undefined.
  - Symbol versions: `GLIBC <= 2.34` (target 2.36), `GLIBCXX <= 3.4.30` (target 3.4.30), `CXXABI <= 1.3.9` (target 1.3.13).
  - Exported symbols: 67,122 dynamic symbols (`32524 bcm_*`, `204 hwbcm_*`, `45 _bcm_esw_*_sync`, all 14 warm-boot SDK & hwbcm symbols present).
  - `ldd`: all NEEDED libraries resolve (`libswsscommon.so.0 libstdc++.so.6 libc.so.6 libgcc_s.so.1 libm.so.6`).
- Saved to `deploy/build/libsaivs.so.p4c`:
  - `sha256`: `380e7769ac0cac547a8edc81708116fa778f780bfdcc59a239fb8222b42793d4`

---

## 4. Exact Retest Steps (for Lead on Switch)

### A. Install `libsaivs.so.p4c` and Cold Baseline
```bash
# On PC (~/homelab/sai-td2p-p4):
sha256sum deploy/build/libsaivs.so.p4c
# Expect: 380e7769ac0cac547a8edc81708116fa778f780bfdcc59a239fb8222b42793d4
deploy/install.sh --stop --lib deploy/build/libsaivs.so.p4c

# On switch:
sudo systemctl reset-failed swss syncd
sudo systemctl start swss
# Wait for ports & BGP/ARP up; verify cold RIOT log + CPU RX working:
sudo grep -E 'riot:|hwbcm_init: done' /var/log/syslog | tail -n 10
sudo cat /proc/bcm/knet/stats
```

### B. Syncd Warm Restart (Default `dma-reset` Mode)
```bash
# On switch (with PC traffic harness running L2/L3 flows for hitless check):
sudo config warm_restart enable syncd
sudo cat /proc/bcm/knet/stats > /tmp/knet_stats.before
sudo systemctl restart syncd
```
Verify:
1. **Syslog checks:**
   ```bash
   sudo grep -E 'wb:|hwbcm_|COS_CTRL_RX|dma_idle' /var/log/syslog | tail -n 60
   ```
   Expect:
   - Deinit:
     - `wb: dma-reset deinit (default for syncd warm restart; set HWBCM_WB_KEEP_KNET=1 for live mode)`
     - `hwbcm_warm_deinit: mode=KEXEC/dma-reset (abort KNET DMA)`
     - `hwbcm_warm_deinit[kexec]: soc_knet_hw_reset(-1) rv=0 (KNET rings aborted)`
     - `hwbcm_warm_deinit[kexec]: dma_idle=ok (...)` (no segfault, no `SOC_E_TIMEOUT`)
   - Warm init:
     - `wb: knet fresh=0 state=dma-reset (...)`
     - `hwbcm_init_ex[warm]: knet dma-reset -> warmboot_knet_shutdown_mode=0 knet_filter_persist=1`
     - `hwbcm_init_ex[warm]: CMIC_CMC0_CH1_COS_CTRL_RX_0=0xffffffff RX_1=0x0400ffff`  *(bit 26 = 0x04000000 MUST be set!)*
     - `hwbcm_init_ex[warm]: KNET kernel side re-initialised (dma-reset, rx active)`
     - `wb knet: Case A (syncd restart) matched 72 hostif(s) to live KNET netifs`
     - `wb: true warm boot completed`
2. **CPU RX alive check:**
   ```bash
   sudo cat /proc/bcm/knet/stats    # Rx0 packets MUST keep incrementing (compare with /tmp/knet_stats.before)
   sudo cat /proc/bcm/knet/dma      # Rx0 run=1, cur/dirty advancing
   # From PC: ping switch SVI / clear ARP and verify switch replies to ARP
   ```
3. **Data plane:** 0 packet loss on PC traffic harness, no port flaps.
4. Repeat `sudo systemctl restart syncd` 3 times in a row.

### C. Optional: Verify `live` Mode (`HWBCM_WB_KEEP_KNET=1`) Also Keeps CPU RX Alive
To confirm the `CPU_ARM_RSVD_QUEUE_BITMAP` fix independently solves `live` mode too:
- Pass `-e HWBCM_WB_KEEP_KNET=1` to syncd (or test in a container env), run `sudo systemctl restart syncd`, and confirm:
  - `wb: live-mode deinit (keep KNET rings) selected by HWBCM_WB_KEEP_KNET=1`
  - `wb: knet fresh=0 state=live`
  - `hwbcm_init_ex[warm]: CMIC_CMC0_CH1_COS_CTRL_RX_0=0xffffffff RX_1=0x0400ffff`
  - `/proc/bcm/knet/stats` Rx0 packets keep incrementing and ARP works.

### D. Stage 1 / Stage 2 / Stage 4 Kexec-Mode Verification
Follow `notes/runbook_warmboot_phase4.md` Stages 1–4 using `libsaivs.so.p4c`:
- **Stage 1 (`systemctl stop swss syncd` with `warm_restart enable syncd`):** confirm `dma_idle=ok`, `warm-reboot_plugin --check` returns `idle` (`rc=0`), and `/proc/bcm/knet/dma` has no active rings even with `use_rx_skb=1`. Start `swss` and confirm `state=dma-reset` + `RX_1=0x0400ffff` + Rx0 alive.
- **Stage 2 (module reload / simulated kexec):** confirm `state=fresh`, `mode=0 persist=0`, `RX_1=0x0400ffff`, `Case B recreated 72/72`, Rx0 alive, 0 data-plane loss.
- **Stage 4 (real `sudo warm-reboot -v` after `config warm_restart disable syncd`):** confirm `PRE_SHUTDOWN` -> `dma_idle=ok` -> plugin `registers: idle` -> kexec -> `state=fresh`, `RX_1=0x0400ffff`, `Case B recreated 72/72`, Rx0 alive, 0 data-plane loss.
