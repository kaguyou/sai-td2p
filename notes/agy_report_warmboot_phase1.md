# Warm Boot Phase 1 + Phase 2 Report (`sai-td2p-wb`, branch `warmboot`)

**Date:** 2026-11-24
**Branch:** `warmboot` (isolated worktree `/home/kaguyou/homelab/sai-td2p-wb` + build copy `/home/kaguyou/homelab/sonic-sairedis-wb`)
**Design Reference:** `notes/design_warmboot.md`
**Phase 0 Reference:** `notes/agy_report_warmboot_phase0.md`

---

## 1. Summary & Build Artifacts

Both **Phase 1** (`hwbcm` warm init / warm shutdown / Level 2 file-backed scache / KNET recovery / 1G SGMII preservation + standalone `hwtest_wb`) and **Phase 2** (`vslib` warm-boot file plumbing, `RESTART_WARM` / `PRE_SHUTDOWN` / `UNINIT_DATA_PLANE_ON_REMOVAL` lifecycle, versioned CRC32-protected `.hwstate` persistence for Core + LAG + L3, KNET Case A/B recovery, `BRIDGE_PORT` FDB learning freeze/unfreeze, and warm-safe QoS/Stats init) are implemented, built cleanly with **zero compiler warnings**, and verified against all symbol/ABI/ldd checks (`ALL CHECKS PASSED`).

| Artifact | Path | Size | SHA-256 |
|---|---|---|---|
| **Warm-boot `libsaivs.so.0.0.0`** | `/home/kaguyou/homelab/sai-td2p-wb/deploy/build/libsaivs.so.0.0.0` | `149,072,464` bytes (143 MiB) | `b4ca0027a4e0a50c258c38411fd77f878aa4cd70c915413171a139c7347dc749` |
| **Standalone `hwtest_wb`** | `/home/kaguyou/homelab/sai-td2p-wb/hwtest/hwtest_wb` | `98,099,160` bytes (94 MiB) | `57a6f6bd1a96267c23d014265284b8b50fcba3098d718d53105deee3bce8e34b` |
| **Merged WB SDK archive** | `/home/kaguyou/homelab/sai-td2p-wb/deploy/build/libopenbcm_td2p_wb.a` | `151,304,566` bytes (1,962 members) | — |

### `deploy/build_wb.sh` Verification Highlights
- **Status:** `ALL CHECKS PASSED`
- **Undefined symbols:** `0` undefined `bcm_*/soc_*/sal_*/bde_*/hwbcm_*` symbols
- **ABI tags:** `GLIBC max 2.34 <= 2.36`, `GLIBCXX max 3.4.30 <= 3.4.30`, `CXXABI max 1.3.9 <= 1.3.13` (`hwtest_wb`: `GLIBC max 2.34`)
- **Exported/defined symbols:** `67,055` dynamic symbols (`32,524` `bcm_*`, `177` `hwbcm_*`, `sai_api_*`, `bde`, `bde_create`)
- **Verified warm-boot SDK & `hwbcm` symbols inside `libsaivs.so.0.0.0`:**
  - `soc_scache_init`, `soc_scache_recover`, `soc_scache_commit`, `soc_stable_set`, `soc_stable_size_set`, `soc_switch_stable_register`, `soc_scache_hsbuf_crc32_append` (`-DSCACHE_CRC_CHECK`), `soc_knet_wb_cleanup`, `soc_shutdown`, `_bcm_shutdown`
  - **45** `_bcm_esw_*_sync` warm-boot sync functions
  - `hwbcm_init_ex`, `hwbcm_warm_sync`, `hwbcm_warm_deinit`, `hwbcm_knet_warm_recover`

---

## 2. What Is Implemented

### Build Infrastructure
- **`deploy/build_wb.sh`**: Builds `/w/sonic-sairedis-wb/vslib` against `/w/openbcm-src/sdk-6.5.27-wb`, `/w/as5812-bdeshim/sdk_link_list_wb.txt`, `sdk_cflags_wb.txt`, and `sdk_incs_wb.txt`, writing outputs to `sai-td2p-wb/deploy/build/`. Tracks a `.wb_cflags_stamp` so stale non-WB `hwbcm/*.o` objects are always recompiled with `-DBCM_WARM_BOOT_SUPPORT -DBCM_WARM_BOOT_SUPPORT_SW_DUMP -DSCACHE_CRC_CHECK`, and enforces assertions on all 14 warm-boot SDK/`hwbcm` symbols plus `_bcm_esw_*_sync`.
- **`sdk_cflags_wb.txt` & `sdk_incs_wb.txt`**: Isolated WB SDK compile flags and include paths pointing exclusively at `/w/openbcm-src/sdk-6.5.27-wb/...`.
- **`hwtest/hwtest_wb.c` & `hwtest/build_wb.sh`**: Standalone C warm-reload test binary (no SONiC/SAI required) to validate Phase 1 directly on the switch.

