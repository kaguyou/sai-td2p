# parity-fix-2 on hardware (2026-11-27, lib eff9c352 = 419a389)

Review: two independent read-only reviews of the 12 commits, no confirmed bug that breaks the box.
Open low-risk items carried to the next batch:
- VTEP teardown on underlay loss does not first punt RIOT overlay egresses that point at the VP (`_vxlan.cpp:642`).
- `ING_EN_EFILTER_BITMAPm` in `_soc_trident2_misc_init` is still rewritten on warm boot (trident2.c:13614); on this config the
  value was identical before/after, so no effect seen.
- Several next hops on one (rif, ip): only the neighbor's own egress is passed to `hwVxlanRetryPendingForNeighbor`.
- `AUTO_NEG_MODE` replay branch in cold fallback is a no-op (speed 0); IP2ME station is re-added on sibling trap removal.

## Cold deploy
install rc=0, swss/syncd/bgp up, ports Ethernet24/35 up, BGP EVPN peer established, remote VTEP 10.1.0.2 oper_up.
New ERR lines: none (only the known create-SWITCH 35 s watchdog). `Clear watermark failed`: 4 -> 0 after deploy.
L2 cache traps: BPDU, LACP, LLDP, CDP/UDLD present.

## Scale, real EVPN peer up (remote VTEP present), tools/scale/rs3.sh rs4.sh
| test | before (fix-1) | fix-2 |
|---|---|---|
| 10,000 IPv4 neighbors add | 339 s (30/s) | 46.5 s (215 neighbors/s) |
| 10,000 IPv4 neighbors del | 157 s | 53.8 s (186/s) |
| 100,000 IPv4 /24 add, EVPN up | 125/s | 970/s (103.1 s) |
| 100,000 IPv4 /24 del | ~800/s (no EVPN) | 1162/s |
| 10,000 IPv6 /64 add / del | - / 620/s | 1059/s / 1225/s |
syncd/orchagent stayed up, ERR counters unchanged (33/10) through all runs, VTEP stayed oper_up.

## kexec warm-reboot (/home/admin/sai-td2p/wr/warm-reboot -v, log wr-pf2.log)
- SONIC_BOOT_TYPE=warm, `true warm boot completed (zero ASIC resets, zero port flaps)`, KNET Case B 66/66.
- orchagent/vxlanmgrd/neighsyncd/fdbsyncd/teamsyncd/bgp all `reconciled`.
- PC link (enp2s0) down events: 0. Ping to the switch CPU: one 101 s gap (kernel restart), nothing else.
- Registers before/after (wr_pf2_regs_{before,after}.txt on the box): ING_EN_EFILTER_BITMAP, RTAG7_HASH_SEL,
  EGR_VLAN_CONTROL_1, ASF_PORT_CFG (by address), L2 cache: identical.
- After boot: BGP peer re-established, remote VTEP oper_up, no handleSaiFailure / cold fallback / warm_restore failed.

Not measured: loss on a path that crosses the ASIC over VXLAN during the warm-reboot (needs a second host/netns, sudo on the PC).
