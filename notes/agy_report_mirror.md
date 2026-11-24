# Mirror (SPAN / RSPAN / ERSPAN) Implementation Report

## Deliverables Created (New Files Only)

| File | Description |
|---|---|
| `sairedis/vslib/hwbcm/hwbcm_mirror.h` | SDK-free C API (`hwbcm_mirror_*`) for mirror destinations, per-port ingress/egress mirror bindings, and debug dump |
| `sairedis/vslib/hwbcm/hwbcm_mirror.c` | OpenBCM `sdk-6.5.27` implementation targeting Trident2+ (`BCM56864` / `BCM56860_A0`) |
| `sairedis/vslib/SwitchBCM56860HW_mirror.inc.h` | Class member declarations and state to `#include` at the end of `class SwitchBCM56860HW` |
| `sairedis/vslib/SwitchBCM56860HW_mirror.cpp` | `SAI_OBJECT_TYPE_MIRROR_SESSION` create/set/remove handlers, `SAI_PORT_ATTR_INGRESS/EGRESS_MIRROR_SESSION` port hooks, and ACL hook integration |
| `notes/integration_mirror.md` | Exact wiring instructions (`SwitchBCM56860HW.h`, `SwitchBCM56860HW.cpp`, `Makefile.am`), design notes, parity gaps, and hardware test plan |
| `notes/agy_report_mirror.md` | This report |

No existing files were modified, and `/home/kaguyou/homelab/sonic-sairedis` was not touched.

## Implemented Scope

- **`SAI_OBJECT_TYPE_MIRROR_SESSION`:**
  - `SAI_MIRROR_SESSION_TYPE_LOCAL` (SPAN), `SAI_MIRROR_SESSION_TYPE_ENHANCED_REMOTE` (ERSPAN IPv4 L3 GRE with src/dst IP, src/dst MAC, TOS, TTL, GRE protocol default `0x88be`, optional outer 802.1Q VLAN tag with TPID/VID/PRI/CFI), and `SAI_MIRROR_SESSION_TYPE_REMOTE` (RSPAN L2 VLAN tunnel).
  - Supports both physical port (`SAI_OBJECT_TYPE_PORT`) and LAG (`SAI_OBJECT_TYPE_LAG`) monitor ports, optional `SAI_MIRROR_SESSION_ATTR_TC` (`BCM_MIRROR_DEST_INT_PRI_SET`), and live `set_mirror_session_attribute` updates (`MONITOR_PORT`, `DST_MAC_ADDRESS`, `VLAN_*`, etc.).
- **Port mirroring:**
  - `SAI_PORT_ATTR_INGRESS_MIRROR_SESSION` and `SAI_PORT_ATTR_EGRESS_MIRROR_SESSION` via `mirrorIsPortAttr` / `mirrorHwSetPort` / `mirrorOnPortRemoved`, with diff-based detach-before-attach updates and rollback on MTP exhaustion.
- **ACL hook (`notes/integration_acl.md` 1.7):**
  - Calls `hwAclMirrorSessionUpdated(session_id, destGport)` after creating/updating a destination and `hwAclMirrorSessionUpdated(session_id, -1)` before destroying it.

## Key Trident2+ SDK Details Verified

- `src/bcm/esw/mirror.c:12745-12887`: TD2+ ERSPAN (`_bcm_trident_mirror_ipv4_gre_tunnel_set`) is IPv4-only (`version != 4` returns `BCM_E_UNAVAIL`), checks `BCM_VLAN_VALID(BCM_VLAN_CTRL_ID(vlan_id))`, and writes `(tpid << 16) | vlan_id` verbatim — so PRI/CFI/VID are packed with `BCM_VLAN_CTRL(pri, cfi, vid)`.
- `src/bcm/esw/mirror.c:27696`: `BCM_MIRROR_DEST_TUNNEL_WITH_SPAN_ID` and `BCM_MIRROR_DEST_TUNNEL_RSPAN` return `BCM_E_PARAM` on ESW; RSPAN uses `BCM_MIRROR_DEST_TUNNEL_L2`.
- `src/bcm/esw/mirror.c:27924` & `:5682`: creating a tunnel (or second SPAN) destination on the same monitor gport as an existing non-tunnel SPAN destination returns `BCM_E_EXISTS`; `hwbcm_mirror_dest_create` retries with `BCM_MIRROR_DEST_REPLACE` (without `WITH_ID`) to allocate a fresh `mirror_dest_id`.
- `sonic-swss/orchagent/mirrororch.cpp:1224-1255`: `updateSessionType` sets `VLAN_HEADER_VALID=true` before `VLAN_ID`; `mirrorReadConfig` handles `VLAN_HEADER_VALID=true && VLAN_ID==0` cleanly without failing the intermediate call.

## Syntax Check Results (in `saibuild`)

All checks passed with exit code 0 and zero warnings:

- **C (exact prompt command):**
  `gcc -std=gnu99 -fsyntax-only -Wall -Wextra -Werror $(sed -e "s/ -f$//" -e "s/-DBCM_HIDE_DISPATCHABLE//" /w/sai-td2p/sdk_cflags.txt) $(cat /w/sai-td2p/sdk_incs.txt) -Ihwbcm hwbcm/hwbcm_mirror.c` — **PASS**
- **C (strict trailing `-Wall -Wextra -Werror`):** **PASS**
- **C++ (`vslib_cxx_cmd.txt` flags + `-fsyntax-only`, includes `-Werror -Wextra -Wconversion -Wshadow`, from `/w/sonic-sairedis/vslib` against a scratch copy of `SwitchBCM56860HW.h` with `#include "SwitchBCM56860HW_mirror.inc.h"` added):**
  - `SwitchBCM56860HW_mirror.cpp` — **PASS**
  - `SwitchBCM56860HW_acl.cpp` — **PASS**
  - `SwitchBCM56860HW_copp.cpp` — **PASS**
  - `SwitchBCM56860HW_qos.cpp` — **PASS**
  - `SwitchBCM56860HW.cpp` — **PASS**
