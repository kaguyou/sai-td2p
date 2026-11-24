# ACL Wiring Report

## Changes Made

### `sairedis/vslib/SwitchBCM56860HW.h` (copied to `/home/kaguyou/homelab/sonic-sairedis/vslib/SwitchBCM56860HW.h`)
- **Lines 73–77**: Added `virtual sai_status_t get(...) override;` declaration in the public dispatch section (section 1.1).
- **Line 433**: Added `#include "SwitchBCM56860HW_acl.inc.h"` as the last line inside `class SwitchBCM56860HW`, right after `#include "SwitchBCM56860HW_stats.inc.h"` (section 1.1).

### `sairedis/vslib/SwitchBCM56860HW.cpp` (copied to `/home/kaguyou/homelab/sonic-sairedis/vslib/SwitchBCM56860HW.cpp`)
- **Line 2347**: Added `hwAclLagMembersChanged(lag->value.oid);` after `syncLagMemberPort(..., true)` in `hwCreateLagMember()` (section 1.6).
- **Line 2486**: Added `hwAclLagMembersChanged(lag);` after `syncLagMemberPort(..., false)` in `hwRemoveLagMember()` (section 1.6).
- **Lines 2561–2567**: Added `create()` switch dispatch for `ACL_TABLE`, `ACL_ENTRY`, `ACL_COUNTER`, `ACL_RANGE`, `ACL_TABLE_GROUP`, `ACL_TABLE_GROUP_MEMBER` (section 1.2).
- **Lines 2575–2580**: Added `hwAclBindOnCreate()` call after the `create()` switch for `PORT`, `LAG`, `VLAN`, `ROUTER_INTERFACE` (section 1.2).
- **Lines 2623–2627**: Added `hwAclObjectRemoved(oid)` call inside `if (m_hwReady)` before the `remove()` switch for `PORT`, `LAG`, `VLAN`, `ROUTER_INTERFACE` (section 1.3).
- **Lines 2657–2663**: Added `remove()` switch dispatch for the six ACL object types (section 1.3).
- **Lines 2708–2722**: Added `hwAclIsBindAttr()` / `hwAclBind()` branch before the `set()` switch and added `ACL_ENTRY` (`hwSetAclEntry`) and `ACL_COUNTER` (`hwSetAclCounter`) cases (section 1.4).
- **Lines 2741–2757**: Added `SwitchBCM56860HW::get()` override calling `hwAclCounterGet()` for `SAI_OBJECT_TYPE_ACL_COUNTER` (section 1.5).
- Mirror hook (section 1.7) was intentionally skipped.

### `/home/kaguyou/homelab/sonic-sairedis/vslib/Makefile.am`
- **Line 60**: Added `SwitchBCM56860HW_acl.cpp \` after `SwitchBCM56860HW_stats.cpp \`.
- **Lines 83–84**: Added `hwbcm/hwbcm_acl.c` after `hwbcm/hwbcm_stats.c \`.

### Copied ACL files to `/home/kaguyou/homelab/sonic-sairedis/vslib/`
- `SwitchBCM56860HW.h`
- `SwitchBCM56860HW.cpp`
- `SwitchBCM56860HW_acl.cpp`
- `SwitchBCM56860HW_acl.inc.h`
- `hwbcm/hwbcm_acl.c`
- `hwbcm/hwbcm_acl.h`

## Build Result

- Command: `./deploy/build.sh --no-meta > /tmp/agy_build_acl.log 2>&1`
- Result: `==> ALL CHECKS PASSED  ->  /home/kaguyou/homelab/sai-td2p/deploy/build/libsaivs.so.0.0.0`
- Size: `144602872 bytes (138M)`
- `sha256` (`deploy/build/libsaivs.so.0.0.0`): `557149d98703999bc41fb9e13a72f61da193a08ba61987879eae92582dc574ef`
- Symbols: 128 `hwbcm_*` defined (including all 16 `hwbcm_acl_*` functions), 32449 `bcm_*` defined, 0 undefined SDK/hwbcm symbols, all version/ldd checks passed.

## Fixes Required in ACL Files

None — `SwitchBCM56860HW_acl.cpp`, `SwitchBCM56860HW_acl.inc.h`, `hwbcm/hwbcm_acl.c`, and `hwbcm/hwbcm_acl.h` compiled and linked cleanly under `-Werror` on the first build without modification.
