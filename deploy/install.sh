#!/bin/bash
# install.sh - push libsaivs.so to the AS5812-54T and wire it in as syncd's libsai.so.1.
#
# Run on the PC:
#     deploy/install.sh [--lib PATH] [--stop] [--skip-load-test] [--devnode-only]
#
#   --lib PATH         library to install (default: deploy/build/libsaivs.so.0.0.0 from build.sh)
#   --stop             stop swss/syncd on the switch if they are running (default: refuse)
#   --skip-load-test   skip the LD_BIND_NOW load test in a throw-away syncd container
#   --devnode-only     only (re)create /dev/linux-user-bde (use after a reboot), nothing else
#
# Steps on the switch (see remote_install.sh for the details):
#   /dev/linux-user-bde -> container stopped? -> load test -> backup vendor libsai.so.1 as
#   libsai.so.1.vendor (container + /home/admin/sai-td2p/backup) -> our lib as
#   /usr/lib/libsaivs.so.0.0.0 + symlink /usr/lib/libsai.so.1 -> libsaivs.so.0.0.0 (vendor libsai.so.1.0
#   untouched) -> hwsku: sai.profile.vendor backup, lanemap.ini, extended sai.profile
#   -> platform dir: deploy/platform/warm-reboot_plugin as warm-reboot_plugin + fast-reboot_plugin.
# It does NOT start syncd; it prints the start/watch commands at the end.
set -euo pipefail
. "$(dirname "$0")/common.sh"

LIB="$DEPLOY_DIR/build/libsaivs.so.0.0.0"
FLAGS=()
DEVNODE_ONLY=0
while [ $# -gt 0 ]; do
    case "$1" in
        --lib) LIB="$2"; shift ;;
        --stop|--skip-load-test) FLAGS+=("$1") ;;
        --devnode-only) FLAGS+=("$1"); DEVNODE_ONLY=1 ;;
        -h|--help) sed -n '2,18p' "$0"; exit 0 ;;
        *) die "unknown option $1" ;;
    esac
    shift
done

pick_target
rssh "mkdir -p $REMOTE_DIR/backup"

if [ $DEVNODE_ONLY = 1 ]; then
    rssh_script "$DEPLOY_DIR/remote_install.sh" /dev/null /dev/null /dev/null --devnode-only
    exit 0
fi

[ -f "$LIB" ] || die "$LIB not found (run deploy/build.sh first, or pass --lib)"
LOCAL_SHA=$(sha256_of "$LIB")
log "uploading $LIB ($(du -h "$LIB" | cut -f1), sha256 $LOCAL_SHA)"
rscp_to "$LIB" "$REMOTE_DIR/libsaivs.so.upload"
REMOTE_SHA=$(rssh "sha256sum $REMOTE_DIR/libsaivs.so.upload | awk '{print \$1}'")
[ "$REMOTE_SHA" = "$LOCAL_SHA" ] || die "upload corrupted (sha256 mismatch)"
rssh "mv -f $REMOTE_DIR/libsaivs.so.upload $REMOTE_DIR/libsaivs.so && echo $LOCAL_SHA > $REMOTE_DIR/libsaivs.so.sha256"

log "uploading helper files"
rscp_to "$DEPLOY_DIR/gen_lanemap.py" "$REMOTE_DIR/gen_lanemap.py"
rscp_to "$DEPLOY_DIR/sai.profile.td2p.extra" "$REMOTE_DIR/sai.profile.td2p.extra"
rscp_to "$DEPLOY_DIR/platform/warm-reboot_plugin" "$REMOTE_DIR/warm-reboot_plugin"

QOS_FLAGS=()
if [ -d "$DEPLOY_DIR/ref/hwsku-qos" ]; then
    rssh "mkdir -p $REMOTE_DIR/hwsku-qos"
    for qf in "$DEPLOY_DIR"/ref/hwsku-qos/*; do
        [ -f "$qf" ] && rscp_to "$qf" "$REMOTE_DIR/hwsku-qos/$(basename "$qf")"
    done
    QOS_FLAGS=(--qos-dir "$REMOTE_DIR/hwsku-qos")
fi

log "running install steps on the switch"
rssh_script "$DEPLOY_DIR/remote_install.sh" \
    "$REMOTE_DIR/libsaivs.so" "$REMOTE_DIR/sai.profile.td2p.extra" "$REMOTE_DIR/gen_lanemap.py" "${FLAGS[@]}" \
    --plugin "$REMOTE_DIR/warm-reboot_plugin" "${QOS_FLAGS[@]}"

print_start_howto
echo "Rollback at any time:  deploy/rollback.sh [--stop]"
