#!/bin/bash
# Build the standalone hwbcm exerciser (static) inside the saibuild container.
# Host paths: ~/homelab is mounted at /w in the container.
set -euo pipefail

HOST_OUT="$(cd "$(dirname "$0")" && pwd)/hwtest"

podman exec saibuild bash -euo pipefail -c '
cd /w
SDK=/w/openbcm-src/sdk-6.5.27
LIBDIR=$SDK/build/unix-user/x86-smp_generic_64-2_6
HW=/w/sonic-sairedis/vslib/hwbcm
OUT=/w/sai-td2p/hwtest
TMP=$(mktemp -d /tmp/hwtest.XXXXXX)

# sdk_cflags.txt is a raw capture: drop the dangling -f, the KBUILD_* defines
# with literal quotes and the badly quoted platform string (re-added below).
# BCM_HIDE_DISPATCHABLE is removed so bcm_* prototypes are visible everywhere.
CFLAGS=$(tr " " "\n" < /w/sai-td2p/sdk_cflags.txt \
    | grep -v "^-f$" | grep -v KBUILD | grep -v BCM_PLATFORM_STRING \
    | grep -v BCM_HIDE_DISPATCHABLE | sort -u | tr "\n" " ")
INCS=$(cat /w/sai-td2p/sdk_incs.txt)
COMMON="-std=gnu99 -fPIC -Wall -DBCM_PLATFORM_STRING=\"X86\" $CFLAGS $INCS -I$HW"

echo "== compile"
gcc -c $COMMON $HW/hwbcm_core.c -o $TMP/hwbcm_core.o
gcc -c $COMMON $HW/hwbcm_l2.c   -o $TMP/hwbcm_l2.o
gcc -c $COMMON $OUT/hwtest.c    -o $TMP/hwtest.o

# Link list: names relative to LIBDIR, already wrapped in --start-group/--end-group.
LIBS=""
for w in $(cat /w/as5812-bdeshim/sdk_link_list.txt); do
    case "$w" in
        -Wl,*|-l*) LIBS="$LIBS $w" ;;
        *)         LIBS="$LIBS $LIBDIR/$w" ;;
    esac
done

echo "== link"
LINK="gcc -static -o $OUT/hwtest $TMP/hwtest.o $TMP/hwbcm_core.o $TMP/hwbcm_l2.o $LIBS -lpthread -lrt -lm -ldl"
echo "$LINK" > $OUT/link_cmd.txt
$LINK
strip $OUT/hwtest
ls -la $OUT/hwtest
rm -rf $TMP
'
echo "built: $HOST_OUT"
