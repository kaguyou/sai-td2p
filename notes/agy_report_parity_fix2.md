# Parity Fix Batch 2 Report (`parity-fix-2`)

- **Branch**: `parity-fix-2` (branched from `parity-fix-1` at `ce8a5b2`, 11 commits: 1 per item + 2-line SAL annotation fix `e3220e5` for Item 9)
- **Warm-boot binary**: `deploy/build/libsaivs.so.0.0.0` (`153846912` bytes, `ALL CHECKS PASSED`)
- **SHA-256**: `4091006d5e7f7d6e4ecd5c3f06f6fa912bfd2fb336294a77601f99170648f3cc`
- **Rules compliance**:
  - Zero hardware writes added to warm-boot init (all new indexes are derived SW state rebuilt from `m_objectHash` on warm restore).
  - `.hwstate` format remains backward-compatible with `parity-fix-1` (Item 9 appended trap enum slots 11..15 after `HWBCM_TRAP_STP = 10` using the existing `ntraps` length prefix in `HWSTATE_TAG_CORE`; older `ntraps = 11` files load cleanly with slots 11..15 initialized to `-1`).
  - SDK ALPM behaviour untouched; normal SONiC boot calls keep returning `SAI_STATUS_SUCCESS`.

---

## A. Scale / Performance

### 1. Neighbor create O(N) scan
- **Status**: fixed
- **Commit**: `e8f2cb7`
- **Summary**: Added derived binary-keyed `RifIpKey -> next hop oids` (`m_nhByRifIp`) and `nh oid -> RifIpKey` (`m_nhKey`) hash indexes maintained on `hwCreateNextHop`/`hwRemoveNextHop` and rebuilt on warm restore (`hwWarmRebuildLagAndL3FromHash`, not persisted). Replaced O(N_nh) `m_nhToEgr` scans in `hwCreateNeighbor`, `hwSetNeighbor`, `hwRemoveNeighbor`, `hwRefreshNeighborsOnLearn`, and `hwPuntNeighborsOnAge`.
- **On-box test**:
  ```bash
  tools/scale/rs4.sh nbr SET 0 10000
  tools/scale/rs4.sh nbr DEL 0 10000
  ```
  - **Before**: Add 10,000 neighbors = 339 s (~30/s, O(N^2), `syncd` 100% CPU); delete = 157 s (~180/s).
  - **After**: Add 10,000 neighbors independent of N, expected ~500–900/s (~11–20 s); delete expected ~500–900/s (~11–20 s).

### 2. Remaining hot-path full scans
- **Status**: fixed
- **Commit**: `b98fa9b`
- **Scans found and eliminated** (all derived indexes cleared on load/cold-fallback and rebuilt on warm restore with zero HW writes):

