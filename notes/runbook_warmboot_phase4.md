# Runbook: warm boot phase 4 (SONiC warm-reboot via kexec), stages 1-5

Branch `warmboot-phase4` (`~/homelab/sai-td2p-p4`), library `deploy/build/libsaivs.so.p4`
(sha256 in the commit message / `deploy/build/libsaivs.so.0.0.0.sha256`). Plan and rationale:
`notes/plan_warmboot_phase4.md`. Stage 0 (read-only preflight) is in the plan, run it first.

Conventions: `PC$` = on the PC in `~/homelab/sai-td2p-p4`, `SW$` = on the switch as admin.
`log` below = `sudo grep -E 'hwbcm|wb[: ]|syncd#' /var/log/syslog | tail -n 200` (hwbcm logs to syslog).
Keep the serial console (ttyS1 115200) open from stage 2 on; an oops after kexec is only visible there.

## What changed (what the logs will show)

| moment | log line | meaning |
|---|---|---|
| warm-reboot pre-shutdown | `wb: PRE_SHUTDOWN: system warm reboot, final deinit will run in kexec mode` | kexec mode armed |
| final shutdown | `wb: destructor warm sync (final, after PRE_SHUTDOWN sync)` | scache/.hwstate re-synced |
| | `hwbcm_warm_deinit: mode=KEXEC (abort KNET DMA)` | |
| | `hwbcm_warm_deinit[kexec]: soc_knet_hw_reset(-1) rv=0 (KNET rings aborted)` | |
| | `packet DMA re-armed after abort ... aborting again` | can happen now that KNET runs `use_rx_skb=1` (td2p-knet.conf): bkn_rxtick re-arms an aborted RX ring; must end idle |
| | `hwbcm_warm_deinit[kexec]: dma_idle=ok (pktdma_en=0x... statdma_en=0x...)` or `dma_idle=busy (...)` | verdict, also in `/host/warmboot/sai-warmboot.bin.dma_idle` |
| | `wb: shutdown total N ms (sync, deinit, mode kexec)` and vslib `end (apiUninitialize took N ms)` | must stay well below the 20 s syncd stop budget |
| warm init | `wb: knet fresh=F state=S (bootid ... saved ..., netifs N, dma marker ...)` | S = live / dma-reset / fresh / unknown |
| | `hwbcm_init_ex[warm]: knet S -> warmboot_knet_shutdown_mode=M knet_filter_persist=P` | live: 1/1, dma-reset+unknown: 0/1, fresh: 0/0 |
| | `KNET kernel side re-initialised (S, rx active)` | not printed for live |
| | `wb knet: verdict V ..., N live netif(s)` then `Case A ... matched` or `Case B recreated 72/72` | ERROR lines here = detection mismatch or failed netdevs |
| | `wb: true warm boot completed` | |
| plugin | `/host/warmboot/plugin.log` | verdict, registers, actions |

Test overrides for kexec mode without warm-reboot: `sudo touch /host/warmboot/kexec-pending`
(container path `/var/warmboot/kexec-pending`) or env `HWBCM_WB_KEXEC=1`. **Remove the file after
each test**, otherwise every syncd warm restart aborts the KNET rings (it then recovers through
`dma-reset`, but with a CPU-path blip).

## Install (once per build)

```
PC$ git -C ~/homelab/sai-td2p-p4 log --oneline -1
PC$ deploy/install.sh --stop --lib deploy/build/libsaivs.so.p4
     # also installs deploy/platform/warm-reboot_plugin as warm-reboot_plugin + fast-reboot_plugin
SW$ ls -la /usr/share/sonic/device/x86_64-accton_as5812_54t-r0/*reboot_plugin
SW$ sudo systemctl reset-failed swss syncd; sudo systemctl start swss
```
Cold start first: everything must come up as with the previous library (72 ports, traffic harness 0 loss
across a syncd warm restart = Case A regression check, see "Case A regression" below).

## Case A regression (do this first, and after every change)

```
SW$ sudo config warm_restart enable syncd
SW$ sudo systemctl restart syncd          # syncd-only warm restart, no PRE_SHUTDOWN
```
Expect `mode=syncd restart (keep KNET)`, `state=live`, `mode=1 persist=1`, `Case A matched 72`,
traffic 0 loss, no `KNET kernel side re-initialised`. Exactly the pre-phase-4 behaviour.

## Stage 1: kexec-mode deinit leaves DMA idle (no reboot)

