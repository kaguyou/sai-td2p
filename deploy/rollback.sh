#!/bin/bash
# rollback.sh - restore the vendor Broadcom SAI (libsai.so.1.vendor) and the vendor sai.profile.
#
# Run on the PC:
#     deploy/rollback.sh [--stop] [--remove-lanemap]
#
#   --stop             stop swss/syncd on the switch first (default: refuse if running)
#   --remove-lanemap   also delete lanemap.ini from the hwsku dir (kept by default, harmless)
#
# Backups (.vendor files) are never deleted, so rollback can be run any number of times.
set -euo pipefail
. "$(dirname "$0")/common.sh"

FLAGS=()
while [ $# -gt 0 ]; do
    case "$1" in
        --stop|--remove-lanemap) FLAGS+=("$1") ;;
        -h|--help) sed -n '2,10p' "$0"; exit 0 ;;
        *) die "unknown option $1" ;;
    esac
    shift
done

pick_target
log "running rollback steps on the switch"
rssh_script "$DEPLOY_DIR/remote_rollback.sh" "${FLAGS[@]}"

print_start_howto