| Location / Hot Path | Scan Found | Cost Before | Cost After |
|---|---|---|---|
| `hwRemoveNextHop`, `hwRemoveNeighbor`, `hwCreateNeighbor` rollback (`_l3.cpp`) | `valueInMap(m_nhToEgr/m_neighborEgr, egr)` linear scan for egress refcount | O(N_nh + N_nbr) | O(1) via `m_egrNhRefCount` & `m_egrNeighborRefCount` |
| `hwRefreshNeighborsOnLearn`, `hwPuntNeighborsOnAge` (`_l3.cpp`) | Iterated all `m_objectHash[NEIGHBOR_ENTRY]` + JSON parse per FDB event | O(N_nbr) | O(k) via `m_vlanNeighborsByMac[(vlanOid, mac)]` |
| `hwReresolveNeighborsOnVlan` (`_l3.cpp`) | Iterated all `m_objectHash[NEIGHBOR_ENTRY]` per VLAN/VXLAN change | O(N_nbr) | O(k_vlan) via `m_vlanNeighborsByVid[vid]` |
| `neighborTarget` (`_l3.cpp`) | Scanned `m_objectHash[FDB_ENTRY]` + `sai_deserialize_fdb_entry` per neighbor | O(N_fdb) | O(log N_fdb) single canonical `find(sai_serialize_fdb_entry(fe))` |
| `groupEgressList`, `hwRemoveNextHopGroup` (`_l3.cpp`) | Scanned all `m_nhgMemberToNhg` per NHG member add/del/remove | O(N_nhg_members) | O(k_group) via `m_nhgToMembers[nhg]` & `m_ecmpToNhg[ecmp]` |
| `vxlanResolveUnderlayEgress` (`_vxlan.cpp`) | Scanned all `m_neighborEgr` + all `m_routeEgr` + JSON deserialize | O(N_nbr + N_route) | O(1) (<=33 prefix-len hash lookups via `m_v4NeighborsByVrfIp` & `m_v4RoutesByVrfLen[0..32]` + `m_v4RouteLenMask`) |
| `hwVxlanRetryPending` on neighbor/route/NH change (`_vxlan.cpp`) | Full `hwVxlanRetryPending()` called on every neighbor/route/NH op | O(N_vtep * (N_nbr + N_route)) | O(1) early-out when `m_vtepByIp.empty()`, plus targeted `ForNeighbor`/`ForRoute`/`ForNextHop` filtering |
| `vlanOidOf`, `vidOf`, MAC move (`SwitchBCM56860HW.cpp`), `hwVxlanTranslateL2Event` (`_vxlan.cpp`) | Linear scans of `VLAN`, `LAG_MEMBER`, `BRIDGE_PORT`, `m_vniToVpn` | O(N_vlan / N_lag / N_bp / N_vni) | O(1) via `m_vidToVlanOid`/`m_vlanOidToVid`, `m_portToLag`, `m_targetToBridgePort`/`m_bridgePortToTarget`, `m_vpnToVid` |
| `hwbcm_l3_is_ecmp` (`hwbcm_l3.c`) | Linear scan of `s_ecmp_ids[]` on every route add/del | O(N_ecmp) | O(log N_ecmp) sorted array binary search (O(1) when count == 0) |

- **On-box test**:
  ```bash
  tools/scale/rs4.sh nbr SET 0 10000 && tools/scale/rs4.sh nbr DEL 0 10000
  tools/scale/rs3.sh SET 0 10000 && tools/scale/rs3.sh DEL 0 10000
  ```
  - **Before**: Every neighbor/NH/NHG/FDB-learn event scaled linearly with table size; `hwVxlanRetryPending` ran on every L3 op.
  - **After**: Flat per-op CPU time at 10k neighbors and 131k routes; FDB learn/age touches only matching `(vlan, mac)` neighbors.

### 3. `hwbcm` per-object INFO logs
- **Status**: fixed
- **Commit**: `3187bc4`
- **Summary**: Added `hwbcm_log_enabled()`; downgraded per-object success logs in `hwbcm_l3.c` (also gating `hwbcm_l3_addr_str`/`hwbcm_l3_mac_str` `snprintf` formatting), `hwbcm_l2.c`, `hwbcm_vxlan.c`, `hwbcm_copp.c`, and `hwbcm_mirror.c` from `INFO` to `DEBUG`. Init, warm-restore, dump, `ERR`, and `WARN` logs unchanged.
- **On-box test**:
  ```bash
  tools/scale/rs3.sh SET 0 10000 && tools/scale/rs3.sh DEL 0 10000
  sudo grep -c "begin to drop messages\|l3: route vrf=" /var/log/syslog
  ```
  - **Before**: 1 INFO line per route/host/egress add/del to syslog + supervisord stdout; `rsyslog` rate-limited and dropped messages.
  - **After**: 0 per-object `hwbcm` INFO lines at default log level; no `rsyslog` rate-limit drops; higher sustained route/neighbor rate.

