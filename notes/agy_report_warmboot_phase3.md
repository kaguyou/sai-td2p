# Warm Boot Phase 3 Report (`sai-td2p-wb`, branch `warmboot`)

**Date:** 2026-11-24
**Branch:** `warmboot` (isolated worktree `/home/kaguyou/homelab/sai-td2p-wb` + build copy `/home/kaguyou/homelab/sonic-sairedis-wb`)
**Design Reference:** `notes/design_warmboot.md`
**Previous Phase Report:** `notes/agy_report_warmboot_phase1.md`

---

## 1. Summary & Build Artifacts

**Phase 3** completes `.hwstate` binary TLV persistence and zero-HW-write warm recovery across **all remaining SAI/hwbcm subsystems**:
1. **Hash settings** (RTAG7 ECMP/LAG fields & seeds + base switch OID restoration)
2. **VXLAN / EVPN / RIOT** (VPNs, access & network VPs, tunnels, remote VTEP state, RIOT SVI bindings / overlay intfs, overlay egresses, remote MACs)
3. **ACL** (FP groups, entries/variants per binding scope, counters + software offsets, ranges, table groups & bind points)
4. **CoPP** (dedicated IFP group, policers, installed entries, stat IDs + color baselines, pending to-me traps + `g_qmax` probe)
5. **QoS** (content-keyed BCM map cache + per-port map keys / `pfcTouched`, buffer pools & service-pool usage, first-touch PG/queue MMU snapshots, scheduler/WRED/buffer desired-state bindings)
6. **Mirror** (SPAN/RSPAN/ERSPAN destinations, port ingress/egress mirror sets, `m_aclMirrorDest` integration)
7. **Stats baselines** (sparse non-zero `READ_AND_CLEAR` software baselines `g_qbase` / `g_xbase`)

After a warm `syncd` restart, **every SAI object across every implemented subsystem can be queried, modified (`set`), or deleted (`remove`) without `BCM_E_NOT_FOUND`**, and **zero hardware registers/tables are rewritten during warm recovery**.

`deploy/build_wb.sh` built cleanly with **zero warnings in all new/modified code** (`-Werror` on all C++ translation units) and passed every symbol, ABI, and `ldd` check (**`ALL CHECKS PASSED`**). Standalone `hwtest_wb` also rebuilt cleanly.

| Artifact | Path | Size | SHA-256 |
|---|---|---|---|
| **Phase 3 `libsaivs.so.0.0.0`** | `/home/kaguyou/homelab/sai-td2p-wb/deploy/build/libsaivs.so.0.0.0` | `150,359,176` bytes (144 MiB) | `0bc7e152ac08bdc4e293934d06ef0b97d362b4adae09cf4f8b6b3c1558fa80cb` |
| **Phase 2 backup (UNTOUCHED)** | `/home/kaguyou/homelab/sai-td2p-wb/deploy/build/libsaivs.so.wb-phase2` | `149,072,464` bytes (143 MiB) | `b4ca0027a4e0a50c258c38411fd77f878aa4cd70c915413171a139c7347dc749` |
| **Standalone `hwtest_wb`** | `/home/kaguyou/homelab/sai-td2p-wb/hwtest/hwtest_wb` | `98,099,192` bytes (94 MiB) | `5106d3194f06c712bbbc43c9288915eb3da56e99acf96771490e3f4872b94771` |
| **Merged WB SDK archive** | `/home/kaguyou/homelab/sai-td2p-wb/deploy/build/libopenbcm_td2p_wb.a` | `151,304,566` bytes (1,962 members) | — |

