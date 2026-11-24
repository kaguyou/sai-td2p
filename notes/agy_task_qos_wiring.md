# Task for agy: wire the QoS/buffer feature into the SAI and make it build

Repo: `/home/kaguyou/homelab/sai-td2p` (custom SAI for SONiC on Edgecore AS5812-54T / Broadcom Trident2+).
The QoS/buffer implementation is already written (new files) and syntax-checked; your job is ONLY the wiring
described in `notes/integration_qos.md` section 1, then a clean full build. ACL and CoPP were wired just before
you (see `notes/agy_report_acl.md`, `notes/agy_report_copp.md`) — keep their edits intact.

## Read first
- `notes/integration_qos.md` (section 1 = exact code; follow it literally)
- `sairedis/vslib/SwitchBCM56860HW.h`, `SwitchBCM56860HW.cpp` (create/remove/set dispatch, hwCreatePort, hwRemovePort, hwSetPort, hwSetSwitch, initialize_default_objects / warm_boot_initialize_objects, buildPortMaps)
- New QoS files: `sairedis/vslib/SwitchBCM56860HW_qos.cpp`, `SwitchBCM56860HW_qos.inc.h`, `hwbcm/hwbcm_qos.c`, `hwbcm/hwbcm_qos.h`

## Do
1. Header: `#include "SwitchBCM56860HW_qos.inc.h"` as the last line inside the class (after stats, acl, copp includes).
2. All wiring of section 1: qosBuildHwMap() after base init and after buildPortMaps on warm boot; hooks in hwCreatePort / hwRemovePort / hwSetPort / hwSetSwitch; the create / remove / set dispatch lines.
3. `/home/kaguyou/homelab/sonic-sairedis/vslib/Makefile.am`: add `SwitchBCM56860HW_qos.cpp` after the copp .cpp line and `hwbcm/hwbcm_qos.c` after the hwbcm copp line.
4. Copy every changed/new file from `sai-td2p/sairedis/vslib/` to the same path under `/home/kaguyou/homelab/sonic-sairedis/vslib/`.
5. Build: `cd /home/kaguyou/homelab/sai-td2p && ./deploy/build.sh --no-meta > /tmp/agy_build_qos.log 2>&1; grep -E "error:|ALL CHECKS|FAIL" /tmp/agy_build_qos.log` — fix until `==> ALL CHECKS PASSED` (C++ is -Werror).
6. Report to `notes/agy_report_qos.md`: changes (file:line), build sha256 of `deploy/build/libsaivs.so.0.0.0`, any fixes and why.

## Do NOT
- No ssh/scp to the switch (192.168.52.1, 192.168.0.2), no /dev/ttyUSB0, no deploy/install.sh. Never run `cos port config` or anything on the switch.
- Do not run `sairedis/sync_from_sairedis.sh` (it deletes sai-td2p/sairedis/vslib).
- Do not edit DESIGN.md, README.md. No git commit/push.
