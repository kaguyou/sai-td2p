# Integration notes: port mirroring (SPAN / RSPAN / ERSPAN)

Owner: Mirror agent, 2026-11-24. All four code files are new. No existing file was edited.

| File | What it is |
|---|---|
| `sairedis/vslib/hwbcm/hwbcm_mirror.h` | SDK-free C API (`hwbcm_mirror_*`): `hwbcm_mirror_dest_cfg_t`, mirror destination create/replace/destroy, per-port ingress/egress attach/detach, debug dump |
| `sairedis/vslib/hwbcm/hwbcm_mirror.c` | Implementation using `bcm_mirror_destination_create/destroy/traverse`, `bcm_mirror_port_dest_add/delete/delete_all`, `BCM_MIRROR_DEST_TUNNEL_IP_GRE`, `BCM_MIRROR_DEST_TUNNEL_L2`, `BCM_MIRROR_DEST_INT_PRI_SET` |
| `sairedis/vslib/SwitchBCM56860HW_mirror.cpp` | `SAI_OBJECT_TYPE_MIRROR_SESSION` create/set/remove handlers, port mirror hook (`SAI_PORT_ATTR_INGRESS_MIRROR_SESSION` / `EGRESS_MIRROR_SESSION`), ACL hook calls (`hwAclMirrorSessionUpdated`) |
| `sairedis/vslib/SwitchBCM56860HW_mirror.inc.h` | Member declarations and state, to `#include` inside `class SwitchBCM56860HW` |

---

## 1. Wiring

### 1.1 Class header (`sairedis/vslib/SwitchBCM56860HW.h`)

Add as the last line inside `class SwitchBCM56860HW`, after `#include "SwitchBCM56860HW_qos.inc.h"`:

```cpp
#include "SwitchBCM56860HW_mirror.inc.h"
```

The `.inc.h` file opens with `public:` and closes in `protected:`. Optionally add a line to the file-split comment at the top of `SwitchBCM56860HW.h`:

```cpp
 *   SwitchBCM56860HW_mirror.cpp MIRROR_SESSION (SPAN/RSPAN/ERSPAN), port mirror
```

### 1.2 `create()` dispatch (`sairedis/vslib/SwitchBCM56860HW.cpp`, inside `switch (object_type)`)

Add after the QoS cases (before `default:`):

```cpp
        // Mirror
        case SAI_OBJECT_TYPE_MIRROR_SESSION:         hw = hwCreateMirrorSession(oid, attr_count, attr_list); break;
```

`SwitchBCM56850::create` has already stored all attributes in `m_objectHash` before the switch runs. If `hwCreateMirrorSession` fails (e.g. unsupported IPv6 ERSPAN or `SAI_STATUS_INSUFFICIENT_RESOURCES`), the existing rollback block below the switch removes the object from vslib.

### 1.3 `remove()` dispatch (`sairedis/vslib/SwitchBCM56860HW.cpp`, inside `if (m_hwReady)` `switch (object_type)`)

Add after the QoS cases (before `default:`):

```cpp
            // Mirror
            case SAI_OBJECT_TYPE_MIRROR_SESSION:         hw = hwRemoveMirrorSession(oid); break;
```

### 1.4 `set()` dispatch (`sairedis/vslib/SwitchBCM56860HW.cpp`, inside `switch (objectType)`)

Add after the QoS cases (before `default:`):

```cpp
        case SAI_OBJECT_TYPE_MIRROR_SESSION:         hw = hwSetMirrorSession(oid, attr); break;
```

`SwitchBCM56850::set` has already stored the new attribute value before `hwSetMirrorSession` runs, so `mirrorReadConfig()` sees the full updated attribute set.

### 1.5 `hwSetPort()` and `hwRemovePort()` hooks (`sairedis/vslib/SwitchBCM56860HW.cpp`)

In `hwSetPort()`, right after the `qosIsPortAttr` check (line 575, before `bcmPortOf`):

```cpp
    if (mirrorIsPortAttr(attr->id))
    {
        return mirrorHwSetPort(port_id, attr);
    }
```

