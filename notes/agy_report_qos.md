# QoS / Buffer Wiring Report

## Changes Made

### `sairedis/vslib/SwitchBCM56860HW.h` (copied to `/home/kaguyou/homelab/sonic-sairedis/vslib/SwitchBCM56860HW.h`)
- **Line 22**: Added `SwitchBCM56860HW_qos.cpp` entry to the file split comment at the top.
- **Line 437**: Added `#include "SwitchBCM56860HW_qos.inc.h"` as the last line inside `class SwitchBCM56860HW`, after the stats, acl, and copp includes (section 1a).

### `sairedis/vslib/SwitchBCM56860HW.cpp` (copied to `/home/kaguyou/homelab/sonic-sairedis/vslib/SwitchBCM56860HW.cpp`)
- **Line 261**: Added `(void)qosBuildHwMap();` in `initialize_default_objects()` right after `SwitchBCM56850::initialize_default_objects(...)` (section 1b.1).
- **Lines 290–292**: Replaced `return buildPortMaps(true);` in `warm_boot_initialize_objects()` with `CHECK_STATUS(buildPortMaps(true)); return qosBuildHwMap();` (section 1b.2).
- **Line 544**: Added `qosOnPortMapped(port_id);` in `hwCreatePort()` right after `mapPort(port_id, bcm, lane);` (section 1b.4).
- **Lines 572–575**: Added `if (qosIsPortAttr(attr->id)) return qosHwSetPort(port_id, attr);` in `hwSetPort()` right after the `attr == NULL` check and before `bcmPortOf(port_id)` (section 1b.3).
- **Line 694**: Added `qosOnPortRemoved(port_id);` in `hwRemovePort()` right before `unmapPort(port_id);` (section 1b.5).
- **Lines 961–964**: Added `if (qosIsSwitchAttr(attr->id)) return qosHwSetSwitch(attr);` in `hwSetSwitch()` before `switch (attr->id)` (section 1b.6).
- **Lines 2602–2607**: Added `create()` switch dispatch for `QOS_MAP`, `SCHEDULER`, `WRED`, `BUFFER_POOL`, `BUFFER_PROFILE` (section 1b.7).
- **Lines 2708–2713**: Added `remove()` switch dispatch for `QOS_MAP`, `SCHEDULER`, `WRED`, `BUFFER_POOL`, `BUFFER_PROFILE` (section 1b.8).
- **Lines 2781–2788**: Added `set()` switch dispatch for `QOS_MAP`, `SCHEDULER`, `WRED`, `BUFFER_POOL`, `BUFFER_PROFILE`, `QUEUE`, `SCHEDULER_GROUP`, `INGRESS_PRIORITY_GROUP` (section 1b.9).

### `/home/kaguyou/homelab/sonic-sairedis/vslib/Makefile.am`
- **Line 62**: Added `SwitchBCM56860HW_qos.cpp \` after `SwitchBCM56860HW_copp.cpp \`.
- **Lines 87–88**: Added `hwbcm/hwbcm_qos.c` after `hwbcm/hwbcm_copp.c \`.

### Copied QoS files to `/home/kaguyou/homelab/sonic-sairedis/vslib/`
- `SwitchBCM56860HW.h`
- `SwitchBCM56860HW.cpp`
- `SwitchBCM56860HW_qos.cpp`
- `SwitchBCM56860HW_qos.inc.h`
- `hwbcm/hwbcm_qos.c`
- `hwbcm/hwbcm_qos.h`

## Fixes Required in QoS Files

None — `SwitchBCM56860HW_qos.cpp`, `SwitchBCM56860HW_qos.inc.h`, `hwbcm/hwbcm_qos.c`, and `hwbcm/hwbcm_qos.h` compiled and linked cleanly under `-Werror` on the first build without modification.

## Build Result

- Command: `./deploy/build.sh --no-meta > /tmp/agy_build_qos.log 2>&1`
- Result: `==> ALL CHECKS PASSED  ->  /home/kaguyou/homelab/sai-td2p/deploy/build/libsaivs.so.0.0.0`
- Size: `146543920 bytes (140M)`
- Symbols: 157 `hwbcm_*` defined (including all 17 `hwbcm_qos_*`, 12 `hwbcm_copp_*`, 19 `hwbcm_acl_*`, and 4 `hwbcm_stats_*` symbols), 32449 `bcm_*` defined, 0 undefined SDK/hwbcm symbols, all version/ldd checks passed.
- Final `sha256` (`deploy/build/libsaivs.so.0.0.0`): `61c6b46a7607927bf26513b50fd7cf2fd4a6b4ca8f4a7366d59e036509985913`
