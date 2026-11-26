# QoS scheduler / shaper / WRED / buffer: hardware results (2026-11-26)

Branch `qos-merge` = main d8ff2dc + qos-sched 4132642, lib e9a3ba13 (build_wb.sh, ALL CHECKS PASSED).
Topology: PC (r8169, 1G) -> Ethernet30 (Vlan100) -> Ethernet39 (Vlan100) -> switch mgmt port eth0 moved to
netns hostA (192.168.52.50). Sender `tools/qos-test/udpblast.py` (UDP, IP TOS), sampling on the switch with
`tools/qos-test/qsample.sh` (diag `show c all`, two samples) and hostA rx_bytes.

| item | result |
|---|---|
| hwsku templates + `config qos reload` (cold) | no errors, 66 ports, `BUFFER_MAX_PARAM_TABLE|global mmu_size=16777216` |
| ASIC programming (test overlay on Ethernet39) | DWRR weights 1 / 4, queue shaper KbpsMax 100000, WRED 104000..416000 10 % + ECN_MARKING_EN |
| queue shaper 100 Mbit/s (UC1, 794 Mbit/s offered) | 104 Mbit/s received, excess dropped on UC1 |
| port shaper (PORT_QOS_MAP scheduler, 200 / 100 Mbit/s) | 200 / 100 Mbit/s received |
| DWRR 1:4 (UC1 vs UC5, both backlogged, 100 M port) | 20.8 : 83.2 Mbit/s = 4.00 |
| DWRR 14:4:1 (UC0, UC5, UC1) | 57.7 : 16.5 : 4.1 Mbit/s |
| ECN, congested, ECT(0) | 51300 / 51300 received packets CE marked |
| ECN, congested, non-ECT | 0 marked, tail/WRED drops |
| ECN, not congested | 0 marked |
| buffer profile update (egress_lossy_profile dynamic_th 3 -> 0) | Q_SHARED_ALPHA 0xa -> 7 on 288 queues, ingress drops move to egress queue drops |
| revert (`config qos reload`) | weights 14/15, shapers 0, WRED back to AZURE_LOSSY, alpha 0xa |
| cold swss restart with QoS in config_db | ok |
| kexec warm-reboot with QoS + pseudo EVPN | true warm boot, `qos: warm restored 528 PG / 528 queue / 528 qgroup-min snapshot(s) (zero HW writes)`, no QoS ASIC ops in APPLY_VIEW, MMU tables (THDU/THDI/WRED/Q_TO_QGRP) identical before/after, PC carrier unchanged, NUM_COS=8 |

## Problems found

1. **syncd SIGSEGV on any scheduler operation after a warm boot** (open, see notes/agy_task_wb_lls_sched_crash.md).
   After the kexec warm boot both the diag command `cos port show PortBitMap=xe39` and a SAI scheduler bind
   (CONFIG_DB `QUEUE|Ethernet39|6 scheduler t_w2`) crashed syncd at the same address
   (libsaivs exec segment +0x1cb599 = `soc_td2_sched_hw_index_get`+0x89, NULL deref). Works on a cold boot.
   Not introduced by qos-sched (the scheduler path exists on main), but QoS is not usable after a warm boot
   until fixed. First crash: syncd warm-restarted by itself (Case A, no loss seen). Second crash happened with
   the warm flag already cleared: swss hit start-limit, manual `reset-failed` + cold start.
2. `config qos reload` on a running system leaves `SCHEDULER|scheduler.N` DEL+SET pending in orchagent
   (still referenced by queues), so the next warm-reboot is refused (`RESTARTCHECK failed`). Upstream
   behaviour; needs a swss restart (or avoid reloading twice).
3. Template: queues use `egress_lossy_profile` (dynamic_th 3) and PGs `ingress_lossy_profile` (dynamic_th 3)
   on equal pools, so with one ingress port feeding congested queues the ingress limit drops first
   (DROP_PKT_ING on the ingress port, no egress queue drops). `q_lossy_profile` is defined but unused.
4. The build tree ~/homelab/sonic-sairedis-merge had a stale hwbcm_core.c: libs f0cf4e10 and 3b1688a3 were
   built without 16c9d22 (NUM_COS guard) and 8d046dd (autoneg skip). e9a3ba13 has both.
5. Diag port names change after a warm boot (ge0/ge1 -> xe30/xe39).
6. PC side: r8169 `NETDEV WATCHDOG: transmit queue timed out` above ~450 Mbit/s of UDP; keep tests below.

Logs: notes/2026-11-26_wr-qos_syslog.log.gz, notes/2026-11-26_syncd_segfault_lines.txt.