### Phase 1 — `hwbcm` Warm Lifecycle, File-Backed Scache, KNET Recovery & 1G SGMII State
- **`sairedis/vslib/hwbcm/hwbcm.h`, `hwbcm_core.c`**:
  - **Buffer-backed Level 2 scache** (`hwbcm_scache_read`/`hwbcm_scache_write` registered via `soc_switch_stable_register`, `soc_stable_set(0, BCM_SWITCH_STABLE_APPLICATION, 0)`, `soc_stable_size_set` defaulting to 32 MiB / respecting `stable_size` clamped to `[16 MiB, 128 MiB]`):
    - On cold boot: arms a zeroed in-memory buffer (`calloc`, lazy pages, **zero disk I/O during cold boot**) before `init soc` so a subsequent warm shutdown can sync cleanly.
    - On warm boot: loads `<scache_file>` read-only into the buffer before `soc_init(0)`, enforcing EOF bounds checking so truncated files fail cleanly, and verifies `soc_stable_get() == BCM_SWITCH_STABLE_APPLICATION` after `soc_init(0)` to catch SDK CRC32/corruption downgrades (`-DSCACHE_CRC_CHECK`).
    - On `hwbcm_warm_sync()`: runs `bcm_switch_control_set(0, bcmSwitchControlSync, 1)`, writes only the used/committed extent (plus null-descriptor padding) to `<scache_file>.tmp` → `fsync` → atomic `rename` → parent directory `fsync`.
  - **`hwbcm_init_ex(config_bcm, warm_boot, scache_file)`** (`hwbcm_init()` delegates with `warm_boot=0, scache_file=NULL`):
    - **Cold boot (`warm_boot=0`)**: Identical behavior to production (`init soc`/`misc`/`mmu`/`bcm`, ports admin-down, VLAN 1 flush, STP forward, CPU port in VLAN 1, `knet_stale_cleanup`, `bcm_rx_start`, linkscan, port map, RIOT `EGR_VLAN[0]`, `hash_init`).
    - **Warm boot (`warm_boot=1`)**: Sets `warmboot_knet_shutdown_mode=1` and `knet_filter_persist=1` via `sal_config_set`, loads scache, enters `SOC_WARM_BOOT_START(0)`, runs `soc_init(0)` (no reset) → scache-recovery verification → `soc_misc_init(0)` → `soc_mmu_init(0)` → `bcm_attach` (if needed) → `bcm_init(0)` → `SOC_WARM_BOOT_DONE(0)`. **Skips** port disable, VLAN 1 flush, STP write, `knet_stale_cleanup`, `hash_init`, and `EGR_VLAN[0]` write. Calls `bcm_rx_start(0, NULL)` if inactive, builds port map, pre-populates `g_sgmii_applied[]` before enabling linkscan, and preserves per-port linkscan mode.
    - **In-process fallback safety**: Tracks `g_sal_sysconf_inited` so if a warm-boot attempt fails (e.g., corrupt scache), `SwitchBCM56860HW` can cleanly fall back to cold `hwbcm_init_ex(..., 0, ...)` in the same process without re-initializing SAL/BDE.
  - **1G SGMII warm-boot preservation (`hwbcm_sgmii_warm_probe`)**: Before enabling linkscan on warm boot, scans every front port; any port already `UP` at `<= 1000 Mbps` with `BCM_PORT_IF_SGMII` gets `g_sgmii_applied[p] = 1` (and `g_sgmii_applied_at[p]` aged past `HWBCM_SGMII_RATE_LIMIT_SEC`) so linkscan does not bounce 1G copper links.
  - **`hwbcm_warm_deinit()`**: Stops shell socket, unregisters linkscan and L2 callbacks (without touching ports or KNET netifs/filters), enforces `warmboot_knet_shutdown_mode=1` + `knet_filter_persist=1`, runs `bcm_tx_cleanup(0)` → `_bcm_shutdown(0)` → `soc_shutdown(0)`, and frees the scache buffer.
  - **`hwbcm_knet_warm_recover()`**: Traverses live kernel KNET netifs and filters (`bcm_knet_netif_traverse` + `bcm_knet_filter_traverse`), repopulates `g_knet[]` (including prio 48 VFI, prio 49 PVID strip + `pvid`, and prio 50/100 catch-all filters) with **zero KNET mutations**, and returns netif metadata to C++.
  - **Core/L2/L3/Stats warm helpers**: `hwbcm_core_wb_get/restore`, `hwbcm_port_desired_speed_restore`, `hwbcm_l2_warm_reset`, `hwbcm_trunk_member_count_get`, `hwbcm_l3_warm_restore/reset` (probes `bcmSwitchL3EgressMode`/`bcmSwitchL3IngressMode` without writing HW; restores `s_vrf_used[]` and `s_ecmp_ids[]`), and warm-safe `hwbcm_stats_init()` (reads `bcmSwitchBstEnable` instead of writing on warm boot).

