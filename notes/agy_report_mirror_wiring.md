# Mirror Wiring Report

## Changes Made

### `sairedis/vslib/SwitchBCM56860HW.h` (copied to `/home/kaguyou/homelab/sonic-sairedis/vslib/SwitchBCM56860HW.h`)
- **Line 23**: Added `SwitchBCM56860HW_mirror.cpp` entry to the file split comment at the top.
- **Line 439**: Added `#include "SwitchBCM56860HW_mirror.inc.h"` as the last line inside `class SwitchBCM56860HW`, after `#include "SwitchBCM56860HW_qos.inc.h"` (section 1.1).

### `sairedis/vslib/SwitchBCM56860HW.cpp` (copied to `/home/kaguyou/homelab/sonic-sairedis/vslib/SwitchBCM56860HW.cpp`)
- **Lines 577–580**: Added `if (mirrorIsPortAttr(attr->id)) return mirrorHwSetPort(port_id, attr);` in `hwSetPort()` right after the `qosIsPortAttr` check and before `bcmPortOf(port_id)` (section 1.5).
- **Line 701**: Added `mirrorOnPortRemoved(port_id);` in `hwRemovePort()` right after `qosOnPortRemoved(port_id);` and before `unmapPort(port_id);` (section 1.5).
- **Lines 2616–2617**: Added `create()` switch dispatch for `SAI_OBJECT_TYPE_MIRROR_SESSION` -> `hwCreateMirrorSession(oid, attr_count, attr_list)` after the QoS cases (section 1.2).
- **Lines 2725–2726**: Added `remove()` switch dispatch for `SAI_OBJECT_TYPE_MIRROR_SESSION` -> `hwRemoveMirrorSession(oid)` after the QoS cases (section 1.3).
- **Line 2802**: Added `set()` switch dispatch for `SAI_OBJECT_TYPE_MIRROR_SESSION` -> `hwSetMirrorSession(oid, attr)` after the QoS cases (section 1.4).
- Preserved `SwitchBCM56860HW::get()` ACL counter behavior (`SAI_OBJECT_TYPE_ACL_COUNTER` skipping base `get()` for `PACKETS`/`BYTES`) and all existing ACL, CoPP, QoS, and stats wiring unchanged.

### ACL Mirror Hook (`notes/integration_acl.md` 1.7 / `notes/integration_mirror.md` 1.6)
- Verified already wired directly in `sairedis/vslib/SwitchBCM56860HW_mirror.cpp`:
  - **Line 530** (`hwCreateMirrorSession`): `hwAclMirrorSessionUpdated(session_id, destGport)` after creating the hardware destination.
  - **Lines 637, 671, 699, 728** (`hwSetMirrorSession`): `hwAclMirrorSessionUpdated(session_id, -1)` before updating/recreating the destination, and `hwAclMirrorSessionUpdated(session_id, newDest)` (or rollback dest) afterwards.
  - **Line 802** (`hwRemoveMirrorSession`): `hwAclMirrorSessionUpdated(session_id, -1)` before `hwbcm_mirror_dest_destroy(destGport)`.

### `/home/kaguyou/homelab/sonic-sairedis/vslib/Makefile.am`
- **Line 63**: Added `SwitchBCM56860HW_mirror.cpp \` after `SwitchBCM56860HW_qos.cpp \`.
- **Lines 89–90**: Added `hwbcm/hwbcm_mirror.c` after `hwbcm/hwbcm_qos.c \`.

### Copied files to `/home/kaguyou/homelab/sonic-sairedis/vslib/`
- `SwitchBCM56860HW.h`
- `SwitchBCM56860HW.cpp`
- `SwitchBCM56860HW_mirror.inc.h`
- `SwitchBCM56860HW_mirror.cpp`
- `hwbcm/hwbcm_mirror.h`
- `hwbcm/hwbcm_mirror.c`

## Fixes Required in Mirror Files

None — `SwitchBCM56860HW_mirror.cpp`, `SwitchBCM56860HW_mirror.inc.h`, `hwbcm/hwbcm_mirror.c`, and `hwbcm/hwbcm_mirror.h` compiled and linked cleanly under `-Werror` on the first build without modification.

## Build Result

- Command: `./deploy/build.sh --no-meta > /tmp/agy_build_mirror.log 2>&1`
- Result: `==> ALL CHECKS PASSED  ->  /home/kaguyou/homelab/sai-td2p/deploy/build/libsaivs.so.0.0.0`
- Size: `147060288 bytes (141M)`
- Symbols: 165 `hwbcm_*` defined (including all 8 `hwbcm_mirror_*` symbols: `hwbcm_mirror_dest_cfg_init`, `hwbcm_mirror_dest_create`, `hwbcm_mirror_dest_replace`, `hwbcm_mirror_dest_destroy`, `hwbcm_mirror_port_add`, `hwbcm_mirror_port_delete`, `hwbcm_mirror_port_delete_all`, `hwbcm_mirror_dump`), 32449 `bcm_*` defined, 0 undefined SDK/hwbcm symbols, all version/ldd checks passed.
- Final `sha256` (`deploy/build/libsaivs.so.0.0.0`): `99a8c64f0a5236add959f70f7e26145443420d886ef2b85b4692b0e0246a04e7`
