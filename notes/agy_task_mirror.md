# Task for agy: implement port mirroring (SPAN / ERSPAN) for the custom SAI — new files only

Repo `/home/kaguyou/homelab/sai-td2p`: custom SAI for SONiC 202505 on Edgecore AS5812-54T (Broadcom Trident2+,
OpenBCM sdk-6.5.27). Implement SAI_OBJECT_TYPE_MIRROR_SESSION (SAI_MIRROR_SESSION_TYPE_LOCAL = SPAN and
ENHANCED_REMOTE = ERSPAN with GRE, src/dst IP, MAC, TTL, TOS, VLAN), port mirroring (SAI_PORT_ATTR_INGRESS_MIRROR_SESSION /
EGRESS_MIRROR_SESSION) and the ACL hook: the ACL code calls `hwAclMirrorSessionUpdated(session_oid, mirror_dest_gport)`
when a session's bcm mirror destination is created (and with -1 before it is destroyed) — see `notes/integration_acl.md` 1.7.

Follow the style of the existing features exactly (read them first):
- C layer (SDK-facing): `sairedis/vslib/hwbcm/hwbcm_acl.c/.h`, `hwbcm_copp.c/.h` — functions return 0 / negative BCM_E_*, log with hwbcm_log, unit HWBCM_UNIT.
- C++ layer: `sairedis/vslib/SwitchBCM56860HW_acl.cpp` + `SwitchBCM56860HW_acl.inc.h` (member declarations included at the end of the class in `SwitchBCM56860HW.h`).
- How SONiC uses it: `/home/kaguyou/homelab/sonic-swss/orchagent/mirrororch.cpp`, `portsorch.cpp` (port mirror), `aclorch.cpp` (MIRROR tables).
- SDK: `/home/kaguyou/homelab/openbcm-src/sdk-6.5.27/include/bcm/mirror.h` (bcm_mirror_destination_create, BCM_MIRROR_DEST_TUNNEL_IP_GRE, bcm_mirror_port_dest_add), `src/bcm/esw/mirror.c`, examples under `src/examples/xgs/**/mirror*`.
  For ERSPAN the destination port comes from the route/neighbor to the dst IP: SONiC passes SAI_MIRROR_SESSION_ATTR_MONITOR_PORT plus src/dst MAC and VLAN, so you do not need to resolve routes yourself.

Deliverables (new files only):
1. `sairedis/vslib/hwbcm/hwbcm_mirror.c` + `hwbcm_mirror.h`
2. `sairedis/vslib/SwitchBCM56860HW_mirror.cpp` + `SwitchBCM56860HW_mirror.inc.h`
3. `notes/integration_mirror.md`: exact wiring (dispatch lines in create/set/remove of SwitchBCM56860HW.cpp, hwSetPort hook for the port mirror attributes, Makefile.am lines), design, parity gaps, and a hardware test plan (SONiC `config mirror_session add ...`, `config mirror_session span add ...`, how to see mirrored frames).
4. Syntax-check both files inside the podman container `saibuild` (host ~/homelab is /w):
   C: `podman exec saibuild bash -lc 'cd /w/sai-td2p/sairedis/vslib && gcc -std=gnu99 -fsyntax-only -Wall -Wextra -Werror $(sed -e "s/ -f$//" -e "s/-DBCM_HIDE_DISPATCHABLE//" /w/sai-td2p/sdk_cflags.txt) $(cat /w/sai-td2p/sdk_incs.txt) -Ihwbcm hwbcm/hwbcm_mirror.c'`
   C++: the flags of `/w/sai-td2p/vslib_cxx_cmd.txt` + `-fsyntax-only`, run from /w/sonic-sairedis/vslib, against a COPY of SwitchBCM56860HW.h placed in your own scratch dir (put it first on -I) with `#include "SwitchBCM56860HW_mirror.inc.h"` added as the last line of the class. Must be clean (-Werror).
5. Short report in `notes/agy_report_mirror.md`.

Do NOT: edit any existing file; touch /home/kaguyou/homelab/sonic-sairedis (except reading); run deploy/build.sh or deploy/install.sh;
ssh/scp to the switch (192.168.52.1 / 192.168.0.2) or use /dev/ttyUSB0; run sairedis/sync_from_sairedis.sh; git commit/push.
