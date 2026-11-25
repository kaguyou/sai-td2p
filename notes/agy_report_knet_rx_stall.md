# KNET Rx0 DMA Stall on Warm Boot — Root Cause Analysis and Verification Report

**Target:** BCM56864 (Trident2+, `SOC_IS_TD2P_TT2P(unit)`) running `sdk-6.5.27-wb`, `linux-kernel-bde`, `linux-user-bde`, and `bcm-knet.ko` (`use_rx_skb=1`).  
**Constraints respected:** Read-only analysis of `openbcm-src` and `sai-td2p-p4`; no switch access, no git push, and no edits to `hwbcm_core.c` or any SDK files.

> [!NOTE]
> **Repository status note:** While this task's prompt ([agy_task_knet_rx_stall.md](file:///home/kaguyou/homelab/sai-td2p-p4/notes/agy_task_knet_rx_stall.md)) was written before any `hwbcm_core.c` change, a separate audit pass modified [hwbcm_core.c](file:///home/kaguyou/homelab/sai-td2p-p4/sairedis/vslib/hwbcm/hwbcm_core.c#L1975-L1994) on 2026-11-24 17:10 to add `SHR_BITSET(CPU_ARM_RSVD_QUEUE_BITMAP(unit, 0), 58);` when `soc_mmu_init()` is skipped on warm boot (plus `CMIC_CMC%d_CH1_COS_CTRL_RX_0/1` logging at [hwbcm_core.c](file:///home/kaguyou/homelab/sai-td2p-p4/sairedis/vslib/hwbcm/hwbcm_core.c#L2050-L2058)). This report analyzes the exact pre-fix hardware observations (`COS_CTRL_RX_1 = 0x0000ffff`) and confirms how that fix addresses both modes `(a)` and `(b)`.

---

## Executive Summary

1. **Disjoint DMA memories (Q1):** `bcm-knet`'s Rx0 DCB ring at DMA address `0x7f6b9040` and its SKB packet buffers live in **kernel coherent/streaming DMA memory** allocated inside `bcm-knet.ko`, completely separate from `linux-kernel-bde`'s `dmasize=32M` pool mapped into userspace by `linux-user-bde`. A fresh userspace process during warm init cannot allocate, zero, or overwrite `bcm-knet`'s Rx0 ring, and with `use_rx_skb=1`, `KCOM_M_DMA_INFO` leaves `start_dma = 0` so HW `CMIC_CMC0_DMA_DESC1` is untouched.
2. **Primary bug causing Rx0 stall in both `(a)` and `(b)` (Q2):** On TD2+ (`BCM56864`), PCI CPU port L0.0 (`hw_index=259`) is mapped to COS/queue **bit 58** — which is **bit 26 (`0x04000000`)** of `CMIC_CMC0_CH1_COS_CTRL_RX_1` (`0x31174`), making its healthy value **`0x0400ffff`**. Only `soc_td2_cpu_port_lls_init()` sets bit 58 in `CPU_ARM_RSVD_QUEUE_BITMAP(unit, 0)`. Because warm boot skips `soc_mmu_init()`, that bitmap word remained `0`. Then `bcm_esw_knet_init()` (in `(b)`) and `_bcm_common_rx_start()` (in **both `(a)` and `(b)`**) called `_bcm_common_rx_queue_channel_set(unit, -1, 1)`, overwriting `CMIC_CMC0_CH1_COS_CTRL_RX_1` from `0x0400ffff` to **`0x0000ffff`**, cutting off all CPU L0.0 traffic to CMICm CH1 while `MC_PERQ_PKT` kept incrementing in MMU.
3. **Why `(b)` consumed 5 descriptors (`0x7f6b9180`, `dirty 3`) before freezing (Q2):** Neither `soc_knet_hw_reset(-1)` nor `soc_dma_chan_config()` clears `COS_CTRL_RX_1`. When `soc_dma_init()` -> `soc_knet_hw_init()` restarted CH1 DMA at `0x7f6b9040`, `COS_CTRL_RX_1` still held cold-boot `0x0400ffff`, allowing 3 packets to complete (`dirty 3, free 61`) and CMICm to prefetch to DCB 5 (`0x7f6b9180`) before `bcm_esw_knet_init()` / `bcm_rx_start()` clobbered `COS_CTRL_RX_1` to `0x0000ffff`.