This handles `SAI_PORT_ATTR_INGRESS_MIRROR_SESSION` and `SAI_PORT_ATTR_EGRESS_MIRROR_SESSION`. Because `hwCreatePort()` loops over `attr_list` calling `hwSetPort()`, mirror attributes passed at port creation time are also handled automatically.

In `hwRemovePort()`, right before `unmapPort(port_id);` (next to `qosOnPortRemoved(port_id);`):

```cpp
    mirrorOnPortRemoved(port_id);
```

### 1.6 ACL hook (`notes/integration_acl.md` 1.7)

No extra wiring is needed in `SwitchBCM56860HW_acl.cpp` or `SwitchBCM56860HW.cpp`. `SwitchBCM56860HW_mirror.cpp` calls `hwAclMirrorSessionUpdated()` directly:
- At the end of `hwCreateMirrorSession`: `hwAclMirrorSessionUpdated(session_id, destGport)`.
- During `hwSetMirrorSession`: `hwAclMirrorSessionUpdated(session_id, -1)` before updating the destination, and `hwAclMirrorSessionUpdated(session_id, newDest)` after.
- In `hwRemoveMirrorSession`: `hwAclMirrorSessionUpdated(session_id, -1)` **before** calling `hwbcm_mirror_dest_destroy(destGport)`, so all IFP/EFP rules release their MTP references first.

### 1.7 `Makefile.am` (`sonic-sairedis/vslib/Makefile.am`)

Add to `libSaiVS_a_SOURCES`:

```makefile
					  SwitchBCM56860HW_qos.cpp \
					  SwitchBCM56860HW_mirror.cpp \
```

and at the end of the `hwbcm/` list:

```makefile
					  hwbcm/hwbcm_qos.c \
					  hwbcm/hwbcm_mirror.c
```

`hwbcm_mirror.c` compiles with the existing `libSaiVS_a_CFLAGS` (`SDK_CFLAGS`, `-I$(srcdir)/hwbcm`). No new link libraries are needed (`bcm_esw_mirror_*` is already part of the linked OpenBCM static libraries).

---

## 2. Design

### 2.1 Trident2+ (BCM56864 / BCM56860_A0) hardware model (`sdk-6.5.27`)

- **Directed mirroring is always enabled** on TD2+ (`src/bcm/esw/switch.c:18858` returns `1` unconditionally for `bcmSwitchDirectedMirroring` on `SOC_IS_TD_TT`; `soc_feature_directed_mirror_only` is inherited from `bcm56840_a0`). `bcm_mirror_port_dest_add` enables mirroring mode (`BCM_MIRROR_L2`) automatically (`src/bcm/esw/mirror.c:26996`).
- **4 shared MTP slots (`BCM_MIRROR_MTP_COUNT = 4`):** `IM_MTP_INDEX`, `EM_MTP_INDEX`, `EGR_IM_MTP_INDEX`, `EGR_EM_MTP_INDEX`. Calling `bcm_mirror_destination_create` only allocates a software descriptor (`dest_arr[]`). A hardware MTP slot (and `EGR_MIRROR_ENCAP_CONTROL`/`DATA_1` profile entry for RSPAN/ERSPAN) is reserved when the first port or FP rule attaches, and freed when the last user detaches.
- **Monitor gport:** `bcm_mirror_destination_create` requires `MODPORT`, `TRUNK`, or `DEVPORT` (`mirror.c:27818`). Local ports are converted with `bcm_port_gport_get(0, port, &gport)` (which yields `MODPORT(my_modid, port)`); LAGs use `BCM_GPORT_TRUNK_SET(gport, tid)`.
- **SPAN (`SAI_MIRROR_SESSION_TYPE_LOCAL`):** `flags = 0` (plus optional `BCM_MIRROR_DEST_INT_PRI_SET` when `SAI_MIRROR_SESSION_ATTR_TC` is specified, programming `EGR_IM/EM_MTP_INDEX.CHANGE_INT_PRI` / `NEW_INT_PRI`, supported on `SOC_IS_TD_TT` at `mirror.c:27770`).
- **ERSPAN (`SAI_MIRROR_SESSION_TYPE_ENHANCED_REMOTE`):** uses `BCM_MIRROR_DEST_TUNNEL_IP_GRE` (`_bcm_trident_mirror_ipv4_gre_tunnel_set`, `mirror.c:12745-12887`):
  - Encapsulates mirrored packets in `L2 [802.1Q] | IPv4 (proto 47) | 4-byte GRE (0x0000 + gre_protocol, default 0x88be) | Original Frame` — matching `SAI_ERSPAN_ENCAPSULATION_TYPE_MIRROR_L3_GRE_TUNNEL`, which is what SONiC's `MirrorOrch` always requests (`mirrororch.cpp:993`).
  - `src_addr` / `dst_addr` are in host byte order (`ntohl` in C++). `tos` carries the full TOS byte (`DSCP << 2 | ECN`).
  - **Outer VLAN TCI packing:** on TD2+, `_bcm_trident_mirror_ipv4_gre_tunnel_set` (`mirror.c:12793, 12817-12836`) checks `BCM_VLAN_VALID(BCM_VLAN_CTRL_ID(mdest.vlan_id))`, sets `ERSPAN__ADD_ERSPAN_OUTER_VLANf = 1`, and writes `(tpid << 16) | mdest.vlan_id` verbatim into `ERSPAN_HEADER_VLAN_TAG`. We therefore pack `PRI`, `CFI`, and `VID` into `mdest.vlan_id` via `BCM_VLAN_CTRL(pri, cfi, vid)`. When `VLAN_HEADER_VALID == false`, `tpid` and `vlan_id` are set to `0` so the outer Ethernet header is emitted untagged.
