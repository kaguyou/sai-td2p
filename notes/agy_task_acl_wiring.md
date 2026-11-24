# Task for agy: wire the ACL feature into the SAI and make it build

Repo: `/home/kaguyou/homelab/sai-td2p` (custom SAI for SONiC on Edgecore AS5812-54T / Broadcom Trident2+).
The ACL implementation is already written (new files) and syntax-checked; your job is ONLY the wiring
described in `notes/integration_acl.md` section 1, then a clean full build.

## Read first
- `notes/integration_acl.md` (section 1 = exact code to add; follow it literally)
- `sairedis/vslib/SwitchBCM56860HW.h`, `sairedis/vslib/SwitchBCM56860HW.cpp` (create / remove / set dispatch, hwCreateLagMember / hwRemoveLagMember)
- New ACL files: `sairedis/vslib/SwitchBCM56860HW_acl.cpp`, `SwitchBCM56860HW_acl.inc.h`, `hwbcm/hwbcm_acl.c`, `hwbcm/hwbcm_acl.h`

## Do
1. `SwitchBCM56860HW.h`: add `#include "SwitchBCM56860HW_acl.inc.h"` as the last line inside the class, right after the existing `#include "SwitchBCM56860HW_stats.inc.h"`. Add the `get()` override declaration (section 1.1).
2. `SwitchBCM56860HW.cpp`: create/remove/set dispatch (1.2, 1.3, 1.4), the `get()` override (1.5), LAG hooks in hwCreateLagMember / hwRemoveLagMember (1.6). Do not implement the mirror hook (1.7) — mirror does not exist yet.
3. Makefile.am: the build tree is `/home/kaguyou/homelab/sonic-sairedis/vslib/Makefile.am`. Add `SwitchBCM56860HW_acl.cpp` after `SwitchBCM56860HW_stats.cpp`, and `hwbcm/hwbcm_acl.c` after `hwbcm/hwbcm_stats.c` (same style as the existing lines).
4. Copy every file you changed or that is new for ACL from `sai-td2p/sairedis/vslib/` to the same relative path under `/home/kaguyou/homelab/sonic-sairedis/vslib/` (the build compiles that tree). Files: SwitchBCM56860HW.h, SwitchBCM56860HW.cpp, SwitchBCM56860HW_acl.cpp, SwitchBCM56860HW_acl.inc.h, hwbcm/hwbcm_acl.c, hwbcm/hwbcm_acl.h.
5. Build: `cd /home/kaguyou/homelab/sai-td2p && ./deploy/build.sh --no-meta > /tmp/agy_build_acl.log 2>&1; grep -E "error:|ALL CHECKS|FAIL" /tmp/agy_build_acl.log`.
   C++ is compiled with -Werror. Fix any error/warning (in sai-td2p first, then copy again) until the last line is `==> ALL CHECKS PASSED`.
6. Write a short report to `notes/agy_report_acl.md`: what you changed (file:line), build result (sha256 of `deploy/build/libsaivs.so.0.0.0`), anything you had to fix in the ACL files and why.

## Do NOT
- Do not ssh/scp to the switch (192.168.52.1, 192.168.0.2) or touch /dev/ttyUSB0. Do not run deploy/install.sh or redeploy anything.
- Do not run `sairedis/sync_from_sairedis.sh` (it deletes sai-td2p/sairedis/vslib and would destroy other in-progress work).
- Do not edit files belonging to other features: anything named *_copp*, *_qos*, *_stats* (other than reading), DESIGN.md, README.md.
- No git commit / push.