### `deploy/build_wb.sh` Verification Highlights
- **Status:** `ALL CHECKS PASSED`
- **Undefined symbols:** `0` undefined `bcm_*/soc_*/sal_*/bde_*/hwbcm_*` symbols
- **ABI tags:** `GLIBC max 2.34 <= 2.36`, `GLIBCXX max 3.4.30 <= 3.4.30`, `CXXABI max 1.3.9 <= 1.3.13`
- **Exported/defined symbols:** `67,117` dynamic symbols (`32,524` `bcm_*`, `200` `hwbcm_*` — +23 new Phase 3 `hwbcm_*` warm-boot helpers, `sai_api_*`, `bde`, `bde_create`)
- **Warm-boot SDK & `hwbcm` symbols:** All 14 required WB symbols + 45 `_bcm_esw_*_sync` symbols present.

---

## 2. What Is Implemented

### `.hwstate` TLV Schema Extension (Backward- & Forward-Compatible, Version 1)
All Phase 3 subsystems append new TLV tags `10..23` inside the existing CRC32-protected version-1 `.hwstate` payload (`SwitchBCM56860HW_wb.inc.cpp`). Unknown tags continue to be skipped cleanly, and loading a Phase 2 `.hwstate` file (tags `1..9` only) cleanly resets Phase 3 SW tables without failing.

| Tag | Name | Persisted HW/SW State | Rebuilt From `m_objectHash` (Zero HW Writes) |
|---|---|---|---|
| `10` | `HWSTATE_TAG_HASH` | `m_ecmp_hash_id`, `m_lag_hash_id`, `g_hash_fields[0..1]`, `g_hash_seed[0..1]` | Base switch OIDs (`m_default_vlan_id`, `m_default_1q_bridge`, `m_ecmp_hash_id`, `m_lag_hash_id`) via `hwWarmRestoreSwitchOids()` |
| `11` | `HWSTATE_TAG_VXLAN_GLOBALS` | `m_vxlanInited`, `m_vxlanStationId`, `m_vtepTunnel`, `m_vtepSrcValid`, `m_vtepSrcIp4[4]`, `m_vtepTtl`, `m_vtepDscp`, C-layer `s_vxlan_inited`, `s_udp_port` | Fallback P2MP local VTEP attrs if absent |
| `12` | `HWSTATE_TAG_VXLAN_VPNS` | `m_vniToVpn`, `m_vpnAccessVps`, C-layer `s_vp[]`, `s_acc_intf[]`, `s_vpn_l3[]` | `m_tunnelMapType`, `m_mapEntryVidVni`, `m_vidToVni`, `m_vniRefs` |
| `13` | `HWSTATE_TAG_VXLAN_VTEPS` | `m_tunnelInit`, `m_tunnelTerm`, `m_vtepByIp` (`ip`, `ip4`, `tunnelInit`, `tunnelTerm`, `egr`, `srcEgr`, `vp`, `egrMc`, `vpMc`, `vpReady`, `vpVpn`, `port`, `floodVpns`) | `m_tunnelRemoteIp`, `m_termEntryTunnel`, `m_bridgeToVpn`, `m_bridgePortRemoteIp`, `m_bridgePortToVp` (when `vpReady`), `VxlanRemoteVtep::{tunnels, bridgePorts, macs}` |
| `14` | `HWSTATE_TAG_ACL_TABLES` | `m_aclTables` (`saiStage`, `hwStage`, `quals`, `gid`, `prio`, `matchesPorts`) + `hwbcm_acl_group_restore()` | `m_aclPrioUsed`, `AclTableHw::entries` |
| `15` | `HWSTATE_TAG_ACL_ENTRIES` | `m_aclEntries` (`table`, `counter`, `hw[]={eid, stat}`) | Linked into `AclTableHw::entries` and `AclCounterHw::entries` |
| `16` | `HWSTATE_TAG_ACL_COUNTERS` | `m_aclCounters` (`table`, `pktOffset`, `byteOffset`) | Missing zero-offset counters backfilled from `m_objectHash` |
| `17` | `HWSTATE_TAG_ACL_RANGES` | `m_aclRanges` (`oid -> range_id`) | — |
| `18` | `HWSTATE_TAG_COPP` | `m_coppLocalEgress`, `m_coppPolicer`, `m_coppTrapHandle`, C-layer `g_group*`, `g_local_egr`, `g_pol[]`, `g_slot[]` (including pending to-me slots with `neid==0`, `eid[]`, `stat_id[]`, color counter baselines) | `g_qmax` probed read-only via `bcm_rx_queue_max_get()` |
| `19` | `HWSTATE_TAG_QOS_MAPS` | `m_qosHwMaps` (`key -> {id, refs}`), per-port `hwIngDscpKey`, `hwIngDot1pKey`, `hwEgrDscpKey`, `pfcTouched` | `m_qosSwitchMaps`, `m_qosSwitchDefaultTc`, `m_qosMapType`, per-port `maps[]`, `defaultTc`, `pfc*` |
| `20` | `HWSTATE_TAG_QOS_POOLS` | `m_qosIngressSpUsed[4]`, `m_qosEgressSpUsed[4]`, `m_qosPools`, `m_qosPgPending` | `m_qosProfiles`, `m_qosPgProfile`, `m_qosQueueProfile` |
| `21` | `HWSTATE_TAG_QOS_SNAPS` | C-layer first-touch MMU snapshots `s_pg_snap[]` and `s_q_snap[]` | `m_qosSchedulers`, `m_qosSgSched`, `m_qosQueueSched`, `m_qosWreds`, `m_qosQueueWred`, `m_qosPort[].scheduler` |
| `22` | `HWSTATE_TAG_MIRROR` | `m_mirrorSessions` (`oid -> destGport`) | `MirrorSessionHw::cfg` (via `mirrorReadConfig`), `m_aclMirrorDest`, `m_portIngMirrors`, `m_portEgrMirrors` |
| `23` | `HWSTATE_TAG_STATS_BASE` | Sparse non-zero `g_qbase` and `g_xbase` `READ_AND_CLEAR` baselines | `m_statPoolUser` cleared (repopulated lazily from restored `m_qosPgProfile`/`m_qosQueueProfile`) |