- **RSPAN (`SAI_MIRROR_SESSION_TYPE_REMOTE`):** uses `BCM_MIRROR_DEST_TUNNEL_L2` (`_bcm_trident_mirror_l2_tunnel_set`, `mirror.c:12679`). Note that `BCM_MIRROR_DEST_TUNNEL_RSPAN` and `BCM_MIRROR_DEST_TUNNEL_WITH_SPAN_ID` are DNX-only flags and return `BCM_E_PARAM` on ESW (`mirror.c:27696`). On TD2+, RSPAN cannot occupy MTP slot 0 (`soc_feature_mirror_rspan_no_mtp0`, handled automatically by the SDK at `mirror.c:17088`).

### 2.2 Important SDK & SONiC edge cases handled

1. **Duplicate monitor gport (`BCM_E_EXISTS`) quirk:** In `mirror.c:27924`, `bcm_esw_mirror_destination_create()` (without `BCM_MIRROR_DEST_REPLACE`) calls `_bcm_mirror_destination_match()` (`mirror.c:5682`), which matches any existing **non-tunnel** destination with the same monitor `gport` — even when the new destination is an ERSPAN tunnel. If an ERSPAN session happens to egress the same physical port as an existing SPAN session, the first call returns `BCM_E_EXISTS`. `hwbcm_mirror_dest_create()` catches `BCM_E_EXISTS` and retries with `BCM_MIRROR_DEST_REPLACE` (without `WITH_ID`), which bypasses the match check and allocates a fresh `mirror_dest_id` (`mirror.c:6861`).
2. **Live session updates (`hwSetMirrorSession`):** SONiC's `MirrorOrch` updates `DST_MAC_ADDRESS`, `MONITOR_PORT`, and `VLAN_*` on active sessions when ARP/FDB/routes change. In OpenBCM ESW, `bcm_mirror_destination_create(WITH_ID | REPLACE)` only modifies the software descriptor (`mirror.c:6866`); hardware MTP and `EGR_MIRROR_ENCAP` tables are only written when sources attach. `hwSetMirrorSession` therefore detaches bound ports, calls `hwAclMirrorSessionUpdated(oid, -1)`, destroys and recreates the destination (falling back to in-place `replace` only if an external reference keeps it busy, and rolling back to the old config on failure), then re-attaches ports and calls `hwAclMirrorSessionUpdated(oid, newDest)`.
3. **Staged VLAN attribute updates in SONiC:** `MirrorOrch::updateSessionType()` (`mirrororch.cpp:1224-1255`) sets `VLAN_HEADER_VALID=true`, `VLAN_TPID`, `VLAN_ID`, `VLAN_PRI`, `VLAN_CFI` in five separate `set_mirror_session_attribute` calls. On the first call, `VLAN_ID` is still `0`. `mirrorReadConfig()` treats `VLAN_HEADER_VALID=true` with `VLAN_ID==0` as temporarily untagged (and `MirrorCfgSnapshot::sameAs()` short-circuits no-op updates), then programs the tagged outer header as soon as `VLAN_ID` arrives.
4. **Port mirror diffing:** `mirrorHwSetPort()` computes the diff between current and desired session sets on `(port, direction)`, detaching removed sessions **before** attaching new ones so MTP slots are freed first, and rolling back on error (e.g. MTP exhaustion).

