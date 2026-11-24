# Task for agy: CPU RX stalls after every syncd warm restart — find the cause in the SDK/KNET code (no switch access)

Tree to read: ~/homelab/openbcm-src/sdk-6.5.27-wb (user-side SDK: src/soc/common/dma.c, cmicm_dma.c, knet.c, src/sal/core/unix/dma*.c,
systems/linux/user/common/*, systems/bde/linux/user/kernel/linux-user-bde.c, systems/bde/linux/kernel/linux-kernel-bde.c (DMA pool, salloc),
kernel module systems/linux/kernel/modules/bcm-knet/bcm-knet.c). Our SAI: ~/homelab/sai-td2p-p4/sairedis/vslib/hwbcm/hwbcm_core.c
(hwbcm_init_ex warm branch, hwbcm_warm_deinit_ex). Write your answer to ~/homelab/sai-td2p-p4/notes/agy_report_knet_rx_stall.md.
DO NOT touch the switch, push, edit any tree other than ~/homelab/sai-td2p-p4 notes/ (and code there only if you are sure; then build as in
notes/agy_task_p4_audit.md and save deploy/build/libsaivs.so.p4d). Another agy run is doing notes/agy_task_p4_audit.md in the same worktree:
do not edit hwbcm_core.c at the same time unless needed; coordinate by committing small focused changes.

## Facts measured on hardware (BCM56864 TD2+, CMICm, KNET loaded with use_rx_skb=1 rx_buffer_size=9238, kernel-bde dmasize=32M)
Syncd warm restart (same kernel, KNET module and netifs/filters kept). Data plane is hitless. CPU RX dies; TX still works.
Both deinit/init variants fail the same way:
 (a) "live": warmboot_knet_shutdown_mode=1 at deinit and init, knet_filter_persist=1
 (b) "dma-reset": kexec-mode deinit (soc_knet_hw_reset(-1) before _bcm_shutdown and before soc_shutdown; afterwards all pktdma idle),
     init with warmboot_knet_shutdown_mode=0 knet_filter_persist=1 (soc_knet_hw_init + channel config run), then bcm_rx_start.
After warm init, case (b):
 - CMIC_CMC0_CH1_COS_CTRL_RX_0/1 = 0xffffffff/0xffff; CH0 COS = 0. CH1_DMA_CTRL = 0x2 (DMA_EN=1, DIRECTION=0 rx). DMA_STAT: CH1_DMA_ACTIVE=1,
   CH1_DESC_DONE=0, CH1_CHAIN_DONE=0, no addr/ECC errors. PCIE_IRQ_MASK0=0x3f20 (CH1/2/3 DESC_DONE+CHAIN_DONE enabled).
 - CMIC_CMC0_DMA_DESC1 = 0x7f6b9040; CH1_DMA_CURR_DESC = 0x7f6b9180 and it does NOT move (5 DCBs of 64 B consumed, then stuck).
 - CPU MMU queues receive packets (show c cpu0: MC_PERQ_PKT(0/1/4) +32/+95/+124).
 - /proc/bcm/knet/stats: Rx0 packets frozen (316986 → 316986), Tx packets and Interrupts increase.
 - /proc/bcm/knet/dma Rx0 section: api 1, dirty 3, cur 0, free 61, run 1, chain head [0x7f6b9040].
 - No kernel KNET errors in dmesg in case (b). In case (a) with use_rx_skb=0 the kernel printed "Fatal error: Incomplete chain" on Rx0
   while syncd was down.
Same switch after a COLD restart of syncd (healthy): DESC1 = 0x7f6b9040 (same), CURR_DESC moves (0x7f6b9d40 …), Rx0 dcb info api 1,
dirty 30, cur 0, free 34, run 1, Rx0 packets increasing. So the ring is at the same bus address in both cases.

## Questions
1. Which memory holds the KNET Rx0 DCB ring at 0x7f6b9040 with use_rx_skb=1 (kernel-bde shared pool via salloc? where exactly in the
   pool?), and which memory does the user-side SDK allocate DCBs/packet buffers from (sal_dma_alloc over linux-user-bde mmap of the same
   pool)? Can a freshly started user process (warm init) allocate/zero/overwrite the region KNET's ring lives in (pool re-init, memset,
   soc_dma_init/soc_dma_attach, bcm_rx_start pool alloc, KCOM_M_DMA_INFO)? Show the code path with file:line.
2. Why would the hardware consume 5 descriptors and stop while the kernel's cur stays 0 (kernel never saw DESC_DONE)? Consider: DCB
   control bits overwritten (e.g. chain/reload/scatter bits cleared), the kernel ring rebuilt but HW still pointing at an older chain, interrupt
   enable lost, KCOM DMA_INFO from the new process re-pointing the channel at a user chain.
3. Propose the minimal robust fix (in hwbcm warm init/deinit, or SDK warm path guarded by SOC_WARM_BOOT, or KNET params) so that CPU RX
   resumes after a syncd warm restart in both (a) and (b), plus a hardware experiment I (the lead) can run to confirm the cause in < 5
   minutes (exact diag-shell / /proc commands; the lead can run bcm diag commands like getreg/setreg/dump via a socket, and read /proc/bcm/knet/*).
