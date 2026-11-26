# agy task: QoS second half — scheduler / WRED / buffer (code + SONiC templates), no switch access

Repo: ~/homelab/sai-td2p-p4, branch `qos-sched` (just created from warmboot-phase4 81de466 = main content).
Build tree ~/homelab/sonic-sairedis-p4 (copy sairedis/vslib/* there by hand + Makefile.am list, like
before; DO NOT run sairedis/sync_from_sairedis.sh). Build:
`SAI_TD2P_DIR=$PWD SAIREDIS_DIR=~/homelab/sonic-sairedis-p4 ./deploy/build_wb.sh --no-meta --force-archive`.
Do not push, do not touch main or other worktrees. The lead tests on the switch; you do NOT ssh to it.

## Background
SONiC 202505 on Edgecore AS5812-54T, custom SAI (vslib SwitchBCM56860HW + hwbcm C layer on
OpenBCM sdk-6.5.27, Trident2+ BCM56864). QoS so far: DSCP->TC->queue maps verified on hardware
(DSCP46->UC5, DSCP10->UC1). Non-ETS mode, 8 cos per port, `mmu_lossless=0`.
Code: sairedis/vslib/SwitchBCM56860HW_qos.cpp (+ .inc.h), hwbcm/hwbcm_qos*.c (find them).
The hwsku dir has NO qos.json.j2 / buffers.json.j2 / buffers_defaults_*.j2 / pg_profile_lookup.ini,
so `config qos reload` pushes nothing for schedulers/WRED/buffers today.

## Goal
Make SCHEDULER, WRED and BUFFER objects from SONiC really program the ASIC, the way the Broadcom SAI does.
1. Audit what SwitchBCM56860HW_qos.cpp does today for:
   - SAI_OBJECT_TYPE_SCHEDULER (SCHEDULING_TYPE STRICT/DWRR/WRR, SCHEDULING_WEIGHT,
     MIN/MAX_BANDWIDTH_RATE/BURST, METER_TYPE) and binding via SAI_QUEUE_ATTR_SCHEDULER_PROFILE_ID /
     SAI_SCHEDULER_GROUP_ATTR_SCHEDULER_PROFILE_ID / SAI_PORT_ATTR_QOS_SCHEDULER_PROFILE_ID.
   - SAI_OBJECT_TYPE_WRED (GREEN/YELLOW/RED_ENABLE, MIN/MAX_THRESHOLD, DROP_PROBABILITY, ECN_MARK_MODE)
     bound via SAI_QUEUE_ATTR_WRED_PROFILE_ID.
   - BUFFER_POOL / BUFFER_PROFILE (static/dynamic th, xoff/xon, size) bound to
     SAI_INGRESS_PRIORITY_GROUP_ATTR_BUFFER_PROFILE and SAI_QUEUE_ATTR_BUFFER_PROFILE_ID.
   Write the audit (what is a stub / attribute-only / really programs hw) in the report.
2. Implement the missing hardware programming with the SDK cosq gport API (bcm_cosq_gport_sched_set,
   bcm_cosq_gport_bandwidth_set, bcm_cosq_gport_discard_set (bcm_cosq_gport_discard_t, ECN),
   bcm_cosq_control_set for bcmCosqControlEgressUCQueueMinLimitBytes / SharedLimitBytes /
   bcmCosqControlEgressUCSharedDynamicEnable / Alpha, ingress PG limits). Use the gport/queue mapping
   already used for queue stats (hwbcm_stats.c) so queue N of port P is the same object.
   Removal / re-bind must restore defaults. Keep warm boot working: every new hw state must be
   recoverable with zero hardware writes on warm (follow the existing .hwstate pattern used by
   ACL/CoPP/QoS in SwitchBCM56860HW_wb.inc.cpp: save on sync, restore maps on warm).
3. Add SONiC templates to deploy/ref/hwsku-qos/ for the AS5812-54T hwsku (Accton-AS5812-54T), modelled
   on a Trident2 platform in sonic-buildimage (e.g. device/dell/x86_64-dell_s6000_s1220-r0/Force10-S6000:
   qos.json.j2, buffers.json.j2, buffers_defaults_t0/t1.j2, pg_profile_lookup.ini). Port names
   Ethernet0..Ethernet71 per deploy/ref/port_config.ini (note Ethernet48 and Ethernet60 are now 1x40G
   and Ethernet49-51, 61-63 no longer exist). Lossy only (no PFC) is fine.
   Add an install step to deploy/remote_install.sh (copy into the hwsku dir only if absent/different).
4. Write deploy/test_qos_sched.sh for the lead: a step list that applies a scheduler (e.g. queue 1
   DWRR weight 1 vs queue 5 weight 4, and a max-rate shaper 100 Mbps on one queue) and a WRED profile,
   and shows how to verify in the diag shell (`cos port show`, `cos bandwidth_show`, `cos discard_show`
   or the SDK equivalents for TD2+) and with queue counters.
5. Build, ALL CHECKS must pass. Commit in logical pieces on qos-sched.

## Report
notes/agy_report_qos_sched_wred_buffer.md: audit table, what you implemented (functions, SDK calls),
warm-boot handling, templates added, test steps, sha256 of deploy/build/libsaivs.so.0.0.0. Short.