### Per-Subsystem Implementation Details

1. **Base Switch OIDs & Hash Settings** (`hwbcm.h`, `hwbcm_core.c`, `SwitchBCM56860HW.cpp`, `SwitchBCM56860HW_wb.inc.cpp`):
   - **Critical fix discovered in base `vslib`**: `SwitchStateBase::warm_boot_initialize_objects()` did **not** restore `m_default_vlan_id`, `m_default_1q_bridge`, `m_ecmp_hash_id`, or `m_lag_hash_id`. Added `hwWarmRestoreSwitchOids()` immediately after `SwitchBCM56850::warm_boot_initialize_objects()` so `hwSetHash()` and `hwRemoveRouterInterface()` work identically after warm boot.
   - `hwbcm_core.c` tracks `g_hash_fields[2]` and `g_hash_seed[2]` across `hash_init()`, `hwbcm_hash_fields_set()`, and `hwbcm_hash_seed_set()`. `hwbcm_hash_wb_restore()` restores them in SW and read-only cross-checks `bcmSwitchHashSeed0/1` and `bcmSwitchHashControl` against hardware (zero HW writes).

2. **VXLAN / EVPN / RIOT** (`hwbcm.h`, `hwbcm_vxlan.c`, `hwbcm_core.c`, `SwitchBCM56860HW_wb.inc.cpp`):
   - `hwbcm_vxlan_warm_restore()` sets `s_vxlan_inited` and `s_udp_port` directly **without calling `bcm_vxlan_init(0)`** (which would wipe live VXLAN HW state), probes `bcmSwitchVxlanUdpDestPortSet` read-only, and restores `s_vp[]`, `s_acc_intf[]`, and `s_vpn_l3[]` (RIOT SVI overlay L3 interfaces).
   - Overlay egresses created by `hwbcm_vxlan_overlay_egress_create/replace()` live in `m_neighborEgr` / `m_nhToEgr` (already persisted by Phase 2); combined with restored `s_vpn_l3[]`, `m_vtepByIp`, and `m_bridgePortRemoteIp`, RIOT neighbor re-resolution and removal work seamlessly post-warm-boot.
   - `hwbcm_vxlan_warm_reset()` is called from `hwbcm_warm_deinit()` / `hwbcm_deinit()` (keeping `hwtest_wb` self-contained).
   - `hwWarmRebuildVxlanFromHash()` rebuilds all pure-SW lookup maps (`m_tunnelMapType`, `m_mapEntryVidVni`, `m_vidToVni`, `m_vniRefs`, `m_tunnelRemoteIp`, `m_termEntryTunnel`, `m_bridgeToVpn`, `m_bridgePortRemoteIp`, `m_bridgePortToVp`, and `VxlanRemoteVtep::{tunnels, bridgePorts, macs}`) from `m_objectHash`.