### Phase 2 — `vslib` Plumbing, SAI Warm Lifecycle & Core/LAG/L3 `.hwstate` Persistence
- **`vslib` plumbing (`sonic-sairedis-wb/vslib/{Sai.cpp,SwitchConfig.h,Makefile.am}` + `sairedis/vslib.patch`)**:
  - Propagates `m_warmBootReadFile` and `m_warmBootWriteFile` onto `SwitchConfig`.
  - Confirmed `SwitchStateBase::queryAttributeCapability()` already advertises `SAI_SWITCH_ATTR_UNINIT_DATA_PLANE_ON_REMOVAL` (`set_implemented = true`).
- **`SwitchBCM56860HW` warm lifecycle (`SwitchBCM56860HW.{h,cpp}`, `SwitchBCM56860HW_wb.inc.{h,cpp}`, `SwitchBCM56860HW_l3.cpp`, `SwitchBCM56860HW_qos.{inc.h,cpp}`)**:
  - **`hwSetSwitch()`**: Handles `SAI_SWITCH_ATTR_RESTART_WARM`, `SAI_SWITCH_ATTR_UNINIT_DATA_PLANE_ON_REMOVAL`, and `SAI_SWITCH_ATTR_PRE_SHUTDOWN` (triggers `hwWarmSync()`: `hwbcm_warm_sync()` + atomic `.hwstate` write).
  - **`~SwitchBCM56860HW()`**: When `m_restartWarm && !m_uninitDataPlaneOnRemoval`, runs `hwWarmSync()` if `PRE_SHUTDOWN` was not called, then calls `hwbcm_warm_deinit()`. Otherwise runs unchanged cold `hwbcm_deinit()`.
  - **Versioned `.hwstate` binary TLV persistence (`<warm_boot_file>.hwstate`)**:
    - 32-byte header: magic `"HWST"`, version `1`, payload length, IEEE CRC32 over payload, CRC32 of `config.bcm` (rejects warm boot if `config.bcm` changed), timestamp. Written atomically via `.tmp` + `fsync` + `rename` + directory `fsync`.
    - TLV sections implemented:
      1. `TAG_CORE`: `g_switch_mac`, `g_switch_mac_valid`, `g_ip2me_station_id`, `g_ip2me_vfi_station_id`, `g_ip2me_wanted`, `g_trap_l2cache_idx[]`
      2. `TAG_LAG_TRUNKS`: `m_lagToTrunk` (cross-checked via `hwbcm_trunk_member_count_get`)
      3. `TAG_L3_GLOBALS`: `g_cpuEgr`, `g_dropEgr`
      4. `TAG_L3_VR`: `m_vrToVrf` (restores `s_vrf_used[]`)
      5. `TAG_L3_RIF`: `m_rifToIntf`, `m_rifToVid`
      6. `TAG_L3_NH`: `m_nhToEgr`
      7. `TAG_L3_NHG`: `m_nhgToEcmp` (restores `s_ecmp_ids[]`)
      8. `TAG_L3_NEIGH`: `m_neighborEgr`
      9. `TAG_L3_ROUTE`: `m_routeEgr`
    - Rebuilds `m_lagMembers`, `m_lagMemberInfo`, `m_nhgMemberToNh`, `m_nhgMemberToNhg` from `m_objectHash` with zero HW writes.
  - **`warm_boot_initialize_objects()`**:
    - Validates `.scache` existence and `.hwstate` header/CRC32/`config.bcm` CRC before warm attach.
    - Runs `hwInitialize(true)` → `buildPortMaps(false)` (zero HW writes; restores `g_desired_speed` and refreshes live `OPER_STATUS` & `OPER_SPEED` into `m_objectHash`) → `hwWarmRecoverKnet()` (Case A: matches live KNET netifs by name with 0 mutations; Case B kexec: recreates netifs from `m_objectHash` if live table is empty) → `hwStateLoad()` → `qosBuildHwMap(true)` (SW maps only, zero HW writes).
    - On missing or corrupt `.scache`/`.hwstate`, logs an explicit error and falls back cleanly to cold SDK init.
  - **`SAI_OBJECT_TYPE_BRIDGE_PORT` learning control (`hwSetBridgePort`)**: Wired `SAI_BRIDGE_PORT_ATTR_FDB_LEARNING_MODE` in `SwitchBCM56860HW::set()` to `hwbcm_port_learn_set()` for both port and LAG bridge ports so `orchagent`'s pre-shutdown FDB learning freeze takes effect in hardware.

