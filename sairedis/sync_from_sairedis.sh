#!/bin/bash
# Regenerate this directory from the working tree of ~/homelab/sonic-sairedis (sonic-net/sonic-sairedis,
# branch 202505). Tracked-file changes become vslib.patch; new files are copied verbatim.
#   sairedis/sync_from_sairedis.sh [path-to-sonic-sairedis]
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=${1:-$HOME/homelab/sonic-sairedis}
cd "$SRC"
git rev-parse HEAD > "$HERE/BASE_COMMIT"
git diff -- vslib > "$HERE/vslib.patch"
rm -rf "$HERE/vslib"
mkdir -p "$HERE/vslib/hwbcm"
cp vslib/SwitchBCM56860HW.h vslib/SwitchBCM56860HW.cpp vslib/SwitchBCM56860HW_l3.cpp vslib/SwitchBCM56860HW_vxlan.cpp "$HERE/vslib/"
cp vslib/hwbcm/*.c vslib/hwbcm/*.h "$HERE/vslib/hwbcm/"
echo "base $(cat "$HERE/BASE_COMMIT")"; wc -l "$HERE/vslib.patch"; find "$HERE/vslib" -type f | sort