### 4. Slow delete rates (IPv4 routes 800/s vs 1,100/s add; neighbors 180/s)
- **Status**: fixed
- **Commit**: `a1b3fbf`
- **Summary**: Found four causes in our layer (outside the SDK):
  1. `m_routeEgr`, `m_neighborEgr`, `m_nhToEgr` were `std::map` (O(log N) 75-byte JSON string tree walks at 131k routes) -> switched to `std::unordered_map` (O(1)).
  2. `defaultVirtualRouterId()` did 5 map lookups + string serialization on every route/neighbor add & delete -> cached in `m_defaultVrId`.
  3. Per-neighbor and per-next-hop `SWSS_LOG_NOTICE` (enabled by default in `syncd`, ~20k syslog writes per 10k neighbor add/del) -> downgraded to `SWSS_LOG_INFO` (matching routes).
  4. `hwCoppOnRoute()` did `getStoredAttr(ROUTE_ENTRY, ...)` on every route add -> fast-path early return when `m_coppLocalEgress >= 0 && it->second == m_coppLocalEgress`.
- **On-box test**:
  ```bash
  tools/scale/rs3.sh SET 0 130000 && tools/scale/rs3.sh DEL 0 130000
  tools/scale/rs4.sh v6 SET 0 10000 && tools/scale/rs4.sh v6 DEL 0 10000
  tools/scale/rs4.sh nbr SET 0 10000 && tools/scale/rs4.sh nbr DEL 0 10000
  ```
  - **Before**: IPv4 route del 131k = 800/s (add 1,100/s); IPv6 route del = 620/s; neighbor del = 180/s (add 30/s).
  - **After**: IPv4 route add/del expected ~1,200–1,600/s (del ~= add); IPv6 route del expected ~800–1,100/s; neighbor add/del expected ~500–900/s.

---

## B. Remaining High Audit Findings

### 5. `domain5 HIGH 1`: Dynamic FDB `m_fdb_info_set` lost across warm boot
- **Status**: fixed
- **Commit**: `1b87bcb`
- **Summary**: Added `hwWarmRebuildFdbInfoFromHash()` invoked from `warm_boot_initialize_objects()` right after `hwWarmRestoreSwitchOids()`. Scans `m_objectHash[SAI_OBJECT_TYPE_FDB_ENTRY]` and repopulates `m_fdb_info_set` for all `SAI_FDB_ENTRY_TYPE_DYNAMIC` entries (zero HW writes, no `.hwstate` changes).
- **On-box test**:
  ```bash
  # Learn a dynamic MAC on Ethernet24 (Vlan100), warm-restart syncd, then age or move the MAC
  show mac
  sudo config warm_restart enable swss && sudo systemctl restart syncd
  # Wait for aging timeout or move MAC to another port and re-check:
  show mac
  ```
  - **Before**: After warm boot, pre-existing dynamic MACs had no `m_fdb_info_set` entry, breaking aging cleanup, neighbor punt-on-age, and MAC move handling.
  - **After**: Pre-existing dynamic MACs age out cleanly and emit `SAI_FDB_EVENT_AGED` / `SAI_FDB_EVENT_MOVE` after warm boot.

### 6. `domain5 HIGH 2`: `_soc_trident2_misc_init` warm-boot register overwrites
- **Status**: fixed
- **Commit**: `04096f0` (patched `sdk-6.5.27-wb/src/soc/esw/trident2.c`, rebuilt WB SDK, updated `deploy/sdk-wb-patches/trident2.c.patch` and `README.md`)
- **Code verification of `_soc_trident2_misc_init` writes on warm boot** (explaining why 1771/1771 kexec warm-reboot pings were loss-free while specific live registers were still silently clobbered):