```
SW$ sudo config warm_restart enable syncd
SW$ sudo touch /host/warmboot/kexec-pending
SW$ sudo cat /proc/bcm/knet/dma > /tmp/knet_dma.before
SW$ sudo systemctl stop swss syncd          # warm stop (flag set), kexec mode via kexec-pending
SW$ cat /host/warmboot/sai-warmboot.bin.dma_idle
SW$ sudo /usr/share/sonic/device/x86_64-accton_as5812_54t-r0/warm-reboot_plugin --check; echo rc=$?
SW$ sudo cat /proc/bcm/knet/dma
```
Look for:
- log: `kexec-mode deinit forced by /var/warmboot/kexec-pending`, `soc_knet_hw_reset(-1) rv=0`, `dma_idle=ok`.
- marker starts with `ok boot=<current boot_id>`; plugin `--check` prints `registers: idle ...`, rc=0.
- `/proc/bcm/knet/dma`: no active RX/TX chains (compare with the `.before` copy).
- traffic harness: **0 loss** (the data plane does not use KNET).
- if `busy`: the `regs:` list names the engine (pktdma = KNET ring, fifodma = L2 mod FIFO,
  sbusdma/statdma = counters). Stop here and investigate; do not go to stage 4.

Restart (this is the `dma-reset` path):
```
SW$ sudo rm /host/warmboot/kexec-pending
SW$ sudo systemctl start swss
```
Look for: `state=dma-reset`, `mode=0 persist=1`, `KNET kernel side re-initialised (dma-reset, rx active)`,
`Case A ... matched 72`, `consumed /var/warmboot/sai-warmboot.bin.dma_idle`; `/proc/bcm/knet/dma`
active again; ping the switch SVI / LLDP neighbours back; traffic 0 loss.

## Stage 2: simulated kexec = reload the kernel modules (Case B, no reboot)

```
SW$ sudo config warm_restart enable syncd
SW$ sudo touch /host/warmboot/kexec-pending
SW$ sudo systemctl stop swss syncd
SW$ cat /host/warmboot/sai-warmboot.bin.dma_idle                  # ok ...
SW$ time sudo rmmod linux-knet-cb linux-bcm-knet linux-user-bde linux-kernel-bde   # ~30 s
SW$ sudo systemctl restart td2p-prep    # modprobe + /dev/linux-user-bde (or opennsl-modules start)
SW$ ls -la /dev/linux-user-bde; grep -c . /proc/bcm/knet/link     # header only
SW$ sudo rm /host/warmboot/kexec-pending
SW$ sudo systemctl start swss
```
Look for:
- `wb: knet fresh=1 state=fresh (... netifs 0 ...)`, `mode=0 persist=0`,
  `KNET kernel side re-initialised (fresh, rx active)`, `wb knet: Case B recreated 72/72`,
  `wb: true warm boot completed`.
- `/proc/bcm/knet/link` lists 72 netdevs; `DefaultRxAPI` present
  (`sudo cat /proc/bcm/knet/filters 2>/dev/null` or `bcmcmd knet filter show` via the diag socket).
- `show warm_restart state` all reconciled; ARP/ND/LLDP/BGP back; **traffic 0 loss throughout**.
- `dmesg | grep -iE 'nobody cared|oops|bug:|dma|bkn'`: no new errors.
- Repeat 3 times. Once with VXLAN/EVPN, ACL, CoPP, QoS and mirror configured.
Note: the BDE DMA pool is reallocated here, as after a kexec.

## Stage 3: plugin dry run

```
SW$ P=/usr/share/sonic/device/x86_64-accton_as5812_54t-r0/warm-reboot_plugin
SW$ sudo $P --check; echo rc=$?     # syncd running: expect "busy pktdma=N ..." rc=1
SW$ sudo touch /host/warmboot/kexec-pending; sudo systemctl stop swss syncd
SW$ sudo $P --check; echo rc=$?     # expect verdict "ok boot=<this boot> [this boot: yes]", "idle", rc=0
SW$ sudo rm /host/warmboot/kexec-pending; sudo systemctl start swss
```
`--check` takes no netdev down and never rmmods/reboots. A `register check failed` (rc=2) means BAR0
could not be mapped (look for `0000:01:00.0`, vendor 0x14e4 device 0xb864): fix before stage 4, or the
hook will always take the rmmod path.

## Stage 4: real warm-reboot, L2/L3 only

