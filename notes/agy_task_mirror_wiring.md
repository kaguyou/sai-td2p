# Task for agy: wire the mirror feature into the SAI and make it build

Repo `/home/kaguyou/homelab/sai-td2p`. The mirror implementation you wrote is in
`sairedis/vslib/SwitchBCM56860HW_mirror.cpp/.inc.h`, `sairedis/vslib/hwbcm/hwbcm_mirror.c/.h`; wiring is in `notes/integration_mirror.md`.
ACL, CoPP, QoS and stats are already wired (keep them intact). Note: SwitchBCM56860HW::get() was just changed for ACL counters
(it no longer calls the base get first for ACL_COUNTER) — keep that behaviour.

Do:
1. Apply every wiring step of `notes/integration_mirror.md` to `sairedis/vslib/SwitchBCM56860HW.h` / `SwitchBCM56860HW.cpp`
   (include `SwitchBCM56860HW_mirror.inc.h` as the last line inside the class, dispatch, hwSetPort hook, the call to
   hwAclMirrorSessionUpdated from the mirror code as described in notes/integration_acl.md 1.7).
2. `/home/kaguyou/homelab/sonic-sairedis/vslib/Makefile.am`: add `SwitchBCM56860HW_mirror.cpp` after the qos .cpp line and
   `hwbcm/hwbcm_mirror.c` after the hwbcm qos line.
3. Copy all changed/new files from `sai-td2p/sairedis/vslib/` to the same paths under `/home/kaguyou/homelab/sonic-sairedis/vslib/`.
4. Build: `cd /home/kaguyou/homelab/sai-td2p && ./deploy/build.sh --no-meta > /tmp/agy_build_mirror.log 2>&1; grep -E "error:|ALL CHECKS|FAIL" /tmp/agy_build_mirror.log`
   — fix until `==> ALL CHECKS PASSED` (C++ is -Werror).
5. Report to `notes/agy_report_mirror_wiring.md` (changes file:line, sha256 of deploy/build/libsaivs.so.0.0.0, fixes).
Do NOT: ssh/scp to the switch (192.168.52.1 / 192.168.0.2), /dev/ttyUSB0, deploy/install.sh, sairedis/sync_from_sairedis.sh,
edit notes/design_warmboot.md or DESIGN.md/README.md, git commit/push.