---

## 3. Parity gaps (vs Broadcom SAI on XGS) and open risks

1. **IPv6 ERSPAN not supported:** TD2+ hardware only supports IPv4 GRE encapsulation (`mirror.c:14876` returns `BCM_E_UNAVAIL` for `version != 4`; IPv6 ERSPAN starts on TH3/TD3). IPv6 ERSPAN sessions fail cleanly at create time with `SAI_STATUS_NOT_SUPPORTED`.
2. **ERSPAN Type II / III headers:** TD2+ only emits a 4-byte GRE header (`SAI_ERSPAN_ENCAPSULATION_TYPE_MIRROR_L3_GRE_TUNNEL`, which is what SONiC uses). `TYPE_II` / `TYPE_III` return `SAI_STATUS_NOT_SUPPORTED`; `ERSPAN_SESSION_ID` and `GRE_HEADER_FIRST_16BIT` (if non-zero) are ignored with a WARN.
3. **Truncation (`TRUNCATE_SIZE`):** Not supported on TD2+ (`EGR_MIRROR_TABLE.TRUNCATE_EN` is TD3/TH3 only). Non-zero values log a WARN and are ignored.
4. **Per-session sampling (`SAMPLE_RATE`):** Not supported per mirror destination on TD2+. Values other than 0/1 log a WARN and mirror all packets.
5. **Policer & Counter on mirror session (`POLICER`, `COUNTER_ID`):** Not supported on TD2+ MTPs; ignored with a WARN. (ACL-based EVERFLOW rules still have their own IFP rule counters via `SwitchBCM56860HW_acl.cpp`.)
6. **Multi-port `MONITOR_PORTLIST`:** Only single-port `MONITOR_PORTLIST` (or `MONITOR_PORT`, including a single LAG) is supported; `count > 1` returns `SAI_STATUS_NOT_SUPPORTED`.
7. **`SFLOW` mirror type & `CONGESTION_MODE`:** `SAI_MIRROR_SESSION_TYPE_SFLOW` returns `SAI_STATUS_NOT_SUPPORTED` (`soc_feature_sflow_ing_mirror` is Tomahawk+); non-`INDEPENDENT` congestion mode logs a WARN and is ignored.
8. **Egress mirroring is ingress-pipeline (`BCM_MIRROR_PORT_EGRESS`):** Like standard XGS port egress mirroring (and noted in `DESIGN.md`), mirrored frames reflect the packet before egress-pipe rewrites (e.g. before VXLAN encap / L3 MAC rewrite). `BCM_MIRROR_PORT_EGRESS_TRUE` is not used.
9. **4-MTP hardware limit:** TD2+ has 4 shared MTP slots across all active port and ACL mirror destinations (`BCM_MIRROR_MTP_COUNT = 4`), and RSPAN cannot use slot 0 (max 3 RSPAN MTPs). Exceeding 4 distinct active `(destination, direction)` bindings returns `SAI_STATUS_INSUFFICIENT_RESOURCES`.
10. **CRM availability (`sai_object_type_get_availability`):** vslib's `VirtualSwitchSaiInterface::objectTypeGetAvailability()` returns `SAI_STATUS_NOT_SUPPORTED` for `SAI_OBJECT_TYPE_MIRROR_SESSION`. `MirrorOrch::isHwResourcesAvailable()` (`mirrororch.cpp:365-371`) explicitly treats `NOT_SUPPORTED` as "available, skip check", so SONiC creates sessions normally.
11. **Warm boot:** No hardware mirror reconciliation on warm boot; cold syncd restart replays sessions from orchagent.
12. **Brief gap on live ERSPAN update:** Because `hwSetMirrorSession` detaches sources before recreating the destination, a live MAC/port/VLAN change has a brief sub-millisecond mirroring gap (original data traffic is unaffected).

