# CoPP Wiring Report

## Changes Made

### `sairedis/vslib/SwitchBCM56860HW.h` (copied to `/home/kaguyou/homelab/sonic-sairedis/vslib/SwitchBCM56860HW.h`)
- **Line 21**: Added `SwitchBCM56860HW_copp.cpp` entry to the file split comment at the top (section 1.1).
- **Line 435**: Added `#include "SwitchBCM56860HW_copp.inc.h"` as the last line inside `class SwitchBCM56860HW`, right after `#include "SwitchBCM56860HW_stats.inc.h"` and `#include "SwitchBCM56860HW_acl.inc.h"` (section 1.1).

### `sairedis/vslib/SwitchBCM56860HW.cpp` (copied to `/home/kaguyou/homelab/sonic-sairedis/vslib/SwitchBCM56860HW.cpp`)
- **Lines 878–883**: Added `SAI_OBJECT_TYPE_COUNTER` dispatch to `hwCoppCounterStats()` inside `getStatsExt()` right after `hwGetObjectStats()` and before `int bcm = ...` (section 1.5).
- **Lines 2544–2549**: Updated `SAI_OBJECT_TYPE_HOSTIF_TRAP` in `create()` to call `hwCoppOnTrapCreate(oid)` after `hwCreateHostifTrap(...)`, and added `SAI_OBJECT_TYPE_HOSTIF_TRAP_GROUP` (`hwCreateHostifTrapGroup`) and `SAI_OBJECT_TYPE_POLICER` (`hwCreatePolicer`) cases (section 1.2).
- **Lines 2563–2566**: Updated `SAI_OBJECT_TYPE_ROUTE_ENTRY` in `create()` to call `hwCoppOnRoute(serializedObjectId)` after a successful `hwCreateRoute(...)` (section 1.2).
- **Lines 2649–2651**: Added `SAI_OBJECT_TYPE_HOSTIF_TRAP` (`hwCoppOnTrapRemove`), `SAI_OBJECT_TYPE_HOSTIF_TRAP_GROUP` (`hwRemoveHostifTrapGroup`), and `SAI_OBJECT_TYPE_POLICER` (`hwRemovePolicer`) cases in `remove()` next to `HOSTIF` (section 1.3).
- **Lines 2733–2745**: Updated `SAI_OBJECT_TYPE_SWITCH` in `set()` to call `hwCoppRefresh()` when `SAI_SWITCH_ATTR_SRC_MAC_ADDRESS` is set; added `SAI_OBJECT_TYPE_HOSTIF_TRAP` (`hwCoppOnTrapSet`), `SAI_OBJECT_TYPE_HOSTIF_TRAP_GROUP` (`hwSetHostifTrapGroup`), and `SAI_OBJECT_TYPE_POLICER` (`hwSetPolicer`) cases; updated `SAI_OBJECT_TYPE_ROUTE_ENTRY` to call `hwCoppOnRoute(serializedObjectId)` after a successful `hwSetRoute(...)` (section 1.4).

### `/home/kaguyou/homelab/sonic-sairedis/vslib/Makefile.am`
- **Line 61**: Added `SwitchBCM56860HW_copp.cpp \` after `SwitchBCM56860HW_acl.cpp \`.
- **Lines 85–86**: Added `hwbcm/hwbcm_copp.c` after `hwbcm/hwbcm_acl.c \`.

### Copied CoPP files to `/home/kaguyou/homelab/sonic-sairedis/vslib/`
- `SwitchBCM56860HW.h`
- `SwitchBCM56860HW.cpp`
- `SwitchBCM56860HW_copp.cpp`
- `SwitchBCM56860HW_copp.inc.h`
- `hwbcm/hwbcm_copp.c`
- `hwbcm/hwbcm_copp.h`

## Build Result

- Command: `./deploy/build.sh --no-meta > /tmp/agy_build_copp.log 2>&1`
- Result: `==> ALL CHECKS PASSED  ->  /home/kaguyou/homelab/sai-td2p/deploy/build/libsaivs.so.0.0.0`
- Size: `144965552 bytes (139M)`
- `sha256` (`deploy/build/libsaivs.so.0.0.0`): `994ea297db841bb1c7ffdf8f7a714a4620fa71568dea1d2a9dda7afb9c4fb09a`
- Symbols: 140 `hwbcm_*` defined (including all 12 `hwbcm_copp_*` functions), 32449 `bcm_*` defined, 0 undefined SDK/hwbcm symbols, all version/ldd checks passed.

## Fixes Required in CoPP Files

None — `SwitchBCM56860HW_copp.cpp`, `SwitchBCM56860HW_copp.inc.h`, `hwbcm/hwbcm_copp.c`, and `hwbcm/hwbcm_copp.h` compiled and linked cleanly under `-Werror` without modification. (The first build attempt failed solely because `hwbcm_copp.h` had been copied to `vslib/` instead of `vslib/hwbcm/`; moving it into `vslib/hwbcm/` resolved the build immediately.)