3. **ACL** (`hwbcm_acl.h`, `hwbcm_acl.c`, `SwitchBCM56860HW.cpp`, `SwitchBCM56860HW_wb.inc.cpp`):
   - `hwbcm_acl_group_restore(gid, stage, quals)` restores `s_groups[]` metadata and read-only cross-checks each group via `bcm_field_group_status_get()`.
   - Persists every table (`gid`, `prio`, `quals`, `matchesPorts`), every entry's per-binding-scope variants (`eid`, `stat`), every counter's accumulated software offset (`pktOffset`, `byteOffset`), and every range ID (`m_aclRanges`).
   - `hwWarmRebuildAclFromHash()` rebuilds `m_aclPrioUsed`, links `T.entries` and `C.entries`, rebuilds `m_aclGroups`, `m_aclGroupMembers`, and `m_aclBindings` across `PORT`, `LAG`, `VLAN`, `ROUTER_INTERFACE`, and `SWITCH`.
   - `hwWarmRebuildMirrorFromHash()` runs before ACL recovery so `m_aclMirrorDest` is populated for ACL entries with mirror actions.

4. **CoPP** (`hwbcm_copp.h`, `hwbcm_copp.c`, `SwitchBCM56860HW.cpp`, `SwitchBCM56860HW_wb.inc.cpp`):
   - `hwbcm_copp_wb_get()` / `hwbcm_copp_warm_restore()` persist and restore `g_group*`, `g_local_egr`, `g_pol[32]`, and `g_slot[64]` — including **pending "to me" traps** (`neid == 0`) and accumulated color counter baselines (`base_nr_pkts`, `base_nr_bytes`, `base_red_pkts`).
   - **Key bug avoided**: `hwbcm_copp_warm_restore()` probes `bcm_rx_queue_max_get(0, &g_qmax)` read-only so `cfg_check()` (`cfg->queue > g_qmax`) succeeds on post-warm-boot trap updates even though `group_ensure()` short-circuits when `g_group_valid == 1`. Also cross-checks `g_group` with `bcm_field_group_status_get()`.

5. **QoS** (`hwbcm_qos.h`, `hwbcm_qos.c`, `SwitchBCM56860HW_qos.cpp`, `SwitchBCM56860HW.cpp`, `SwitchBCM56860HW_wb.inc.cpp`):
   - Persists `m_qosHwMaps` (cross-checked read-only via `hwbcm_qos_map_exists()` using `bcm_qos_multi_get()`), per-port active map keys (`hwIngDscpKey`, `hwIngDot1pKey`, `hwEgrDscpKey`) and `pfcTouched`, `m_qosPools` + `m_qosIngressSpUsed[4]` / `m_qosEgressSpUsed[4]`, `m_qosPgPending`, and `hwbcm_qos.c`'s first-touch boot-time MMU snapshots (`s_pg_snap[]` / `s_q_snap[]`) so unbinding a PG or queue buffer profile after warm boot restores true boot-time MMU defaults.
   - `qosBuildHwMap(true)` rebuilds all pure-SW QoS desired-state maps (`m_qosSwitchMaps`, `m_qosSwitchDefaultTc`, `m_qosPort` bindings/PFC/shaper, `m_qosSchedulers`, `m_qosSgSched`, `m_qosQueueSched`, `m_qosWreds`, `m_qosQueueWred`, `m_qosProfiles`, `m_qosPgProfile`, `m_qosQueueProfile`) from `m_objectHash` with **zero HW writes**, while preserving the `.hwstate`-restored map keys and `pfcTouched` flags.