---

## 4. Hardware test plan

SDK diagnostics go through `deploy/bsh "<cmd>"`. Lab setup from `DESIGN.md`: `Ethernet44` is connected to the PC (`enp2s0`, VLAN 100); `Ethernet48` (`xe47`, logical port 49) can be set to `lb=mac` as a synthetic peer for transit traffic.

### 4.0 Pre-check (after build & deploy)

```bash
show mirror_session
deploy/bsh "mirror dest show"       # "No mirror destinations currently defined"
deploy/bsh "mirror show"            # "No mirror ports configured"
```

### 4.1 Port SPAN (LOCAL) — ingress, egress, both

Mirror traffic on `Ethernet48` to monitor port `Ethernet44` (PC):

```bash
# On the switch:
sudo config mirror_session span add span0 Ethernet44 Ethernet48 both
show mirror_session                 # type=SPAN, status=active, dst_port=Ethernet44, src_port=Ethernet48, direction=both
deploy/bsh "mirror dest show"       # Mirror ID 0 -> local port xe43 (Ethernet44)
deploy/bsh "mirror show"            # xe47 (Ethernet48) ingress & egress -> xe43
deploy/bsh "d IM_MTP_INDEX"         # slot 0 programmed with Ethernet44's mod/port
deploy/bsh "d EM_MTP_INDEX"         # slot 1 programmed with Ethernet44's mod/port
```

**Verify mirrored frames on the PC:**
1. On the PC: `sudo tcpdump -i enp2s0 -nn -e -vv`
2. Put `Ethernet48` in `lb=mac` (`deploy/bsh "port xe47 lb=mac"`) and ping a test IP routed toward `Ethernet48` (or send frames from PC to a MAC/route out `Ethernet48`).
3. Confirm each frame traversing `Ethernet48` appears as an exact copy on `enp2s0` (with `direction=both`, both the egress copy toward `Ethernet48` and the looped-back ingress copy appear; test `rx` and `tx` individually to verify ingress-only vs egress-only).
4. Remove and verify cleanup:
   ```bash
   sudo config mirror_session remove span0
   deploy/bsh "mirror show"          # No mirror ports configured
   deploy/bsh "mirror dest show"     # No mirror destinations currently defined
   ```

### 4.2 Port ERSPAN (ENHANCED_REMOTE, IPv4 GRE)

Mirror `Ethernet48` ingress traffic encapsulated in IPv4 GRE (`0x88be`) to the PC's IP on VLAN 100 (or a routed interface):

```bash
# On the switch (ensure the PC IP is in ARP / neighbor table, e.g. ping it first):
sudo config mirror_session add erspan0 <switch_ip> <pc_ip> 8 64 --src_port Ethernet48 --direction rx
show mirror_session                 # status=active, monitor_port=Ethernet44, dst_mac=<pc_mac>, vlan_id=100 (if SVI)
deploy/bsh "mirror dest show"       # IPv4 GRE Tunnel, Src IP, Dst IP, src_mac, dst_mac, vlan_id, tpid=0x8100, ttl=64, tos=32
deploy/bsh "mirror show"            # xe47 ingress -> xe43
deploy/bsh "d EGR_MIRROR_ENCAP_CONTROL"
deploy/bsh "d EGR_MIRROR_ENCAP_DATA_1"
```