### Cold-Boot Behavior Guarantee
When no warm-boot request is active (`SAI_KEY_BOOT_TYPE=0` and `SAI_SWITCH_ATTR_RESTART_WARM=false`):
- `initialize_default_objects()` runs the exact same cold init sequence (plus an untouched lazy virtual scache buffer in RAM, zero disk writes).
- `~SwitchBCM56860HW()` runs the exact same cold `hwbcm_deinit()`.
- `buildPortMaps(true)` and `qosBuildHwMap(false)` are unchanged.

---

## 3. What Is Left (Phase 3 & Phase 4)

1. **Phase 3 — Remaining Subsystem `.hwstate` Persistence & Recovery** (new TLV tags `>= 10`; existing TLV reader already skips unknown tags cleanly):
   - **VXLAN / EVPN / RIOT**: `hwbcm_vxlan_warm_restore()` (set `s_vxlan_inited=1` without calling `bcm_vxlan_init(0)`, restore `s_vp[]`, `s_acc_intf[]`, `s_vpn_l3[]`, `s_udp_port`) + persist `m_vtepByIp`, `m_vniToVpn`, `m_vpnAccessVps`, `m_tunnelInit/Term`, `m_vxlanStationId`, and rebuild map/tunnel/MAC bookkeeping from `m_objectHash`.
   - **ACL**: `hwbcm_acl_group_restore()` + persist `m_aclTables`, `m_aclEntries`, `m_aclCounters` offsets, `m_aclRanges`; rebuild groups/bindings from `m_objectHash`.
   - **CoPP**: `hwbcm_copp_warm_dump/restore()` for `g_group*`, `g_local_egr`, `g_slot[64]`, `g_pol[32]` + persist `m_coppTrapHandle`, `m_coppPolicer`, `m_coppLocalEgress`.
   - **QoS**: Persist `m_qosHwMaps` + per-port map keys, `m_qosPools` + `m_qos*SpUsed`, and `hwbcm_qos.c` `s_pg_snap`/`s_q_snap` defaults (currently `qosBuildHwMap(true)` skips HW writes so ASIC QoS state stays intact across warm boot, but mutating QoS maps/buffer pools post-warm-boot requires Phase 3 handle persistence).
   - **Mirror & Stats**: Persist `m_mirrorSessions` (`destGport`) + rebuild `m_aclMirrorDest` / port mirror sets; persist non-zero `g_qbase`/`g_xbase` baselines in `hwbcm_stats.c`.
2. **Phase 4 — Host `warm-reboot` (kexec) Hardening**:
   - Automate `/dev/linux-user-bde` (c 126 0) device node creation across host kexec (udev rule / systemd drop-in on `opennsl-modules`).
   - Full `config warm_restart enable system` + `sudo warm-reboot` end-to-end validation with VXLAN/ACL/CoPP/QoS/Mirror active.