```
SW$ sudo config warm_restart disable syncd           # else warm-reboot aborts (check_warm_restart_in_progress)
SW$ sonic-installer list                             # Next == Current (same image only)
SW$ cat /sys/kernel/kexec_loaded                     # 0
SW$ docker exec swss orchagent_restart_check -n -w 2000 -r 1   # must succeed. -n is REQUIRED: without it orchagent
   # stays frozen (there is no resume) and warm-reboot then fails with "RESTARTCHECK failed" (code 10);
   # recover with: config warm_restart enable swss; systemctl restart swss; config warm_restart disable swss
   # SONiC 202505 warm-reboot aborts (code 30, "leftover CPA tunnel") whenever ASIC_DB has a VXLAN tunnel,
   # i.e. with any EVPN config. Run a copy with that check skipped: /home/admin/sai-td2p/wr/warm-reboot
   # start the traffic harness (L2 flow, then L3 in a second run) and the serial capture
SW$ sudo warm-reboot -v
```
Look for (before kexec: syslog / `/host/logs_before_reboot` if /var/log is tmpfs; after: plugin.log):
- `pre-shutdown-succeeded`, `wb: PRE_SHUTDOWN: system warm reboot`, `mode=KEXEC`, `dma_idle=ok`,
  `wb: shutdown total` well under 20 s.
- plugin.log: `took 72 KNET netdev(s) down`, `registers: idle`, `kexec may proceed`.
- serial console: no oops; new kernel boots.
- after boot: `cat /proc/cmdline` has `SONIC_BOOT_TYPE=warm`; log `state=fresh`, `bootid ... CHANGED`
  (or saved == none if the scache predates phase 4), `Case B recreated 72/72`, `true warm boot completed`;
  `cat /proc/linux-kernel-bde` interrupt count rising; `show warm_restart state` reconciled;
  `show reboot-cause` = warm-reboot.
- **0 data-plane loss** on the flows for the whole window, no link flap on the PC NICs
  (`ip monitor link` on the PC, pc-link-watchdog silent).
- post: ASIC_DB object count equal to before (`redis-cli -n 1 dbsize`), counters sane, second
  `warm-reboot` right after (warm after warm), then a cold `sudo reboot` (cold path still fine).

If plugin.log says `rmmod` or `COLD reboot`: the stop did not leave DMA idle. Save
`/host/warmboot/plugin.log`, `sai-warmboot.bin.dma_idle` and the syslog of the stop, and go back to stage 1.

## Stage 5: full feature warm-reboot

Same as stage 4 with VXLAN/EVPN (PC FRR with graceful restart, see plan stage 0), RIOT SVI, ACL,
CoPP, QoS and mirror active. Expect the VXLAN flow 0 loss, EVPN routes held by GR on the PC,
CoPP policer counters continuing.

Failure paths, once each:
- `sudo rm /host/warmboot/sai-warmboot.bin.hwstate` before `warm-reboot`: after kexec expect
  `warm boot sidecar files missing or invalid ... falling back to cold SDK init (... knet verdict 2, host kexec'd ...)`,
  a clean cold init (traffic loss accepted), no crash.
- `docker kill syncd` after pre-shutdown succeeded (watch syslog for `pre-shutdown-succeeded`, then kill
  from a second shell): plugin.log must show `syncd verdict: none` or a stale one, `registers: busy`,
  then the `rmmod` and, if needed, the `kexec -u` + cold reboot path.
- Optional: `sudo touch /host/warmboot/plugin-unload-knet` to force the rmmod path once (costs ~30 s
  of extra control-plane downtime); remove it afterwards.

## Rollback

- Any stage: a cold `sudo reboot` brings back the cold init with our library.
- Remove leftovers: `sudo rm -f /host/warmboot/kexec-pending /host/warmboot/plugin-unload-knet`.
- Plugin off: `sudo rm /usr/share/sonic/device/x86_64-accton_as5812_54t-r0/{warm,fast}-reboot_plugin`
  (or restore a `.orig` copy if install.sh made one). Without it warm-reboot simply skips the hook.
- Previous library: `PC$ deploy/install.sh --stop --lib <previous libsaivs.so>`; vendor SAI: `deploy/rollback.sh --stop`.
- A failed `warm-reboot` before step 12 of the script cleans up itself (`kexec -u`,
  `config warm_restart disable`); after that it is committed: let it finish, then cold reboot if needed.