6. **Mirror** (`hwbcm_mirror.h`, `hwbcm_mirror.c`, `SwitchBCM56860HW_wb.inc.cpp`):
   - Persists `m_mirrorSessions` (`oid -> destGport`).
   - `hwWarmRebuildMirrorFromHash()` read-only cross-checks each `destGport` via `hwbcm_mirror_dest_exists()` (`bcm_mirror_destination_get()`), reconstructs `MirrorSessionHw::cfg` via `mirrorReadConfig()` (using already-restored `m_portToBcm` and `m_lagToTrunk`), populates `m_aclMirrorDest`, and rebuilds `m_portIngMirrors` / `m_portEgrMirrors` from `m_objectHash[SAI_OBJECT_TYPE_PORT]`.

7. **Stats Baselines** (`hwbcm_stats.h`, `hwbcm_stats.c`, `SwitchBCM56860HW.cpp`, `SwitchBCM56860HW_wb.inc.cpp`):
   - `hwbcm_stats_wb_get_qbase()` / `hwbcm_stats_wb_get_xbase()` emit only non-zero entries from `g_qbase` and `g_xbase` (compact sparse encoding); `hwbcm_stats_warm_restore()` reloads them under `g_lock` without touching HW counters so `READ_AND_CLEAR` deltas stay monotonic across warm restart.
   - `~SwitchBCM56860HW()` calls `hwbcm_{acl,copp,qos,stats}_warm_reset()` cleanly on destructor teardown (kept outside `hwbcm_core.c` so `hwtest_wb` link boundaries stay unchanged).

---

## 3. What Is Left (Phase 4 & Production Merge)

1. **Phase 4 — Host `warm-reboot` (kexec) Hardening**:
   - Automate `/dev/linux-user-bde` (`c 126 0`) creation across host kexec (udev rule / systemd drop-in on `opennsl-modules`).
   - Full `config warm_restart enable system` + `sudo warm-reboot` end-to-end validation with VXLAN/EVPN/RIOT, ACL, CoPP, QoS, Mirror, and Stats active.
2. **Production Integration (when ready to merge from `warmboot` branch)**:
   - Merge `warmboot` branch into main `sai-td2p` and apply updated `sairedis/vslib.patch` + `vslib/SwitchBCM56860HW*` + `vslib/hwbcm/*` to main `sonic-sairedis` and `sdk-6.5.27`.
   - Update `deploy/build.sh` (or merge `deploy/build_wb.sh`) to use `sdk-6.5.27-wb`, `sdk_cflags_wb.txt`, `sdk_incs_wb.txt`, and `sdk_link_list_wb.txt`.

---

## 4. Exact Hardware Test Steps for the Lead (Edgecore AS5812-54T)

> **Note:** None of these commands were run by agy; `libsaivs.so.wb-phase2`, `/home/kaguyou/homelab/sai-td2p`, `/home/kaguyou/homelab/sonic-sairedis`, and the switch were untouched.

