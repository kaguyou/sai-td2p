# Warm Boot Phase 4 Plan: SONiC `warm-reboot` (kexec) on AS5812-54T

**Date:** 2026-11-24
**Branch:** `warmboot` (`~/homelab/sai-td2p-wb`). The working tree has an uncommitted `hwbcm_core.c` change (soc_mmu_init skip) from the syncd warm restart debugging. Phase 4 builds on top of it.
**Inputs:** `notes/design_warmboot.md` §3.6 / Phase 4, `agy_report_warmboot_phase{1,3}.md`, switch `/usr/local/bin/warm-reboot` (read only, 978 lines, same file as `fast-reboot`, md5 identical), `deploy/remote_install.sh`, switch `/home/admin/sai-td2p/redeploy.sh`, SDK `sdk-6.5.27-wb`.
**Status:** plan only. No code was changed and nothing was run on the switch beyond read-only commands.

---

## 0. TL;DR

The phase 1-3 design assumes "Case B (kexec) = KNET table empty, so recreate netifs from `m_objectHash`". That part exists (`SwitchBCM56860HW::hwWarmRecoverKnet`). Two blockers come before it, and both are about KNET DMA, not netifs:

1. **Stale KNET DMA across kexec (memory corruption).** Our warm deinit forces `warmboot_knet_shutdown_mode=1`. That keeps the KNET RX/TX DMA rings running, which is correct for a syncd restart. SONiC's `warm-reboot` "stops" `opennsl-modules`, but that unit's `ExecStop` is commented out on purpose, so **no module is unloaded and nothing aborts the KNET channels**. The ASIC's CMIC RX channels still point at DCBs in the old kernel's memory (`/proc/bcm/knet/dma` shows them at 0x7b16xxxx). Linux clears Bus Master during the kexec shutdown. The new kernel's `linux-kernel-bde` probe sets `PCI_COMMAND_MASTER` again (`_pci_probe`), about 9 s into boot. From then on, CPU-bound packets (LLDP/ARP/BGP) can be DMA-written into whatever the new kernel has put at those physical addresses. Broadcom's SAI avoids this because its warm shutdown runs `soc_knet_hw_reset`.
2. **Fresh KNET is never HW-initialised on warm init.** `hwbcm_init_ex(warm)` sets `warmboot_knet_shutdown_mode=1` and `knet_filter_persist=1` before `soc_init`. As a result:
   - `soc_dma_init` skips `soc_knet_hw_init` (no `KCOM_M_HW_INIT`: no kernel DCB rings, no `bkn_isr`).
   - `cmicm_dma_chan_config` skips the KNET channels.
   - `_bcm_modules_init` skips `bcm_esw_knet_init` (no `DefaultRxAPI` filter).
   - `bcm_rx_start` then sends `KCOM_M_DMA_INFO` to an sinfo with `dcb_wsize=0`.

   After kexec this leaves CPU RX/TX dead at best, and may oops the kernel at worst. Case B netif recreation would "succeed", but on a dead KNET.

The other items are minor or already solved:
- `/dev/linux-user-bde` is already created at every boot by `td2p-prep.service`, and inside the container by `syncd_init_common.sh` (major 126 matches).
- kexec, the kernel and the initrd are all present.
- No platform init script resets the MAC or PCIe.

---

## 1. `warm-reboot` script audit (switch copy, SONiC 202505)

`warm-reboot` == `fast-reboot` (same file; behaviour keyed on `$0`). `REBOOT_METHOD="/sbin/kexec -e"`. Steps in order, with what can fail or hurt on our platform:

