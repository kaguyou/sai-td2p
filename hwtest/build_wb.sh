#!/bin/bash
# Build the standalone warm-boot hwbcm exerciser (hwtest_wb) inside saibuild.
# hwbcm sources come from this checkout (sairedis/vslib/hwbcm), not from a sonic-sairedis tree.
#   env SDK_DIR    warm-boot SDK, default $HOST_ROOT/openbcm-src/sdk-6.5.27-wb
#   env LINK_LIST  default $HOST_ROOT/as5812-bdeshim/sdk_link_list_wb.txt
# (PC paths under HOST_ROOT=~/homelab, mounted as /w in saibuild; sdk_incs_wb.txt was captured from
#  /w/openbcm-src/sdk-6.5.27-wb, so a different SDK_DIR also needs its own include capture)
set -euo pipefail

HOST_ROOT=${HOST_ROOT:-/home/kaguyou/homelab}
REPO="$(cd "$(dirname "$0")/.." && pwd -P)"
HOST_OUT="$REPO/hwtest/hwtest_wb"
to_ct() {
    local p; p=$(realpath -m "$1")
    case "$p" in
        "$HOST_ROOT"/*) echo "/w${p#"$HOST_ROOT"}" ;;
        *) echo "ERROR: $p is not under $HOST_ROOT (only that is mounted in saibuild)" >&2; return 1 ;;
    esac
}
CT_REPO=$(to_ct "$REPO") || exit 1
CT_SDK=$(to_ct "${SDK_DIR:-$HOST_ROOT/openbcm-src/sdk-6.5.27-wb}") || exit 1
CT_LINK_LIST=$(to_ct "${LINK_LIST:-$HOST_ROOT/as5812-bdeshim/sdk_link_list_wb.txt}") || exit 1

podman exec -e REPO="$CT_REPO" -e SDK="$CT_SDK" -e LINK_LIST="$CT_LINK_LIST" saibuild bash -euo pipefail -c '
cd /w
LIBDIR=$SDK/build/unix-user/x86-smp_generic_64-2_6
HW=$REPO/sairedis/vslib/hwbcm
OUT=$REPO/hwtest
TMP=$(mktemp -d /tmp/hwtest_wb.XXXXXX)

CFLAGS=$(cat $REPO/sdk_cflags_wb.txt)
INCS=$(cat $REPO/sdk_incs_wb.txt)
COMMON="-std=gnu99 -fPIC -Wall $CFLAGS $INCS -I$HW"

echo "== compile hwtest_wb"
gcc -c $COMMON $HW/hwbcm_core.c  -o $TMP/hwbcm_core.o
gcc -c $COMMON $HW/hwbcm_l2.c    -o $TMP/hwbcm_l2.o
gcc -c $COMMON $HW/hwbcm_l3.c    -o $TMP/hwbcm_l3.o
gcc -c $COMMON $HW/hwbcm_vxlan.c -o $TMP/hwbcm_vxlan.o
gcc -c $COMMON $OUT/hwtest_wb.c  -o $TMP/hwtest_wb.o

LIBS=""
for w in $(cat $LINK_LIST); do
    case "$w" in
        -Wl,*|-l*) LIBS="$LIBS $w" ;;
        *)         LIBS="$LIBS $LIBDIR/$w" ;;
    esac
done

echo "== link hwtest_wb"
gcc -o $OUT/hwtest_wb $TMP/hwtest_wb.o $TMP/hwbcm_core.o $TMP/hwbcm_l2.o $TMP/hwbcm_l3.o $TMP/hwbcm_vxlan.o $LIBS -lpthread -lrt -lm -ldl
strip $OUT/hwtest_wb
ls -la $OUT/hwtest_wb
rm -rf $TMP
'
echo "built: $HOST_OUT"