3. **Production Integration (when ready to merge from `warmboot` branch)**:
   - Update `deploy/build.sh` (or merge `build_wb.sh`), copy `sdk_cflags_wb.txt`/`sdk_incs_wb.txt`, apply updated `sairedis/vslib.patch` to main `sonic-sairedis`, and ensure `td2-as5812t-72x10G.config.bcm` has clean `stable_size=0x2000000` (optional — `hwbcm` already sets `warmboot_knet_shutdown_mode=1` and `knet_filter_persist=1` programmatically on warm shutdown/start and only writes used scache bytes).

---

## 4. Exact Hardware Test Steps for the Lead (Edgecore AS5812-54T)

> **Note:** None of these commands were run by agy; the switch and original `sai-td2p` / `sonic-sairedis` / `sdk-6.5.27` trees were never touched.

### Test 1 — Phase 1 Standalone SDK Warm Reload (`hwtest_wb`, Zero SONiC Involvement)
Validates `sdk-6.5.27-wb`, Level 2 file scache + CRC32, `warmboot_knet_shutdown_mode=1` against SONiC's 6.5.32 `linux-bcm-knet.ko` (`KCOM_M_WB_CLEANUP`), 0 link flaps (including 1G copper SGMII), and KNET netif/filter survival.

1. **Copy `hwtest_wb` to the switch:**
   ```bash
   scp /home/kaguyou/homelab/sai-td2p-wb/hwtest/hwtest_wb admin@<switch>:/tmp/hwtest_wb
   ```
2. **On the switch, stop `swss` and `syncd` cleanly so only `hwtest_wb` owns BDE:**
   ```bash
   sudo systemctl stop swss syncd
   sudo rm -f /tmp/hwtest_wb.scache /tmp/hwtest_wb.scache.tmp
   ```
3. **Cold init + program ports 2, 3, VLAN 100, KNET `Ethernet4`, then warm-sync & warm-shutdown:**
   ```bash
   sudo /tmp/hwtest_wb /usr/share/sonic/device/x86_64-accton_as5812_54t-r0/Accton-AS5812-54T/td2-as5812t-72x10G.config.bcm cold /tmp/hwtest_wb.scache 10
   ```
   - Verify:
     - `ls -lh /tmp/hwtest_wb.scache` shows a non-empty scache file (typically 4–12 MiB).
     - `ip -br link show Ethernet4` shows `Ethernet4` STILL EXISTS after `hwtest_wb` exited!
     - `dmesg | tail -30` shows **zero** BDE/KNET oops or errors on `KCOM_M_WB_CLEANUP`.
     - Start continuous ping / traffic through ports 2 & 3 (and record peer NIC `ethtool -S` / `cat /sys/class/net/<if>/carrier_changes`).
4. **Warm attach + KNET recovery + verify 0 link flaps and 0 packet drops:**
   ```bash
   sudo /tmp/hwtest_wb /usr/share/sonic/device/x86_64-accton_as5812_54t-r0/Accton-AS5812-54T/td2-as5812t-72x10G.config.bcm warm /tmp/hwtest_wb.scache 15
   ```
   - **Pass criteria:**
     - `hwbcm_init_ex: WARM init done ... in <N> ms` completes in **< 5 s** (vs ~2–3 min cold).
     - `wb knet: recovered 1 netif(s) and 3 filter(s) with zero mutations` (or 2 filters without RIOT) and `Ethernet4` has the **exact same `netif_id`**.
     - `wb sgmii:` marks any active 1G port applied; `total link_cb events observed: 0`.
     - Peer NIC `carrier_changes` delta == **0**; **0 packets dropped** during exit + warm reload.
     - Second warm sync + warm exit succeeds with no scache size growth.
5. **Cleanup (optional cold exit before returning to SONiC):**
   ```bash
   sudo /tmp/hwtest_wb /usr/share/sonic/device/x86_64-accton_as5812_54t-r0/Accton-AS5812-54T/td2-as5812t-72x10G.config.bcm warm /tmp/hwtest_wb.scache 2 --cold-exit
   ```