| Call / Register in `_soc_trident2_misc_init` | Value vs Cold/Runtime | SW State Init? | Action Taken |
|---|---|---|---|
| `_soc_trident2_port_mapping_init` | Already returns early via `_soc_td2p_log_to_ifp_mapping_recover` on `SOC_WARM_BOOT` | Yes (recovered) | Kept |
| `_soc_trident2_tdm_init` | Identical deterministic TDM calendar from `config.bcm` (why dataplane stayed loss-free) | Yes (`td2_tdm`) | Kept |
| `_soc_trident2_hash_init` | Identical deterministic tables (fixed seed `16777213`) | Yes (`ROBUSTHASH`) | Kept |
| `_soc_trident2_port_init` (`trident2.c:13280`) | **Differs**: rewrites `ASF_PORT_CFGr` to `port_speed_max` (clobbers live 1G ports back to 10G) & resets 100G egress buf | No | Guarded `!SOC_WARM_BOOT(unit)` |
| `ING_DEST_PORT_ENABLEm`, `MODPORT_MAP_SUBPORTm`, `MY_MODID_SET_2_64r` (`13483-13519`) | Pure HW rewrite | No | Guarded `!SOC_WARM_BOOT(unit)` |
| `EGR_VLAN_CONTROL_1m/r` (`13576-13601`) | **Differs**: zeroes live `REMARK_OUTER_DSCPf` / `VT_ENABLEf` and forces `REMARK_OUTER_DOT1Pf=1` | No | Guarded `!SOC_WARM_BOOT(unit)` |
| `RTAG7_FLOW_BASED_HASHm` + `RTAG7_HASH_SELr` (`13658-13698`) | **Differs**: overwrites `USE_FLOW_SEL_ECMPf=0` (set by cold `hash_init()` in `hwbcm_core.c`) back to `1`, breaking port-based ECMP hashing | No | Guarded `!SOC_WARM_BOOT(unit)` |
| Static init regs (`CPU_PBMm`, `ING_CONFIG_64r` RMW, `MISCCONFIGr`, etc.) | Identical static values | Static config | Kept |

- **On-box test**:
  ```bash
  # Before & after warm restart, inspect RTAG7_HASH_SEL and 1G port ASF/EGR_VLAN_CONTROL_1:
  deploy/bsh "g RTAG7_HASH_SEL; g EGR_VLAN_CONTROL_1.xe0"
  sudo config warm_restart enable swss && sudo systemctl restart syncd
  deploy/bsh "g RTAG7_HASH_SEL; g EGR_VLAN_CONTROL_1.xe0"
  ```
  - **Before**: `RTAG7_HASH_SEL.USE_FLOW_SEL_ECMP` flipped `0 -> 1` and `EGR_VLAN_CONTROL_1` / 1G `ASF_PORT_CFG` reset on every warm restart.
  - **After**: `RTAG7_HASH_SEL`, `EGR_VLAN_CONTROL_1`, and `ASF_PORT_CFG` remain unchanged across warm restart; warm-reboot ping remains loss-free.

