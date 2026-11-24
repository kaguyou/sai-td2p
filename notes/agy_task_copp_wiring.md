# Task for agy: wire the CoPP feature into the SAI and make it build

Repo: `/home/kaguyou/homelab/sai-td2p` (custom SAI for SONiC on Edgecore AS5812-54T / Broadcom Trident2+).
The CoPP implementation is already written (new files) and syntax-checked; your job is ONLY the wiring
described in `notes/integration_copp.md` section 1, then a clean full build. The ACL feature was wired just
before you (see `notes/agy_report_acl.md`) — keep its edits intact and add CoPP next to them.

## Read first
- `notes/integration_copp.md` (section 1 = exact code; follow it literally; it was test-compiled against the current files)
- `sairedis/vslib/SwitchBCM56860HW.h`, `SwitchBCM56860HW.cpp` (create/remove/set dispatch, hwCreateHostifTrap, getStatsExt), `SwitchBCM56860HW_l3.cpp` (hwCreateRoute / hwSetRoute)
- New CoPP files: `sairedis/vslib/SwitchBCM56860HW_copp.cpp`, `SwitchBCM56860HW_copp.inc.h`, `hwbcm/hwbcm_copp.c`, `hwbcm/hwbcm_copp.h`

## Do
1. Header: `#include "SwitchBCM56860HW_copp.inc.h"` as the last line inside the class (after the stats and acl includes).
2. All wiring edits of section 1 (POLICER / HOSTIF_TRAP_GROUP dispatch in create/set/remove, HOSTIF_TRAP set+remove, hwCoppOnTrapCreate after hwCreateHostifTrap, hwCoppOnRoute after successful hwCreateRoute/hwSetRoute, hwCoppRefresh on SRC_MAC set, the optional COUNTER block in getStatsExt).
3. `/home/kaguyou/homelab/sonic-sairedis/vslib/Makefile.am`: add `SwitchBCM56860HW_copp.cpp` after the acl .cpp line and `hwbcm/hwbcm_copp.c` after the hwbcm acl line.
4. Copy every changed/new file from `sai-td2p/sairedis/vslib/` to the same path under `/home/kaguyou/homelab/sonic-sairedis/vslib/`.
5. Build: `cd /home/kaguyou/homelab/sai-td2p && ./deploy/build.sh --no-meta > /tmp/agy_build_copp.log 2>&1; grep -E "error:|ALL CHECKS|FAIL" /tmp/agy_build_copp.log` — fix until `==> ALL CHECKS PASSED` (C++ is -Werror).
6. Report to `notes/agy_report_copp.md`: changes (file:line), build sha256 of `deploy/build/libsaivs.so.0.0.0`, any fixes and why.

## Do NOT
- No ssh/scp to the switch (192.168.52.1, 192.168.0.2), no /dev/ttyUSB0, no deploy/install.sh.
- Do not run `sairedis/sync_from_sairedis.sh` (it deletes sai-td2p/sairedis/vslib).
- Do not edit *_qos* files (another feature in progress), DESIGN.md, README.md. No git commit/push.