---

## Question 1 — Memory Ownership and Warm-Init Isolation

### 1.1 Where `bcm-knet`'s Rx0 DCB ring (`0x7f6b9040`) and RX buffers live (`use_rx_skb=1`)

* **DCB rings (`sinfo->dcb_mem` / `sinfo->dcb_dma`):** Allocated inside `bcm-knet.ko` in [`bkn_alloc_dcbs()`](file:///home/kaguyou/homelab/openbcm-src/sdk-6.5.27-wb/systems/linux/kernel/modules/bcm-knet/bcm-knet.c#L1924-L1938) via `BKN_DMA_ALLOC_COHERENT` -> `dma_alloc_coherent(sinfo->dma_dev, sinfo->dcb_mem_size, &dcb_dma, GFP_ATOMIC | GFP_DMA32)` ([bcm-knet.c:488](file:///home/kaguyou/homelab/openbcm-src/sdk-6.5.27-wb/systems/linux/kernel/modules/bcm-knet/bcm-knet.c#L488)).
  * `MAX_TX_DCBS = 64`, `MAX_RX_DCBS = 64` ([bcm-knet.c:556-562](file:///home/kaguyou/homelab/openbcm-src/sdk-6.5.27-wb/systems/linux/kernel/modules/bcm-knet/bcm-knet.c#L556-L562)).
  * On CMICm (`sinfo->dcb_wsize = 16`, `dcb_size = 64` bytes), the Tx ring occupies `(64 + 1) * 64 = 4160 = 0x1040` bytes, immediately followed by `rx_chans` Rx rings of `0x1040` bytes each.
  * Therefore, with Tx0 base `0x7f6b8000`, **Rx0 starts at `0x7f6b8000 + 0x1040 = 0x7f6b9040`**.
  * Once allocated, `bkn_knet_hw_init()` skips reallocation while `sinfo->dcb_mem != NULL` ([bcm-knet.c:8514](file:///home/kaguyou/homelab/openbcm-src/sdk-6.5.27-wb/systems/linux/kernel/modules/bcm-knet/bcm-knet.c#L8514)).
* **RX packet buffers (`use_rx_skb=1`):** Allocated dynamically per descriptor in kernel space by [`bkn_rx_refill()`](file:///home/kaguyou/homelab/openbcm-src/sdk-6.5.27-wb/systems/linux/kernel/modules/bcm-knet/bcm-knet.c#L2372-L2405) via `dev_alloc_skb(...)` and mapped to PCI DMA addresses via `BKN_DMA_MAP_SINGLE(...)`.

### 1.2 Where the user-side SDK allocates DCBs and packet buffers

1. Userspace SDK calls [`soc_cm_salloc()`](file:///home/kaguyou/homelab/openbcm-src/sdk-6.5.27-wb/src/soc/common/cm.c#L7974-L8025), which dispatches via `CMVEC(unit).salloc` -> `sysconf`'s [`_salloc()`](file:///home/kaguyou/homelab/openbcm-src/sdk-6.5.27-wb/src/appl/diag/sysconf.c#L115-L119) -> `linux-user-bde`'s [`_salloc()`](file:///home/kaguyou/homelab/openbcm-src/sdk-6.5.27-wb/systems/bde/linux/user/linux-user-bde.c#L1629-L1643) (`mpool_alloc(_dma_pool, size)`).
2. During [`_open()`](file:///home/kaguyou/homelab/openbcm-src/sdk-6.5.27-wb/systems/bde/linux/user/linux-user-bde.c#L1219-L1229), `linux-user-bde` calls [`_get_dma_info()`](file:///home/kaguyou/homelab/openbcm-src/sdk-6.5.27-wb/systems/bde/linux/user/linux-user-bde.c#L600-L623) (`ioctl(LUBDE_GET_DMA_INFO)`), `mmap`s `[_cpu_pbase, _cpu_pbase + _dma_size)`, and creates `_dma_pool` over that userspace mapping.
3. In kernel `linux-user-bde` ([linux-user-bde.c:1313-1318](file:///home/kaguyou/homelab/openbcm-src/sdk-6.5.27-wb/systems/bde/linux/user/kernel/linux-user-bde.c#L1313-L1318), [1741-1765](file:///home/kaguyou/homelab/openbcm-src/sdk-6.5.27-wb/systems/bde/linux/user/kernel/linux-user-bde.c#L1741-L1765)), `LUBDE_GET_DMA_INFO` queries [`lkbde_get_dma_info()`](file:///home/kaguyou/homelab/openbcm-src/sdk-6.5.27-wb/systems/bde/linux/kernel/linux_dma.c#L1261-L1272) in `linux-kernel-bde`, which returns the single `dmasize=32M` pool allocated at module load in [`linux_dma.c:1047-1089`](file:///home/kaguyou/homelab/openbcm-src/sdk-6.5.27-wb/systems/bde/linux/kernel/linux_dma.c#L1047-L1089) / [`_mpool_alloc()`](file:///home/kaguyou/homelab/openbcm-src/sdk-6.5.27-wb/systems/bde/linux/kernel/linux_dma.c#L830-L898) / [`_dma_per_device_init()`](file:///home/kaguyou/homelab/openbcm-src/sdk-6.5.27-wb/systems/bde/linux/kernel/linux_dma.c#L929-L1015).

### 1.3 Can a fresh user process during warm init touch KNET's Rx0 DCB ring?

* **Directly: No.** The userspace `mmap` window and `mpool` are strictly bounded to `[ _cpu_pbase, _cpu_pbase + _dma_size )` (`dmasize=32M` from `linux-kernel-bde`), whereas `sinfo->dcb_mem` (`0x7f6b8000..`) and `dev_alloc_skb` pages are allocated independently inside `bcm-knet.ko` / kernel slab/page allocators.
* **Via `KCOM_M_DMA_INFO` (`use_rx_skb=1`): No.** When the warm-booting process calls `bcm_rx_start()` and queues API DVs (`KCOM_M_DMA_INFO`), [`bkn_knet_dma_info()`](file:///home/kaguyou/homelab/openbcm-src/sdk-6.5.27-wb/systems/linux/kernel/modules/bcm-knet/bcm-knet.c#L8200-L8249) calls [`bkn_api_rx_restart()`](file:///home/kaguyou/homelab/openbcm-src/sdk-6.5.27-wb/systems/linux/kernel/modules/bcm-knet/bcm-knet.c#L2204-L2228), which only attaches `api_dcb_chain` (`api_active = 1`) and leaves `start_dma = 0` because DMA restart is gated by `if (sinfo->rx[chan].use_rx_skb == 0)`. HW `CMIC_CMC0_DMA_DESC1` stays `0x7f6b9040` and `sinfo->rx[0].desc` is never repointed or zeroed by userspace.
* **In `(b) dma-reset`:** Userspace explicitly calls `soc_knet_hw_reset(-1)` (`warmboot_knet_shutdown_mode=0`) and `soc_dma_init()` -> [`soc_knet_hw_init()`](file:///home/kaguyou/homelab/openbcm-src/sdk-6.5.27-wb/src/soc/common/dma.c#L3473), triggering [`bkn_knet_hw_init()`](file:///home/kaguyou/homelab/openbcm-src/sdk-6.5.27-wb/systems/linux/kernel/modules/bcm-knet/bcm-knet.c#L8456-L8569) in the kernel to clean/re-init `sinfo->dcb_mem` (`0x7f6b9040`), refill 64 new SKBs (`bkn_rx_refill`), and restart CH1 DMA (`bkn_rx_restart`). That is kernel-driven re-initialization, not userspace overwriting KNET memory.

---

## Question 2 — Why Rx0 Consumed 5 Descriptors and Stalled in Both `(a)` and `(b)`

### 2.1 Primary root cause: `CMIC_CMC0_CH1_COS_CTRL_RX_1` bit 26 (queue bit 58) cleared on warm boot

On BCM56864 (`SOC_IS_TD2P_TT2P(unit)`), PCI CPU port scheduler node L0.0 (`hw_index = 259`) is mapped to queue/COS mask **bit 58**:

* [`src/bcm/esw/trident2/cosq.c:288-291`](file:///home/kaguyou/homelab/openbcm-src/sdk-6.5.27-wb/src/bcm/esw/trident2/cosq.c#L288-L291):
  * `bcm_td2p_cosq_rx_chan_l0_map[BCM_RX_CHANNELS] = {-1, 259, 260, 261};` (channel `1` = `259`, i.e., CPU L0.0)
  * `bcm_td2p_cmic_to_mmu_cosmask_map[...] = {58, 59, 60, 61, 62, 63};`
* [`src/soc/esw/trident2/cosq.c:2111-2122`](file:///home/kaguyou/homelab/openbcm-src/sdk-6.5.27-wb/src/soc/esw/trident2/cosq.c#L2111-L2122) (`soc_td2_cpu_port_lls_init`, called from `soc_td2_lls_init` at line `2379` inside `_soc_trident2_mmu_init` at [trident2.c:16125](file:///home/kaguyou/homelab/openbcm-src/sdk-6.5.27-wb/src/soc/esw/trident2.c#L16125)):
  ```c
  if (SOC_IS_TD2P_TT2P(unit)) {
      /* CPU  is L0.0 - bit 58 */
      /* ARM0 is L0.1 - bit 59 */
      /* ARM1 is L0.2 - bit 60 */
      SHR_BITSET(CPU_ARM_RSVD_QUEUE_BITMAP(unit, 0), 58);
  ```
  `58 - 32 = 26`, which corresponds to **bit 26 (`0x04000000`)** of `CMIC_CMC0_CH1_COS_CTRL_RX_1` (`0x31174`, paired with `CMIC_CMC0_CH1_COS_CTRL_RX_0` at `0x31170` per [allregs_c.i:403300-403541](file:///home/kaguyou/homelab/openbcm-src/sdk-6.5.27-wb/src/soc/mcm/allregs_c.i#L403300-L403541)).

* **What broke during warm boot (pre-fix):**
  1. `soc_td2_cpu_port_lls_init()` is the **only** site in the cold-init path that populates `CPU_ARM_RSVD_QUEUE_BITMAP(unit, 0)`.
  2. During `hwbcm_init_ex()` warm boot, `soc_mmu_init(unit)` is skipped to avoid resetting live MMU/LLS queues, so `CPU_ARM_RSVD_QUEUE_BITMAP(unit, 0)` remained **`0`** (`word[1] == 0x00000000`).
  3. Later in warm boot, [`_bcm_common_rx_queue_channel_set(unit, -1, 1)`](file:///home/kaguyou/homelab/openbcm-src/sdk-6.5.27-wb/src/bcm/common/rx.c#L2463-L2482) was called by:
     * [`bcm_esw_knet_init()`](file:///home/kaguyou/homelab/openbcm-src/sdk-6.5.27-wb/src/bcm/esw/knet.c#L291) (runs in **`(b)`** when `warmboot_knet_shutdown_mode=0`), **and**
     * [`_bcm_common_rx_start()`](file:///home/kaguyou/homelab/openbcm-src/sdk-6.5.27-wb/src/bcm/common/rx.c#L1371) via `bcm_rx_start()` after `SOC_WARM_BOOT_DONE` (runs in **both `(a)` and `(b)`**).
  4. `_bcm_common_rx_queue_channel_set()` builds `reg_val` from scratch:
     ```c
     reg_val = 0;
     reg_val |= CPU_ARM_RSVD_QUEUE_BITMAP(unit,cmc)[1];
     ...
     if (SOC_IS_TD2P_TT2P(unit)) {
         soc_pci_write(unit, reg_addr, reg_val);
     }
     ```
     With `CPU_ARM_RSVD_QUEUE_BITMAP(unit, 0)[1] == 0`, it overwrote cold-boot **`0x0400ffff`** with **`0x0000ffff`**, clearing bit 26 (`0x04000000`).
  5. With bit 58 cleared in `CMIC_CMC0_CH1_COS_CTRL_RX_1`, CMICm CH1 (`KNET Rx0`) no longer qualifies/accepts packets from CPU L0.0 (`hw_index=259`), while MMU keeps punting packets into CPU queues (so `MC_PERQ_PKT(0/1/4)` keeps incrementing).

### 2.2 Timeline explaining all observed HW counters in `(b) dma-reset` and `(a) live`

* **In `(b) dma-reset` (`warmboot_knet_shutdown_mode=0`):**
  1. `soc_knet_hw_reset(-1)` and `soc_dma_chan_config()` do **not** touch `CMIC_CMC0_CH1_COS_CTRL_RX_0/1`.
  2. When [`soc_dma_init()`](file:///home/kaguyou/homelab/openbcm-src/sdk-6.5.27-wb/src/soc/common/dma.c#L3442-L3474) runs [`soc_knet_hw_init()`](file:///home/kaguyou/homelab/openbcm-src/sdk-6.5.27-wb/src/soc/common/knet.c#L456) -> [`bkn_knet_hw_init()`](file:///home/kaguyou/homelab/openbcm-src/sdk-6.5.27-wb/systems/linux/kernel/modules/bcm-knet/bcm-knet.c#L8456-L8569), `bkn_rx_refill()` fills all 64 RX SKBs (`cur` wraps back to `0`, `free = 64`), `bkn_rx_restart()` programs `CMIC_CMC0_DMA_DESC1 = 0x7f6b9040` and starts CH1 (`run = 1`) **while `CMIC_CMC0_CH1_COS_CTRL_RX_1` still holds `0x0400ffff` from cold boot**.
  3. During the brief interval between `soc_dma_init()` and `bcm_esw_knet_init()` / `bcm_rx_start()`, buffered/live CPU packets drain through CH1: 3 descriptors complete (`dirty: 0 -> 3`, `free: 64 -> 61`, 3 RX packets processed), while CMICm's descriptor prefetch advances `CMIC_CMC0_CH1_DMA_CURR_DESC` by 5 descriptors (`0x7f6b9040 + 5 * 0x40 = 0x7f6b9180`).
  4. Immediately afterward, `bcm_esw_knet_init()` (and then `bcm_rx_start()`) writes `0x0000ffff` to `CMIC_CMC0_CH1_COS_CTRL_RX_1`. Delivery from CPU L0.0 halts mid-chain: DCB 3 never receives its `DONE` bit (`CH1_DESC_DONE=0, CH1_DMA_ACTIVE=1`), `CH1_DMA_CURR_DESC` remains stuck at `0x7f6b9180`, and `bkn_do_skb_rx()` ([bcm-knet.c:4141, 4509-4518](file:///home/kaguyou/homelab/openbcm-src/sdk-6.5.27-wb/systems/linux/kernel/modules/bcm-knet/bcm-knet.c#L4509-L4518)) exits immediately at `dirty=3` whenever polled. Because non-CDMA `bkn_do_skb_rx()` only refills in [`bkn_rx_chain_done()`](file:///home/kaguyou/homelab/openbcm-src/sdk-6.5.27-wb/systems/linux/kernel/modules/bcm-knet/bcm-knet.c#L4683) rather than per-DCB, `cur` stays `0`, `dirty` stays `3`, and `free` stays `61`.
  5. Meanwhile, Tx runs on CH0 and generates TX interrupts; on every interrupt, [`xgsm_do_dma()`](file:///home/kaguyou/homelab/openbcm-src/sdk-6.5.27-wb/systems/linux/kernel/modules/bcm-knet/bcm-knet.c#L5251-L5286) polls all active Rx channels (`bkn_do_rx`), so `Interrupts` and `Tx packets` continue rising while `Rx0 packets` remains frozen.

* **In `(a) live` (`warmboot_knet_shutdown_mode=1`, `use_rx_skb=1`):**
  * KNET Rx0 DMA stays running cleanly while `syncd` restarts, **until** `bcm_rx_start()` -> `_bcm_common_rx_start()` calls `_bcm_common_rx_queue_channel_set(unit, -1, 1)` ([rx.c:1371](file:///home/kaguyou/homelab/openbcm-src/sdk-6.5.27-wb/src/bcm/common/rx.c#L1371)), which overwrites `CMIC_CMC0_CH1_COS_CTRL_RX_1` to `0x0000ffff` and freezes the already-running CH1 ring wherever it happens to be.

* **Why cold `syncd` restart works:**
  * Cold init runs `soc_mmu_init()` -> `_soc_trident2_mmu_init()` -> `soc_td2_lls_init()` -> `soc_td2_cpu_port_lls_init()`, setting bit 58 in `CPU_ARM_RSVD_QUEUE_BITMAP(unit, 0)` so `_bcm_common_rx_queue_channel_set()` programs `CMIC_CMC0_CH1_COS_CTRL_RX_1 = 0x0400ffff`.

* **Why `(a)` with `use_rx_skb=0` printed `"Fatal error: Incomplete chain"` while `syncd` was down:**
  * With `use_rx_skb=0`, `bcm-knet` runs RX DMA over userspace `soc_cm_salloc` DCB chains queued via `KCOM_M_DMA_INFO` (`bkn_api_rx_restart`, [bcm-knet.c:2204-2228](file:///home/kaguyou/homelab/openbcm-src/sdk-6.5.27-wb/systems/linux/kernel/modules/bcm-knet/bcm-knet.c#L2204-L2228)).
  * Because `(a)` uses `warmboot_knet_shutdown_mode=1`, KNET Rx0 stays active while `syncd` exits and tears down userspace SDK DMA/KCOM; once the active userspace chain hits `CHAIN_DONE` with `chain_complete == 0` and no `syncd` process is alive to post new valid DCBs/complete the chain, [`bkn_rx_chain_done()`](file:///home/kaguyou/homelab/openbcm-src/sdk-6.5.27-wb/systems/linux/kernel/modules/bcm-knet/bcm-knet.c#L4646-L4676) exhausts `rx_sync_retry` retries and prints `gprintk("Fatal error: Incomplete chain\n")` + `bkn_rx_debug_dump()`.
  * Consequently, `use_rx_skb=0` is incompatible with keeping KNET RX DMA active across a userspace restart; **`use_rx_skb=1` is required for `(a)`**.

---

## Question 3 — Minimal Robust Fix and `< 5 min` Hardware Confirmation

### 3.1 Minimal robust fix for `(a)` and `(b)`

1. **Primary fix (already present in [hwbcm_core.c](file:///home/kaguyou/homelab/sai-td2p-p4/sairedis/vslib/hwbcm/hwbcm_core.c#L1975-L1994) as of 2026-11-24 17:10):**
   * During `hwbcm_init_ex()` warm boot when `soc_mmu_init(unit)` is skipped on `SOC_IS_TD2P_TT2P(unit)`, set `SHR_BITSET(CPU_ARM_RSVD_QUEUE_BITMAP(unit, 0), 58)` before `bcm_attach()` / `bcm_rx_start()` execute.
   * Because both `bcm_esw_knet_init()` (hit in `(b)`) and `_bcm_common_rx_start()` (hit in both `(a)` and `(b)`) run **after** this point and read `CPU_ARM_RSVD_QUEUE_BITMAP(unit, 0)[1]`, both `(a)` and `(b)` now write **`0x0400ffff`** instead of `0x0000ffff` to `CMIC_CMC0_CH1_COS_CTRL_RX_1`.
2. **Configuration pairing & operational recommendation:**
   * **Keep `use_rx_skb=1` for `(a)`** (`warmboot_knet_shutdown_mode=1`, `knet_filter_persist=1`), which keeps kernel netifs/filters and kernel SKB RX rings intact across `syncd` exit/re-attach without `"Fatal error: Incomplete chain"`.
   * **`(b)`** (`warmboot_knet_shutdown_mode=0`) also recovers RX delivery once bit 58 is set, though `(a)` remains preferable in production to avoid transiently resetting Rx DMA and kernel netif state during warm `syncd` restart.
3. **Optional upstream-grade SDK hardening (if later hardening `openbcm-src`):**
   * Restore `CPU_ARM_RSVD_QUEUE_BITMAP` in TD2+ warm boot inside `_soc_trident2_mmu_init()` / `soc_td2_cpu_port_lls_init()` warm-boot re-sync, or preserve reserved upper bits (`58..63` / `CPU_ARM_RSVD_QUEUE_BITMAP`) inside [`_bcm_common_rx_queue_channel_set()`](file:///home/kaguyou/homelab/openbcm-src/sdk-6.5.27-wb/src/bcm/common/rx.c#L2463-L2482) when `SOC_WARM_BOOT(unit)` is active so future `bcm_rx_*` callers cannot zero cold-boot bits.

### 3.2 Hardware Confirmation Experiment (`< 5 min`, zero code change needed to test live)

Verify the exact mechanism on the switch using `bcmcmd` (diag shell) and `/proc/bcm/knet/*` ([`bcm-knet.c:7332-7677, 8056-8080`](file:///home/kaguyou/homelab/openbcm-src/sdk-6.5.27-wb/systems/linux/kernel/modules/bcm-knet/bcm-knet.c#L7332-L7677)):

1. **Baseline check (cold healthy state):**
   ```sh
   bcmcmd "getreg CMIC_CMC0_CH1_COS_CTRL_RX_0"   # expect 0xffffffff
   bcmcmd "getreg CMIC_CMC0_CH1_COS_CTRL_RX_1"   # expect 0x0400ffff (bit 26 / bit 58 set)
   cat /proc/bcm/knet/stats
   cat /proc/bcm/knet/dma
   ```
2. **Verify pre-fix stalled box (or pre-fix build) during warm-boot stall:**
   ```sh
   bcmcmd "getreg CMIC_CMC0_CH1_COS_CTRL_RX_0"   # 0xffffffff
   bcmcmd "getreg CMIC_CMC0_CH1_COS_CTRL_RX_1"   # bug shows 0x0000ffff (bit 26 cleared!)
   cat /proc/bcm/knet/dma                        # Rx0 frozen (e.g. dirty 3, cur 0, free 61, run 1)
   cat /proc/bcm/knet/stats                      # Rx0 packets frozen; Tx/Interrupts rising
   ```
3. **Instant 1-command live un-stall proof on the stalled switch (no restart):**
   ```sh
   bcmcmd "setreg CMIC_CMC0_CH1_COS_CTRL_RX_1 0x0400ffff"
   bcmcmd "getreg CMIC_CMC0_CH1_COS_CTRL_RX_1"
   cat /proc/bcm/knet/stats
   cat /proc/bcm/knet/dma
   ```
   **Expected result:** Without touching `bcm-knet` or DCB `0x7f6b9040`, `Rx0 packets` resumes incrementing immediately (draining queued CPU traffic) and `dirty`/`cur`/`free` start advancing.
4. **End-to-end validation of the `hwbcm_core.c` fix across `(a)` and `(b)`:**
   * After warm `syncd` restart with both `(a)` (`warmboot_knet_shutdown_mode=1`, `use_rx_skb=1`) and `(b)` (`warmboot_knet_shutdown_mode=0`, `use_rx_skb=1`), verify `bcmcmd "getreg CMIC_CMC0_CH1_COS_CTRL_RX_1"` reports **`0x0400ffff`**, [hwbcm_core.c:2050-2058](file:///home/kaguyou/homelab/sai-td2p-p4/sairedis/vslib/hwbcm/hwbcm_core.c#L2050-L2058) logs `RX_1=0x0400ffff`, and `/proc/bcm/knet/stats` `Rx0 packets` advances continuously under punted traffic (e.g., ARP/ICMP/LACP).