### 7. `domain5 HIGH 4`: `hwColdFallbackReplay` ordering & silent failures
- **Status**: fixed (two audit items verified not a bug)
- **Commit**: `e788b22`
- **Verification**:
  - *Not a bug*: Mirror (Step 12) already ran before ACL (Step 13); CoPP trap flow counters have no separate HW counter table (`hwCoppCounterStats` reads `m_coppTrapHandle` replayed in Step 10).
  - *Fixed*: Replayed `SAI_PORT_ATTR_AUTO_NEG_MODE` even when stored without `SAI_PORT_ATTR_SPEED`; replaced all 9 `(void)`-cast replay calls (`hwCreateBridge`, `hwSetBridgePort`, `hwCreateTunnelMap`, `hwCreateTunnelTermEntry`, `hwVxlanRetryPending`, `hwCreatePolicer`, `hwCreateHostifTrapGroup`, `hwCreateAclTableGroup`, `hwCreateAclCounter`) with checked error accounting into `errors`; Step 15 now refuses to save a partial `.scache`/`.hwstate` snapshot when `errors > 0` (and unlinks stale read-path sidecars so the next restart won't warm-attach to stale state) while logging `SWSS_LOG_ERROR` and letting `APPLY_VIEW` reconcile.
- **On-box test**:
  ```bash
  # Trigger cold fallback by deleting /var/warmboot/hwbcm.hwstate before warm syncd restart
  sudo rm -f /var/warmboot/hwbcm.hwstate
  sudo systemctl restart syncd
  sudo grep -E "cold fallback|hwColdFallbackReplay" /var/log/syslog | tail -n 15
  ```
  - **Before**: 9 replay step errors were silently cast to `(void)`, `AUTO_NEG_MODE`-only ports were skipped, and partial snapshots were still saved.
  - **After**: Every replay step reports errors; `AUTO_NEG_MODE` replays; any replay error skips saving a corrupt snapshot and logs clearly.

### 8. `domain2 HIGH 6, 7, 8`: VXLAN underlay re-evaluation, VRF-aware lookup, VXLAN over LAG
- **Status**: fixed
- **Commit**: `a86939c`
- **Summary**:
  - **HIGH 6**: `vxlanCheckVtepUnderlay` now tears down the network VP (`vxlanTearDownVtepVp`, emitting `OPER_DOWN`) when underlay resolves `< 0`; wired `hwRemoveNeighbor`, `hwRemoveRoute`, `hwCreateNextHopGroupMember`, and `hwRemoveNextHopGroupMember` (via `hwVxlanRetryPendingForEcmp`) to re-evaluate affected VTEPs.
  - **HIGH 7**: Tracked `m_vtepUnderlayVrf` / per-VTEP `underlayVrf` from `SAI_TUNNEL_ATTR_UNDERLAY_INTERFACE` -> RIF `VIRTUAL_ROUTER_ID` -> `m_vrToVrf` (rebuilt on warm restore, no `.hwstate` change); `vxlanResolveUnderlayEgress(vrf, ip4)` filters strictly by VRF.
  - **HIGH 8**: Added `hwbcm_vxlan_trunk_members_prepare()` for network VPs over L3 trunks (`BCM_L3_TGID`) and access VPs over trunks, plus `hwbcm_vxlan_trunk_member_sync()` called from `syncLagMemberPort(..., true)` when new LAG members join later.
- **On-box test**:
  ```bash
  # 1) Bring up a VXLAN VTEP, withdraw its underlay route/neighbor -> verify VP torn down & tunnel OPER_DOWN:
  deploy/bsh "vxlan port show"
  # 2) Add a matching route in a non-default VRF -> verify default-VRF VTEP does not resolve via non-default VRF
  # 3) Bind underlay RIF or access VLAN to PortChannel0001 and add a late member -> verify VXLAN enable/VT on member:
  deploy/bsh "vxlan port show"
  ```
  - **Before**: Withdrawn underlay left stale network VP up; underlay lookup matched routes/neighbors in any VRF; VXLAN over LAG failed or dropped traffic on member ports.
  - **After**: Underlay withdrawal tears down VP + signals `OPER_DOWN`; lookup is VRF-scoped; underlay and access LAG members are prepared on create and on late member join.

### 9. `domain3 HIGH 1, 2, 6, 8`: Hostif trap removal/FORWARD, shared buckets, CoPP `g_have_stats`, error propagation
- **Status**: fixed
- **Commits**: `baca7b0`, `e3220e5` (`_Out_` SAL fix)
- **Summary**:
  - **HIGH 1**: Added `HWBCM_TRAP_ACTION_FORWARD` (3) to `hwbcm_trap_set` (deletes L2 cache entry or clears `ToCpu`/`Drop` controls) and `hwReconcileTrapReason()` invoked on `PACKET_ACTION=FORWARD` and `hwCoppOnTrapRemove` (preserving shared controls only while another active sibling trap needs them).
  - **HIGH 2**: Split `HWBCM_TRAP_ARP` into `HWBCM_TRAP_ARP_REQ`, `HWBCM_TRAP_ARP_RESP`, `HWBCM_TRAP_IPV6_ND` (with `NdPktDrop`); split `HWBCM_TRAP_PVRST` (`01:00:0c:cc:cc:cd`) from `HWBCM_TRAP_STP`; split `HWBCM_TRAP_L3_MTU_ERR` (`bcmSwitchL3MtuFailToCpu`) from `HWBCM_TRAP_NEIGH_MISS`. Appended slots 11..15 so `.hwstate` loads `parity-fix-1` files cleanly.
  - **HIGH 6**: In `hwbcm_copp.c:entry_build`, only clear `g_have_stats = 0` on `BCM_E_UNAVAIL`, never on transient/TCAM-full `bcm_field_entry_install` failures.
  - **HIGH 8**: Propagated real HW errors from `hwbcm_trap_set`, `coppBcmPolicer -> coppApplyTrap` (refusing to install an unpoliced trap if policer creation fails), and unsupported meter/mode in `hwCreatePolicer`.
- **On-box test**:
  ```bash
  # 1) Check split ARP/ND/L3_MTU controls & L2 cache entries:
  deploy/bsh "l2 cache show"
  # 2) Change a trap (e.g. LLDP or ARP_RESPONSE) to FORWARD or delete it -> verify L2 cache / switch control clears:
  deploy/bsh "l2 cache show"
  ```
  - **Before**: Removing a trap or setting `FORWARD` left CPU steering enabled unpoliced; `ARP_RESPONSE=TRAP` set `ArpRequestDrop=1` breaking L2 ARP; `PVRST` clobbered `STP`; `L3_MTU_ERROR` never set `L3MtuFailToCpu`; one failed install disabled CoPP stats forever; failed policers installed unpoliced traps.
  - **After**: Removed/forwarded traps clear HW steering; ARP req/resp/ND/STP/PVRST/L3_MTU are independent; CoPP stats survive transient errors; policer/trap HW errors propagate cleanly.

### 10. `domain4 R1, R2`: Buffer pool watermark clear-on-read & egress UC-only pool watermarks
- **Status**: fixed
- **Commit**: `f3bba83`
- **Summary**:
  - **R1**: `hwGetObjectStats(SAI_OBJECT_TYPE_BUFFER_POOL)` now accepts `SAI_BUFFER_POOL_STAT_XOFF_ROOM_WATERMARK_{BYTES,CELLS}` as `0` (no-op clear) when paired with supported `WATERMARK_{BYTES,CELLS}` so `BufferOrch`'s `clear_buffer_pool_stats(pool, {WATERMARK_BYTES, XOFF_ROOM_WATERMARK_BYTES})` probe returns `SAI_STATUS_SUCCESS` and enables `STATS_MODE_READ_AND_CLEAR` (standalone probes and `queryStatsCapability` still report `XOFF_ROOM_*` unsupported on TD2+).
  - **R2**: Replaced `egr_pool_rep()` (which polled MC-only `bcmCosqControlEgressPool` up to 432 times/poll and always failed with 0 for UC-only SP1..SP3) with direct O(1) read/clear of `MMU_THDM_DB_POOL_MCUC_BST_STATr[sp]` gated by `BST_TRACK_EN_THDO`.
- **On-box test**:
  ```bash
  # 1) Verify BufferOrch enabled READ_AND_CLEAR (no "Clear watermark failed" in syslog):
  sudo grep "Clear watermark failed" /var/log/syslog || echo "OK: clear enabled"
  redis-cli -n 4 HGETALL "FLEX_COUNTER_GROUP_TABLE|BUFFER_POOL_WATERMARK"
  # 2) Generate traffic through egress UC pool (SP1..SP3), read then clear watermarks:
  show watermark telemetry interval
  show queue watermark unicast
  sonic-clear watermark
  ```
  - **Before**: `BufferOrch` logged `Clear watermark failed ... NOT_SUPPORTED` for every pool and forced `STATS_MODE_READ`; UC-only egress pools (SP1..SP3) burned 432 SDK reads/poll and always returned 0.
  - **After**: `BUFFER_POOL_WATERMARK` runs in `STATS_MODE_READ_AND_CLEAR` and clears on poll/`sonic-clear watermark`; all egress pools SP0..SP3 read real non-zero watermarks in O(1).