**Verify mirrored GRE frames on the PC:**
1. On the PC: `sudo tcpdump -i enp2s0 -nn -e -vv proto 47` (or capture to `.pcap` and open in Wireshark to dissect inner Ethernet).
2. Generate traffic entering `Ethernet48` (e.g. looped-back pings from `Ethernet48` in `lb=mac`).
3. Confirm on `enp2s0`:
   - Outer Ethernet: `src_mac = <switch MAC>`, `dst_mac = <PC MAC>`, `802.1Q VLAN 100` (when `MirrorOrch` resolved the neighbor on `Vlan100`, or untagged when on a routed port).
   - Outer IPv4: `src = <switch_ip>`, `dst = <pc_ip>`, `tos = 0x20` (DSCP 8 << 2), `ttl = 64`, `proto = 47 (GRE)`.
   - GRE header: `0x0000 0x88be` followed by the original inner Ethernet frame.
4. Remove with `sudo config mirror_session remove erspan0` and confirm `bsh "mirror dest show"` and `bsh "mirror show"` are empty.

### 4.3 ACL / EVERFLOW ERSPAN (hook 1.7 verification)

```bash
# On the switch:
sudo config mirror_session add ev0 <switch_ip> <pc_ip> 8 64
sudo config acl add table EVERFLOW MIRROR -p Ethernet44
```

Add an `ACL_RULE` via `sonic-cfggen -j /tmp/ev_rule.json --write-to-db`:

```json
{
  "ACL_RULE": {
    "EVERFLOW|MIRROR_ICMP": {
      "PRIORITY": "100",
      "IP_PROTOCOL": "1",
      "MIRROR_ACTION": "ev0"
    }
  }
}
```

**Verify:**
1. `deploy/bsh "fp show"` and `deploy/bsh "fp show entry <eid>"`: confirm `MirrorIngress` action is installed with the mirror gport (`0x3c00000x` matching `mirror dest show`), and syslog has **no** `"has no hardware destination yet"` WARN.
2. On the PC: `sudo tcpdump -i enp2s0 -nn -e -vv proto 47` in one terminal; send `ping -c 5 <switch_ip>` and `nping --udp -p 9999 -c 5 <switch_ip>` in another.
3. Confirm **only** the 5 ICMP packets produce GRE-encapsulated mirrored frames on `enp2s0`, and `aclshow -a` counts 5 packets.
4. Delete the rule, then `sudo config mirror_session remove ev0`; confirm `fp show` and `mirror dest show` are clean and syncd logs no `BCM_E_BUSY`.

### 4.4 Live ERSPAN path update (`hwSetMirrorSession`)

With `ev0` active and mirroring:
1. Change the PC's static neighbor MAC (or move the FDB entry to another port in `Vlan100`, or switch route next hop from a routed port to `Vlan100` to trigger `updateSessionType` + `updateSessionDstMac` + `updateSessionDstPort`).
2. Confirm `show mirror_session` and `deploy/bsh "mirror dest show"` immediately show the new `dst_mac` / `monitor_port` / `vlan_id`, `fp show entry <eid>` still has `MirrorIngress` pointing at the updated destination, and `tcpdump` sees GRE packets with the new outer header.

### 4.5 Negative / limit checks

- **MTP exhaustion (5th active MTP):** Configure 4 active `(session, dir)` bindings, then add a 5th -> confirm syslog logs `BCM_E_RESOURCE` / `SAI_STATUS_INSUFFICIENT_RESOURCES`, existing 4 sessions keep mirroring, and syncd stays up.
- **SPAN + ERSPAN sharing the same monitor port:** Create `span0` targeting `Ethernet44` and `ev0` whose route also egresses `Ethernet44` simultaneously -> confirm both destinations succeed (exercises the `BCM_E_EXISTS` retry workaround in `hwbcm_mirror_dest_create`).
- **IPv6 ERSPAN:** `sudo config mirror_session add ev6 2001:db8::1 2001:db8::2 8 64` (with an active IPv6 neighbor) -> confirm clean rejection with `SAI_STATUS_NOT_SUPPORTED` in syslog and syncd stays up.