### Test 2 — Phase 2 `syncd` Warm Restart (Core + L2 + LAG + KNET + L3)
1. **Back up the current `libsaivs.so.0.0.0` in the `syncd` container and install the WB build:**
   ```bash
   scp /home/kaguyou/homelab/sai-td2p-wb/deploy/build/libsaivs.so.0.0.0 admin@<switch>:/tmp/libsaivs.so.0.0.0.wb
   # On switch:
   sudo docker cp syncd:/usr/lib/x86_64-linux-gnu/libsaivs.so.0.0.0 /tmp/libsaivs.so.0.0.0.bak
   sudo docker cp /tmp/libsaivs.so.0.0.0.wb syncd:/usr/lib/x86_64-linux-gnu/libsaivs.so.0.0.0
   sudo docker exec syncd sha256sum /usr/lib/x86_64-linux-gnu/libsaivs.so.0.0.0
   # Expect: b4ca0027a4e0a50c258c38411fd77f878aa4cd70c915413171a139c7347dc749
   ```
2. **Cold boot sanity check first** (with warm restart disabled):
   ```bash
   sudo config warm_restart disable
   sudo systemctl restart swss
   ```
   - Verify normal cold boot works identically to production (ports up, L2/LAG/L3 forwarding, ping SVI/routed port, `show interfaces status`).
3. **Enable `syncd` warm restart and restart `syncd` under continuous L2 + L3 traffic:**
   ```bash
   sudo config warm_restart enable syncd
   sudo systemctl restart syncd
   ```
   - **Pass criteria:**
     - `redis-cli -n 6 HGETALL "WARM_RESTART_TABLE|warm-shutdown"` shows `warm-shutdown-succeeded`.
     - `/host/warmboot/` contains all 3 files: `sai-warmboot.bin`, `sai-warmboot.bin.scache`, `sai-warmboot.bin.hwstate`.
     - `syncd` syslog shows:
       - `wb: warm sync succeeded (scache=... bytes, hwstate=...)`
       - `hwbcm_warm_deinit: done in ... ms (ASIC & KNET preserved)` (< 5 s, well under `syncd.sh`'s 20 s timer)
       - `hwbcm_init_ex: WARM init done ... in ... ms`
       - `wb knet: Case A (syncd restart) matched ... hostif(s) to live KNET netifs (missing=0, ...)`
       - `wb hwstate: restored lags=... vrs=... rifs=... nhs=... nhgs=... neighs=... routes=...`
       - `wb: true warm boot completed (zero ASIC resets, zero port flaps)`
     - **0 packets dropped** on L2 and L3 data-plane streams; **0 link flaps** (including 1G copper ports).
4. **Post-warm-restart mutability checks (Phase 2 scope: Core/L2/LAG/KNET/L3):**
   - Add & remove a static L3 route; verify no `BCM_E_NOT_FOUND` on deleting a pre-warm-boot route, neighbor, or RIF.
   - Add & remove a VLAN member and a LAG member.
   - Change a port PVID and verify `hwbcm_port_pvid_set` cleanly replaces the KNET PVID filter.
   - Run a **2nd consecutive** `sudo systemctl restart syncd` and verify scache size stays stable and traffic remains hitless.

### Test 3 — Negative / Corrupt-File Cold Fallback Test
1. Corrupt 1 byte in `/host/warmboot/sai-warmboot.bin.hwstate` (or `.scache`) while `syncd` is stopped (or before `systemctl restart syncd` with warm enabled):
   ```bash
   sudo dd if=/dev/urandom of=/host/warmboot/sai-warmboot.bin.hwstate bs=1 count=1 seek=40 conv=notrunc
   sudo systemctl restart syncd
   ```
   - **Pass criteria:** Syslog reports `wb hwstate: CRC32 mismatch` (or `L2 scache recovery failed/corrupt`) and cleanly falls back to cold SDK init without crashing or restart-looping.

### Rollback
```bash
sudo config warm_restart disable
sudo docker cp /tmp/libsaivs.so.0.0.0.bak syncd:/usr/lib/x86_64-linux-gnu/libsaivs.so.0.0.0
sudo systemctl restart swss
```

---

## 5. Isolation Verification

- Untouched trees: `/home/kaguyou/homelab/sai-td2p`, `/home/kaguyou/homelab/sonic-sairedis`, `/home/kaguyou/homelab/openbcm-src/sdk-6.5.27`.
- No switch access (`ssh`/`scp`/`/dev/ttyUSB0`/`deploy/install.sh`) was performed.
- All changes are committed locally on branch `warmboot` in `/home/kaguyou/homelab/sai-td2p-wb` (never pushed).