| # | Step (function) | What it does | Risk for sai-td2p / AS5812 |
|---|---|---|---|
| 1 | `check_conflict_boot_in_fw_update` | /tmp/firmwareupdate task files | none |
| 2 | `check_warm_restart_in_progress` | **aborts if any `WARM_RESTART_ENABLE_TABLE|*` enable=true** (unless `-f`) | **Will abort now**: `WARM_RESTART_ENABLE_TABLE|syncd enable=true` is set by the syncd warm restart work. Run `config warm_restart disable syncd` first (or use `-f`). |
| 3 | `config warm_restart enable system`, trap `clear_boot` | sets system flag; on failure `kexec -u`, `config warm_restart disable`, renames dump.rdb | fine |
| 4 | `save_counters_folder`, `unload_kernel` | `/host/counters`; `kexec -u` if `/sys/kernel/kexec_loaded`=1 (now 0) | fine |
| 5 | `setup_reboot_variables` | HWSKU via `show platform summary --json` + `python` (python -> python3, works: hwsku `Accton-AS5812-54T`); Next==Current image (`202505.1238526-ff3c0c9f6`); onie_platform -> kernel/cmdline from `/host/grub/grub.cfg` | OK. KERNEL=`/host/image-202505.1238526-ff3c0c9f6/boot/vmlinuz-6.1.0-29-2-amd64` (8.2 MB), INITRD=`.../initrd.img-6.1.0-29-2-amd64` (34 MB), both present. `installer.conf` has only console vars (no `KEXEC_LOAD_EXTRA_CMDLINE_LINUX`). |
| 6 | `reboot_pre_check` | docker exec probe of radv bgp lldp swss database teamd syncd (1 s timeout each); /host rw; `check_db_integrity.py`; >=100 MB free on /host (2.4 GB free); `sonic-installer verify-next-image`; `asic_config_check` (same image -> pass) | all containers must be up and answer within 1 s. |
| 7 | `ctrmgr_tools.py tag-all` | kube image tags | n/a |
| 8 | kernel load | `mokutil --sb-state` fails ("EFI variables are not supported", BIOS boot) -> `kexec -l KERNEL --initrd=INITRD --append="BOOT_IMAGE=... SONIC_BOOT_TYPE=warm" -a` | kexec-tools 2.0.25 at /sbin/kexec (not in admin's PATH, the script uses the full path). Kernel has CONFIG_KEXEC/KEXEC_FILE=y, SIG not forced. |
| 9 | `init_warm_reboot_states` | clears `WARM_RESTART_TABLE|*` state | fine |
| 10 | `lag_keepalive.py` (30 s), sleep 5, `setup_control_plane_assistant` (no `-c` -> skipped), `teamd_increase_retry_count.py --probe-only` | LACP refresh | We have no LAG to the PC. Harmless. |
| 11 | `orchagent_restart_check -w 2000 -r 5` | freezes orchagent; **exit 10 if it cannot freeze** | orchagent must have no pending tasks. Any SAI op failing in a loop (e.g. a retried unsupported attribute) blocks the freeze. Check `orchagent_restart_check` by hand first (it only pauses, it does not stop anything). |
| 12 | `set +e`, committed from here. Stops `sonic-delayed.target` timers. | | no way back after this |
| 13 | services in `/etc/sonic/warm-reboot_order`: `lldp mux nat radv sflow bgp swss dash-ha teamd syncd` | after **swss** stops: `initialize_pre_shutdown` + `docker exec syncd syncd_request_shutdown --pre` (5 s timeout) + wait up to 60 s for `WARM_RESTART_TABLE|warm-shutdown state=pre-shutdown-succeeded` | syncd sets `RESTART_WARM=true` + `PRE_SHUTDOWN=true` -> our `hwWarmSync()` (scache + .hwstate). A failure is only logged, the reboot continues. |
| 14 | `systemctl stop syncd` | host `syncd_common.sh stop`: TYPE=warm (system flag) -> `stopplatform1`: no `plugins/syncd_request_pre_shutdown` on this platform; `syncd_request_shutdown --warm`; **waits max 20 s for syncd to exit**, then `docker exec syncd /bin/sync`, then container stop (SIGKILL after docker timeout) | our destructor runs `hwbcm_warm_deinit`. If the deinit plus vslib `sai-warmboot.bin` write takes more than 20 s plus the docker timeout, syncd is killed mid-deinit: partial files and un-aborted DMA. |
| 15 | `backup_database` | deletes most STATE_DB keys; dumps redis to `/host/warmboot/dump.rdb` | fine |
| 16 | `systemctl stop docker` | | fine |
| 17 | `systemctl stop opennsl-modules` (broadcom) | **no-op**: ExecStop is commented out ("removing knet drivers takes ~30 seconds") | **KNET + BDE stay loaded and KNET DMA keeps running into kexec**, see §0.1. |
| 18 | reboot-cause file, `sync` | | fine |
| 19 | `platform_fw_au_reboot_handle` | not present on the platform | none |
| 20 | `watchdogutil arm` | | Platform has only legacy `plugins/` (eeprom/psuutil/sfputil), so it is expected to fail harmlessly. **Unverified (switch was unreachable):** run `sudo watchdogutil status` once. |
| 21 | `${DEVPATH}/${PLATFORM}/warm-reboot_plugin` | **not present**, so **this is our hook** (runs after docker/syncd are gone, right before kexec; exit code ignored) | see H2 |
| 22 | logs to `/host/logs_before_reboot` (only if /var/log is tmpfs), `exec /sbin/kexec -e` | | |

New-boot side (same image):
- `/proc/cmdline` has `SONIC_BOOT_TYPE=warm`. Host `syncd_common.sh` sees the system warm flag (restored from dump.rdb), so it reuses the existing `syncd` container. The container lives in `/host/image-*/docker`, so **our `libsaivs.so.0.0.0` and the `libsai.so.1` symlink survive**. Warm-rebooting into a *different* image would bring back the vendor libsai: same image only.
- In-container `syncd_init_common.sh` has `check_warm_boot` hard-wired to false and no `SONIC_BOOT_TYPE=warm` case. syncd picks WARM from `WarmStart::isWarmStart()` (STATE_DB), the same way it does for the syncd warm restart that works today.
- `/var/warmboot` = bind mount of `/host/warmboot` (ext4 on sda4), so it persists across kexec. It currently holds `sai-warmboot.bin` (1.1 MB), `.scache` (8.4 MB), `.hwstate` (15 KB) and `warm-starting`.

## 2. Platform / boot-time ASIC & PCIe touch points (new kernel)

| Component | Touches ASIC/PCIe? | Verdict |
|---|---|---|
| kexec itself | no BIOS, so no PERST#/PCIe link reset and no CPLD reset. Linux `pci_device_shutdown` clears Bus Master on all devices (kexec_in_progress) | ASIC keeps forwarding. Bus Master is cleared at kexec (good), then re-set by the BDE probe (bad if KNET channels are still armed). |
| new-kernel PCI enumeration | disables MSI/MSI-X on the device; the device uses INTx IRQ 16 (IO-APIC fasteoi, `DisINTx-`) | OK; the IRQ is only requested on `interrupt_connect`. |
| `linux-kernel-bde` probe (`_pci_probe`) | sets `PCI_COMMAND_MEMORY|MASTER`; `pci_bridge_setup=1` adjusts the bridge; `maxpayload` sets MPS; **no CMIC reset, no CPLD write** (the PLX CPLD reset write is `#if 000`) | OK, provided the KNET DMA is idle (§0.1). |
| `td2p-prep.service` (`DefaultDependencies=no`, after systemd-modules-load) | modprobes `linux-kernel-bde`, `linux-user-bde`, `linux-bcm-knet`, `linux-knet-cb` **without params** and creates `/dev/linux-user-bde`; `i2cset -y 1 0x60 0x0d 0x04` (fan) | It wins the race with `opennsl-modules` (9.0 s vs 13.3 s): the live modules run defaults (`maxpayload=256`, `usemsi=-1`, `dmasize=(null)`, so a 16 MB kernel-chunk instead of 32M; KNET without `use_rx_skb=1 rx_buffer_size=9238 default_mtu=9100`). Not a phase-4 regression (cold works like this), but make it deterministic (H3). R8 of the design (the device node) is **already solved** by this unit. |
| `opennsl-modules.service` | modprobe with params (no-op, already loaded), mknod of `/dev/linux-bcm-bde`, `/dev/linux-kernel-bde` etc. | OK |
| `as5812-platform-init` (`accton_as5812_util.py install`) | instantiates i2c muxes, CPLDs, optoe. The patched CPLD driver probe writes **CPLD 0x23 = 0x3F (QSFP modules out of reset)** | It only touches the QSFP 49-54 reset lines and does nothing if they are already released. Check that 0x23 reads 0x3F on the running system before the test (if any QSFP is held in reset now, the warm boot would release it, which is a link change, not an ASIC reset). The MAC is never reset. |
| `td2p-fan`, `td2p-clock` | CPLD fan PWM / fake RTC | no ASIC effect |
| pmon (delayed on warm), xcvrd | SFP/QSFP EEPROM reads | xcvrd does not reset modules on warm boot (no CMIS here) |
| `led_proc_init.soc` | used by the Broadcom SAI only | the LED processor keeps running from the cold boot |

## 3. libsai after kexec: what breaks (hwbcm_core.c / SwitchBCM56860HW_wb.inc.cpp)

State after kexec: the ASIC tables, ports, MMU, CMIC COS-to-channel map and EPC_LINK are all intact. Kernel modules are fresh: the KNET sinfo has no DCBs, no ISR, `rx_chans=0`, zero netifs/filters. The kernel-BDE DMA pool is newly allocated at a new physical address. The user-BDE is fresh. Our process is fresh in both cases A and B.

| Area | Today | After kexec |
|---|---|---|
| Warm init `hwbcm_init_ex(warm)` L1603: forces `warmboot_knet_shutdown_mode=1`, `knet_filter_persist=1` before `soc_init` | correct for Case A | **BREAKS:** no `KCOM_M_HW_INIT` (dma.c:3443-3474), KNET channels not configured (cmicm_dma.c:231), `bcm_esw_knet_init` skipped (bcm/esw/init.c:843), so there is no `DefaultRxAPI` filter. `bcm_rx_start` -> `KCOM_M_DMA_INFO` reaches an uninitialised sinfo (`dcb_wsize=0`, `cmic_type=0`): undefined, possible TX DMA from garbage or an oops. |
| Warm deinit `hwbcm_warm_deinit` L1966: forces mode=1, so `soc_dma_abort_channel` -> `soc_knet_wb_cleanup` only | correct for syncd restart | **BREAKS (dangerous):** KNET RX/TX channels stay armed into the kexec (§0.1). No code path distinguishes "syncd restart" from "system warm-reboot". |
| Userspace DMA engines (counter DMA, L2 mod FIFO, SBUS DMA, packet DMA chans, mem scan) and CMIC interrupts | stopped in `soc_shutdown` (drv.c: `soc_dma_detach`, `soc_counter_detach`, `soc_l2x_stop`, `soc_mem_scan_stop`, `soc_sbusdma_desc_detach`, `soc_fifodma_deinit`, `soc_cmicm_intr{0..6}_disable(~0)`) | OK **if `soc_shutdown` runs to the end**. Several steps are `SOC_IF_ERROR_RETURN`: an early error skips the later stops. We only log `HWBCM_WARN_ON_FAIL`. In kexec mode a failure there must be escalated (H2 fallback to a cold reboot). |
| DMA buffers: SDK `soc_cm_salloc` from the new kernel-BDE pool | re-allocated every process start, and warm init reprograms the host addresses as the engines start | OK. No DMA address is stored in scache/.hwstate. |
| Interrupts | user-BDE `LUBDE_ENABLE_INTERRUPTS` at attach; KNET registers `bkn_isr` in HW_INIT | OK once HW_INIT runs. `bkn_knet_hw_init` masks all IRQs first. |
| `/dev/linux-user-bde` | host: td2p-prep; container: `syncd_init_common.sh` mknod c 126 0; `/proc/devices` has 126 | OK |
| KNET netifs: `hwWarmRecoverKnet` Case B (`nlive==0`), which recreates each `SAI_HOSTIF_TYPE_NETDEV` on a port via `hwbcm_knet_netif_create` (same MAC = switch SRC_MAC, PVID read from HW, RIOT vfi filter, any-port filter) | implemented | Works only after the HW_INIT fix. Gaps: returns SUCCESS even if 0 of N were recreated; netif IDs change (fine, `.hwstate` has no KNET IDs); MTU is the module default (portmgrd re-applies 9100); carrier is on by default (we never drive carrier, same as cold). |
| `knet_filter_persist=1` | no `DefaultRxAPI` in Case A (it survives in the kernel from cold) | Case B must create it (cold has it). Non-matching packets would otherwise be dropped in the kernel instead of reaching `bcm_rx` (we do not consume `bcm_rx` today, so this is low impact; keep parity anyway). |
| Pre-shutdown vs final shutdown | `PRE_SHUTDOWN` -> `hwWarmSync` sets `m_warmSynced=true`, and the destructor then skips the re-sync | Between `--pre` and `--warm`, teamd stops and linkscan/L2 learning continue. Re-sync in the destructor (cheap) so scache/.hwstate match the final state. |
| Linux-side state (IPs, VLAN bridge, teams, neighbors, routes) | kernel kept them in Case A | Rebuilt by swss warm start (portmgrd/intfmgrd/vlanmgrd/teammgrd, `restore_neighbors.py`), which needs working KNET TX. Standard SONiC behaviour. |
| Control-plane gap | none in Case A | about 60-120 s with no CPU RX/TX. BGP/EVPN to the PC FRR needs **graceful restart on both ends**, otherwise the PC withdraws routes (that is PC-side loss, not ASIC loss). Software linkscan is gone during the gap: a link that drops meanwhile is not reflected in EPC_LINK/trunk until syncd is back. |

## 4. Code changes

### C1: Detect a fresh KNET/kernel before `soc_init` (hwbcm_core.c)
- New `static int knet_kernel_fresh(void)`: read `/proc/bcm/knet/link` (global procfs, visible in the privileged container; today it lists 72 `EthernetN up` lines under "Software link status:"). Fresh = file present and 0 netif lines. If the file cannot be read, also treat it as fresh: misdetecting fresh in Case A only costs a CPU-path blip plus netif recreation, while misdetecting Case A in Case B kills the control plane.
- Plus a boot-id cross-check: at `hwbcm_warm_sync` write `<scache_file>.bootid` = `/proc/sys/kernel/random/boot_id` (the container sees the host value: verified identical). At warm init, a different boot_id means kexec happened, so force fresh even if `/proc/bcm/knet/link` somehow lists netifs. Log both signals: `wb: knet fresh=%d (bootid %s, netifs %d)`.
- Export `int hwbcm_warm_knet_fresh(void)` so `hwWarmRecoverKnet` uses the same verdict instead of `nlive==0`.

### C2: Cold-style KNET bring-up inside the warm init when fresh (hwbcm_init_ex)
In the `#ifdef BCM_WARM_BOOT_SUPPORT if (warm_boot)` block, replace the two `sal_config_set(...,"1")` calls with:
```c
int fresh = knet_kernel_fresh();
sal_config_set(spn_WARMBOOT_KNET_SHUTDOWN_MODE, fresh ? "0" : "1");
sal_config_set(spn_KNET_FILTER_PERSIST,        fresh ? "0" : "1");
```
With mode=0 under `SOC_WARM_BOOT`:
- `soc_dma_init` calls `soc_knet_hw_init` (`KCOM_M_HW_INIT`: DCB alloc, abort+clean, irq mask, `bkn_isr`, `bkn_dma_init`).
- `cmicm_dma_chan_config` re-inits the KNET channels (pci writes are allowed in warm boot).
- `bcm_esw_knet_init` runs: cleanup on an empty table, `_bcm_common_rx_queue_channel_set(-1,1)` rewrites the same CMIC COS map values, and `DefaultRxAPI` is created.

None of this touches the forwarding tables (schan/table-DMA writes stay disabled by `SOC_WARM_BOOT`). After `bcm_init`, set both properties back to "1" so a later syncd-restart deinit keeps the rings. Make sure `bcm_rx_start` still runs (`!bcm_rx_active`).
- Verify `knet_stale_cleanup()` is still skipped (the table is empty anyway).

### C3: "System warm reboot" deinit mode = abort KNET DMA (hwbcm_warm_deinit)
- Add `hwbcm_warm_deinit_ex(int for_kexec)` (keep `hwbcm_warm_deinit()` = `_ex(0)`).
- `for_kexec=1`: keep mode=1 (so netifs/filters are not destroyed one by one, which takes about 30 s), but **right before `soc_shutdown(unit)`** call `soc_knet_hw_reset(unit, -1)`. channels=0 means `bkn_dma_abort` on all TX/RX channels plus `bkn_clean_dcbs`. Log the rv and fail loudly.
- After `soc_shutdown`, read back CMC0..2 `CHy_DMA_CTRL` (0x31140+0x1000*cmc+4*ch, bit1 PKTDMA_ENABLE), `DMA_STAT` (0x31150, bits 8..11 ACTIVE), `FIFO_CHy_RD_DMA_CFG` (0x312c0, bit0) and `STAT_DMA_CFG` (0x311c4), using `soc_pci_read` before the detach or the BDE mmap. Write `<base>.dma_idle` = `ok`/`busy` for the host plugin (H2).
- Also escalate: if `_bcm_shutdown`/`soc_shutdown` fails in kexec mode, write `<base>.dma_idle=busy`.

### C4: Tell hwbcm which mode applies (SwitchBCM56860HW.cpp)
- `SAI_SWITCH_ATTR_PRE_SHUTDOWN=true` is only sent by `warm-reboot`/`fast-reboot` (a syncd-only warm restart uses `--warm` without `--pre`). Set `m_preShutdown=true` there.
- Destructor: `doWarm`, then `hwbcm_warm_deinit_ex(m_preShutdown)`.
- Always re-run `hwWarmSync()` in the destructor for warm deinit (drop the `!m_warmSynced` shortcut, or keep it only when no notification arrived after pre-shutdown) so the final scache/.hwstate are current.
- Optional hard override for tests: env `HWBCM_WB_KEXEC=1` or the file `/var/warmboot/kexec-pending` forces kexec mode.

### C5: Case B hardening (SwitchBCM56860HW_wb.inc.cpp `hwWarmRecoverKnet`)
- Branch on `hwbcm_warm_knet_fresh()` instead of `nlive > 0`. If fresh and nlive > 0 (impossible unless the detection is wrong), log an error and run `knet_stale_cleanup`-style teardown, then recreate.
- Return FAILURE (and let `warm_boot_initialize_objects` fall back) if fewer than all port hostifs were recreated? No: a fallback to cold init means a full data-plane reset. Keep SUCCESS but log `ERROR` with recreated/expected counts, and expose a counter in the log line that the test greps.
- Log the per-netif mapping at NOTICE for the first 4 and the total.

### C6: Timing guard
- Measure `hwWarmSync` + `hwbcm_warm_deinit_ex` + vslib `apiUninitialize` in the log (`wb: shutdown total %ld ms`). It must stay well under the 20 s `stopplatform1` budget plus the docker stop timeout. Phase 1-3 numbers suggest seconds, but the scache is 8.4 MB and is written with fsync.

### C7: Small cleanups found on the way
- config.bcm has `stable_size` twice (0x2000000 then 0x5500000). Remove the first in `remote_install.sh`'s managed block, or leave a comment saying which one wins.
- `warm_boot_initialize_objects` falls back to a *cold SDK init* on warm attach failure. After kexec that is a full ASIC reset with traffic loss. Acceptable, but log it at ERROR with the "kexec" context.

### Host-side changes (deploy, all idempotent, via remote_install.sh)
- **H1** `deploy/remote_install.sh`: install the H2 plugin and the H3 unit fix. Leave `opennsl-modules.service` alone (keeping the modules loaded matches the stock flow).
- **H2** `/usr/share/sonic/device/x86_64-accton_as5812_54t-r0/warm-reboot_plugin` (also as `fast-reboot_plugin`). It runs after docker stops and right before `kexec -e`:
  1. Read `/host/warmboot/sai-warmboot.bin.dma_idle`.
  2. Independently read the CMIC DMA registers via `/sys/bus/pci/devices/0000:01:00.0/resource0` (BAR0 0xdfe00000, read-only mmap, small python3 script; same offsets as C3).
  3. If any KNET/packet channel is still active: `rmmod linux-knet-cb linux-bcm-knet` (`_cleanup` -> `bkn_dma_abort` + irq mask; costs about 30 s of extra control-plane downtime, no data-plane effect), then re-check.
  4. If anything (FIFO/stat/sbus DMA) is still active: log at `user.err`, `kexec -u`, `exec /sbin/reboot`. A cold reboot loses traffic but cannot corrupt memory.
  5. Log all of it to `/host/warmboot/plugin.log` (survives the reboot).
- **H3** `td2p-prep.service`: add `After=opennsl-modules.service` and drop the bare `modprobe`s (or copy the opennsl params). Module params then match the intended SONiC defaults on every boot, cold or warm. **Do this in a cold-boot window, not mid-phase-4** (it changes the KNET MTU default and the BDE DMA pool size, so it needs its own cold regression run).
- **H4** README/notes: warm-reboot is same-image only, because a new image means a new docker dir and the vendor libsai.

## 5. Test procedure

Prerequisites:
- syncd warm restart (Case A) passes at least 3 times in a row with 0 loss.
- Phase 4 code built with `deploy/build_wb.sh` and installed with `install.sh --stop` (or `redeploy.sh`).
- Serial console open (`tools/bdeshim/serial_send.py` / console on ttyS1 115200). After kexec, ssh is gone for a while and a kernel oops is only visible there.

Traffic harness (independent of the switch control plane):
- PC NIC A (onboard) to Ethernet0, PC NIC B (USB NIC, in netns `wbB`) to Ethernet1.
- Same VLAN (L2 path) plus, in a second run, different VLANs with SVIs and static routes (L3 path).
- Run `ping -i 0.002 -W 1` A to B with a timestamp log, and `iperf3 -u -b 100M` with loss counting.
- Optional: a VXLAN flow (two cables, per the 2026-11-21 notes).
- Record `bcmcmd`-free counters: PC-side `ip -s link` before/after.

### Stage 0: Read-only preflight (no change)
1. `cat /sys/kernel/kexec_loaded` must be 0; `sonic-installer list` must show Next==Current.
2. `ls /host/image-*/boot/{vmlinuz,initrd.img}-6.1.0-29-2-amd64`.
3. `sudo watchdogutil status`; `docker exec swss orchagent_restart_check -w 2000 -r 1` (dry pause check; it unpauses).
4. Read CPLD 0x23 via sysfs or `i2cget` (expect 0x3F).
5. FRR GR: `vtysh -c 'show bgp neighbors' | grep -i graceful` on both the switch and the PC.
6. Keep `sudo cat /proc/bcm/knet/dma` and `/proc/bcm/knet/link` as the baseline.

### Stage 1: Unit: kexec-mode deinit leaves DMA idle (no reboot)
1. `config warm_restart enable syncd`, plus `touch /host/warmboot/kexec-pending` (C4 override).
2. `systemctl restart syncd` would immediately restart syncd, so use `systemctl stop swss syncd` instead. Stop is warm because the flag is set.
3. Check:
   - syncd log shows `soc_knet_hw_reset(-1) rv=0` and `dma_idle=ok`.
   - `/proc/bcm/knet/dma` shows no active RX/TX chains.
   - the H2 checker run by hand reports all channels idle.
   - traffic harness: **0 loss** (data plane does not depend on KNET).
4. `rm kexec-pending`; `systemctl start swss`. This is now a "mode=1-less" Case A: KNET rings were aborted but netifs still exist. C1 says not fresh (netifs > 0, same boot_id), so no HW_INIT happens and the KNET RX would stay dead. To cover this, C1 also treats a "rings aborted" marker (`<base>.dma_idle` present and same boot) as fresh-DMA: call HW_INIT, but keep the netifs and run Case A. Implement this as a third state, `KNET_DMA_RESET`. Expect the control plane to come back, `/proc/bcm/knet/dma` to be active again and `ping` to the switch SVI to work.

### Stage 2: Simulated kexec: reload the kernel modules, no reboot (exercises Case B)
1. Warm stop as in Stage 1 (kexec-pending set).
2. `sudo rmmod linux-knet-cb linux-bcm-knet linux-user-bde linux-kernel-bde` (about 30 s for 72 netdevs; `linux_ngbde`/psample stay). Then `sudo systemctl restart td2p-prep` or `opennsl-modules start`, and recreate `/dev/linux-user-bde` (td2p-prep does).
   This gives a fresh KNET (0 netifs), a fresh DMA pool at a new address and fresh BDE IRQ registration, exactly like after kexec, but with an easy rollback.
3. `rm kexec-pending`; `systemctl start swss`.
4. Expect:
   - log `wb: knet fresh=1`, `KCOM HW_INIT` (dmesg KNET debug 0x5020 shows instance/DMA messages), `Case B recreated 72 KNET netdev(s)`.
   - `/proc/bcm/knet/link` has 72 entries.
   - `DefaultRxAPI` filter present (`/proc/bcm/knet/...` or `bcmcmd knet filter show` via our diag socket).
   - orchagent RECONCILED, `show warm_restart state`.
   - ARP/ND/LLDP/BGP back.
   - **traffic loss 0** throughout.
   - `dmesg` shows no `nobody cared`, no oops, no `DMA` errors.
5. Repeat 3 times. Also run once with VXLAN/EVPN, ACL, CoPP, QoS and mirror configured.

### Stage 3: Plugin dry run
1. Install H2. Run it by hand in a "check only" mode (`warm-reboot_plugin --check`, no rmmod/reboot) both with syncd running (expect `busy`, KNET active) and after a kexec-mode stop (expect `ok`).

### Stage 4: Real `warm-reboot`, L2/L3 only
1. `config warm_restart disable syncd` (otherwise step 2 of the script aborts).
2. Optional: set the swss/bgp timers (`config warm_restart neighsyncd_timer`, `bgp_timer`).
3. Start the traffic harness and the serial console capture.
4. `sudo warm-reboot -v` (no `-f`; let every precheck run).
5. Expect:
   - pre-shutdown succeeded (syslog).
   - syncd shutdown under 20 s.
   - plugin log `dma_idle=ok`, kexec.
   - New kernel: no oops; `/proc/linux-kernel-bde` interrupts counting.
   - syncd log `fresh=1 (bootid changed)`, `Case B recreated 72`, `true warm boot completed`.
   - `show warm_restart state` shows all reconciled.
   - **0 data-plane loss** on the L2 and L3 flows during the whole window, and no port flap on the PC NICs (`ip monitor link`, and the PC-side 1G link watchdog must not fire).
   - `show reboot-cause` = warm-reboot.
6. Post checks:
   - `ASIC_DB` object count equal to before.
   - a counters sanity check.
   - `/host/warmboot/*` timestamps.
   - a second `warm-reboot` right after (the warm-after-warm chain).
   - then a cold `reboot` to prove the cold path still works.

### Stage 5: Full feature warm-reboot
Same as Stage 4 with VXLAN/EVPN (PC FRR, GR enabled), RIOT SVI, ACL, CoPP, QoS and mirror active.

Expect:
- VXLAN flow 0 loss.
- EVPN routes kept by GR on the PC.
- CoPP policers and counters continue.

Failure paths to run once each:
- Delete `.hwstate` before the warm-reboot: expect a clean cold SDK init after kexec (traffic loss accepted, no crash).
- Kill syncd (`docker kill syncd`) after pre-shutdown: the plugin must see `busy` and take the `rmmod`/`reboot` path.

Rollback at any stage: cold `sudo reboot`, which brings back the cold init with our lib; `deploy/rollback.sh` returns to the vendor SAI.

## 6. Top risks

| # | Risk | Severity | Mitigation |
|---|---|---|---|
| P1 | KNET RX/TX DMA still armed at kexec, so the ASIC writes CPU packets into new-kernel memory once the BDE probe re-enables Bus Master (random corruption, maybe hours later) | **Critical** | C3 (`soc_knet_hw_reset(-1)` in kexec-mode deinit) + C4 (pre-shutdown -> kexec mode) + H2 (independent register check, `rmmod knet` and cold-reboot fallback). Stage 1/3 verify. |
| P2 | Fresh KNET never gets `KCOM_M_HW_INIT` because warm init forces mode=1, leaving the control plane dead or the kernel oopsing on `DMA_INFO` | **High** | C1 + C2; Stage 2 exercises it without a reboot. |
| P3 | syncd killed mid-shutdown (20 s budget + docker timeout) leaves partial scache/.hwstate and un-aborted DMA | High | C6 timing; C3 aborts DMA *before* the long steps; H2 catches it. |
| P4 | Detection error between Case A and Case B (bad `/proc` read, or rings aborted but netifs kept, Stage 1 step 4) | Medium | Tri-state C1 (`not fresh` / `dma reset` / `fresh`) using boot_id + netif count + `.dma_idle` marker; default to the "fresh" side. |
| P5 | Some userspace DMA engine not stopped because `soc_shutdown` returned early on error (`SOC_IF_ERROR_RETURN`) | Medium | C3 read-back + H2 register check -> cold reboot instead of kexec. |
| P6 | Control-plane gap (about 1-2 min) drops BGP/EVPN on the peer without GR; links that change during the gap are not seen by software linkscan | Medium (test setup) | Enable GR on the PC FRR and the switch; keep links stable during the test. |
| P7 | Module param race (td2p-prep vs opennsl-modules) gives different `maxpayload`/DMA pool/KNET params between boots | Low | Same order every boot today; fix with H3 in a cold-boot window. |
| P8 | Stale INTx pending at BDE `interrupt_connect` (`irq 16: nobody cared`, IRQ disabled) | Low | `soc_shutdown` disables all CMIC IRQ sources and KNET HW_INIT masks before connecting; check dmesg in Stage 2/4. |
| P9 | warm-reboot into a different image loads the vendor libsai with our warm files | High if done, avoidable | Same image only (`sonic-installer list` Next==Current check in Stage 0); document it. |
| P10 | `check_warm_restart_in_progress` aborts because `WARM_RESTART_ENABLE_TABLE|syncd` is still enabled from the syncd-restart work | Low (procedural) | Stage 4 step 1. |

## 7. Not verified (switch became unreachable mid-survey)
- `watchdogutil status` (whether `arm` does anything on this legacy-plugin platform).
- Whether `sonic_platform` imports.
- GR settings in the switch FRR config.
- That `syncd_request_shutdown` exists in the container (it must, since the syncd warm restart uses it).

All of these are covered by Stage 0.
