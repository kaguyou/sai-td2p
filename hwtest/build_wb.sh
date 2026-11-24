#!/bin/bash
# Build the standalone warm-boot hwbcm exerciser (hwtest_wb) inside saibuild.
set -euo pipefail

HOST_OUT="$(cd "$(dirname "$0")" && pwd)/hwtest_wb"

podman exec saibuild bash -euo pipefail -c '
cd /w
SDK=/w/openbcm-src/sdk-6.5.27-wb
LIBDIR=$SDK/build/unix-user/x86-smp_generic_64-2_6
HW=/w/sonic-sairedis-wb/vslib/hwbcm
OUT=/w/sai-td2p-wb/hwtest
TMP=$(mktemp -d /tmp/hwtest_wb.XXXXXX)

CFLAGS=$(cat /w/sai-td2p-wb/sdk_cflags_wb.txt)
INCS=$(cat /w/sai-td2p-wb/sdk_incs_wb.txt)
COMMON="-std=gnu99 -fPIC -Wall $CFLAGS $INCS -I$HW"

echo "== compile hwtest_wb"
gcc -c $COMMON $HW/hwbcm_core.c  -o $TMP/hwbcm_core.o
gcc -c $COMMON $HW/hwbcm_l2.c    -o $TMP/hwbcm_l2.o
gcc -c $COMMON $HW/hwbcm_l3.c    -o $TMP/hwbcm_l3.o
gcc -c $COMMON $HW/hwbcm_vxlan.c -o $TMP/hwbcm_vxlan.o
gcc -c $COMMON $OUT/hwtest_wb.c  -o $TMP/hwtest_wb.o

LIBS=""
for w in $(cat /w/as5812-bdeshim/sdk_link_list_wb.txt); do
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