### 1. Deploy Phase 3 `libsaivs.so.0.0.0` into `syncd`
```bash
scp /home/kaguyou/homelab/sai-td2p-wb/deploy/build/libsaivs.so.0.0.0 admin@<switch>:/tmp/libsaivs.so.0.0.0.wb-phase3
# On switch:
sudo docker cp /tmp/libsaivs.so.0.0.0.wb-phase3 syncd:/usr/lib/x86_64-linux-gnu/libsaivs.so.0.0.0
sudo docker exec syncd sha256sum /usr/lib/x86_64-linux-gnu/libsaivs.so.0.0.0
# Expect: 0bc7e152ac08bdc4e293934d06ef0b97d362b4adae09cf4f8b6b3c1558fa80cb
```

### 2. Provision Phase 3 Subsystems Before Warm Restart
Bring up a configuration exercising all Phase 3 subsystems (cold boot once after installing Phase 3 `.so`, or add objects before warm sync):
- **VXLAN / EVPN / RIOT**: Local VTEP (`LOOPBACK_INTERFACE`), at least one VLAN↔VNI mapping (`VXLAN_EVPN_NVO` + `VXLAN_TUNNEL_MAP`), local VLAN member ports, a remote VTEP + EVPN remote MAC, and an SVI (`VLAN_INTERFACE`) on the VNI-mapped VLAN (RIOT) with a routed neighbor behind the remote VTEP.
- **ACL**: At least one `L3` or `L3V6` ingress ACL table bound to front-panel port(s)/LAG/VLAN with rules + counters (verify counter increments on traffic) and an L4 port-range rule if configured.
- **CoPP**: Standard SONiC CoPP traps + policers (including `bgp`/`ssh`/`ip2me` "to me" traps and ARP/LLDP/LACP).
- **QoS**: DSCP-to-TC / TC-to-Queue / TC-to-PG maps bound to ports, buffer pools/profiles bound to PGs and queues, scheduler weights/shapers, and a WRED/ECN profile on a unicast queue.
- **Mirror**: One SPAN or ERSPAN mirror session attached to a front-panel port (and/or referenced by an ACL `MIRROR_INGRESS_ACTION` rule).
- **Hash**: Custom ECMP/LAG hash fields and/or seed (`config switch-hash ...` / `SAI_HASH_ATTR_NATIVE_HASH_FIELD_LIST`).
- **Stats**: Run `sonic-clear queuecounters` / `sonic-clear counters` (or trigger `READ_AND_CLEAR` on a queue/port counter) so non-zero software baselines exist in `g_qbase`/`g_xbase`.

### 3. Perform Warm `syncd` Restart Under Live Traffic
```bash
sudo config warm_restart enable syncd
sudo systemctl restart syncd
```
**Immediate pass criteria (logs + dataplane):**
- `syncd` syslog shows:
  - `wb hwstate: saved ... bytes (... vpns=... vteps=... aclT=... aclE=... aclC=... aclR=... coppT=... coppP=... qosM=... qosP=... mirrors=...)`
  - `wb: restored base switch OIDs (defVlan=oid:... def1QBridge=oid:... ecmpHash=oid:... lagHash=oid:...)`
  - `wb hwstate: restored ... vpns=... vteps=... aclT=... aclE=... aclC=... aclR=... coppT=... coppP=... qosM=... qosP=... qosSnap=.../... mirrors=... statsBase=.../...`
  - `qos: hardware map built (warm/SW-only): ...`
  - `wb: true warm boot completed (zero ASIC resets, zero port flaps)`
- **Zero warnings** for missing SDK handles (`not found in SDK`, `group ... restore failed`, `dest gport ... not found`).
- **0 packets dropped** on L2, L3, VXLAN L2-VNI, and RIOT SVI streams; **0 link flaps**; ACL/CoPP/Mirror forwarding continues uninterrupted.

### 4. Post-Warm-Restart Mutability Verification (Zero `BCM_E_NOT_FOUND`)
Verify every Phase 3 subsystem accepts live `set` and `remove` calls after warm restart without errors in `syslog` / `syncd.log`:
1. **VXLAN / EVPN / RIOT**:
   - Delete a pre-warm-boot remote MAC and re-add it; verify remote traffic resumes.
   - Add and remove a local VLAN member on the VNI-mapped VLAN (verifies `m_vpnAccessVps` + `s_vp[]` + `s_acc_intf[]`).
   - Delete and recreate a RIOT neighbor or unmap the VLAN↔VNI mapping (verifies `s_vpn_l3[]` + `m_vniToVpn` + `m_vtepByIp` teardown without `BCM_E_NOT_FOUND`).
2. **ACL**:
   - Read a pre-warm-boot ACL counter (`aclshow -a`) — verify it retains its pre-warm count + new hits without resetting to 0 or jumping to raw HW count.
   - Modify an existing pre-warm-boot ACL rule (priority/action) and delete a pre-warm-boot ACL rule, counter, range, and table (verifies `s_groups[]`, `m_aclEntries`, `m_aclCounters`, `m_aclRanges`, `m_aclBindings`).
3. **CoPP**:
   - Update a pre-warm-boot CoPP policer rate (`CIR`/`CBS`), modify a trap group queue, and delete/re-add a hostif trap (including a "to me" trap like SSH/BGP) — verifies `g_group`, `g_qmax`, `g_pol[]`, `g_slot[]`.
   - Confirm BGP/SSH/ARP/LLDP control-plane sessions stay up and policed.
4. **QoS**:
   - Change a port's DSCP-to-TC map (verifies `m_qosHwMaps` refcount release/destroy without leaking or failing `BCM_E_NOT_FOUND`).
   - Unbind a PG and a queue buffer profile (verifies restored `s_pg_snap[]` / `s_q_snap[]` restore boot-time MMU defaults cleanly).
   - Update a scheduler weight and a WRED threshold on an existing queue.
   - Remove an unused buffer pool (verifies `m_qosIngressSpUsed` / `m_qosEgressSpUsed`).
5. **Mirror**:
   - Update a pre-warm-boot mirror session attribute (e.g. ERSPAN dst IP/MAC or monitor port), detach it from a source port, and delete the session — verify `hwbcm_mirror_dest_replace` / `destroy` succeed with zero `BCM_E_NOT_FOUND` and `m_aclMirrorDest` stays consistent.
6. **Hash**:
   - Update ECMP/LAG hash fields (`SAI_HASH_ATTR_NATIVE_HASH_FIELD_LIST`) and seed (`SAI_SWITCH_ATTR_ECMP_DEFAULT_HASH_SEED` / `LAG_DEFAULT_HASH_SEED`); verify `hwbcm_hash_fields_set` / `hwbcm_hash_seed_set` actually program hardware (verifies `m_ecmp_hash_id` / `m_lag_hash_id` restoration).
7. **Stats Baselines**:
   - Poll `show queue counters` / `show interfaces counters` immediately after warm restart — verify previously cleared counters remain near 0 (not jumping back to raw cumulative hardware counters).
8. **Back-to-back 2nd warm restart**:
   - Run `sudo systemctl restart syncd` a second time after the mutations above and confirm clean recovery and stable `.scache` / `.hwstate` sizes.

---

## 5. Isolation Verification

- Untouched trees: `/home/kaguyou/homelab/sai-td2p`, `/home/kaguyou/homelab/sonic-sairedis`, `/home/kaguyou/homelab/openbcm-src/sdk-6.5.27`.
- Untouched Phase 2 build: `/home/kaguyou/homelab/sai-td2p-wb/deploy/build/libsaivs.so.wb-phase2` (`sha256: b4ca0027a4e0a50c258c38411fd77f878aa4cd70c915413171a139c7347dc749`).
- No switch access (`ssh`/`scp`/`/dev/ttyUSB0`/`deploy/install.sh`) was performed.
- All changes are committed locally on branch `warmboot` in `/home/kaguyou/homelab/sai-td2p-wb` (never pushed).
