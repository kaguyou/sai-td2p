#!/bin/bash
# remote_rollback.sh - runs ON THE SWITCH (fed over ssh by rollback.sh via `bash -s`).
#
#   bash -s -- [--stop] [--remove-lanemap]
#
# Restores the vendor Broadcom SAI and the vendor sai.profile:
#   * libsai.so.1 target file in the stopped syncd container <- /usr/lib/libsai.so.1.vendor
#     (fallback: /home/admin/sai-td2p/backup/libsai.so.1.vendor on the host)
#   * $HWSKU/sai.profile <- $HWSKU/sai.profile.vendor
# The .vendor backups are kept. lanemap.ini is left in place unless --remove-lanemap
# (it is harmless: the vendor SAI ignores it). /dev/linux-user-bde is left alone.
set -euo pipefail

STOP=0; RM_LANEMAP=0
for a in "$@"; do
    case "$a" in
        --stop) STOP=1 ;;
        --remove-lanemap) RM_LANEMAP=1 ;;
        *) echo "remote: unknown flag $a" >&2; exit 2 ;;
    esac
done

HWSKU=/usr/share/sonic/device/x86_64-accton_as5812_54t-r0/Accton-AS5812-54T
BK=/home/admin/sai-td2p/backup
CT=syncd

log() { echo "[switch] $*"; }
die() { echo "[switch] ERROR: $*" >&2; exit 1; }
sha() { sha256sum "$1" | awk '{print $1}'; }

TMP=$(mktemp -d /tmp/sai-td2p.XXXXXX)
trap 'rm -rf "$TMP"' EXIT

running=$(docker inspect -f '{{.State.Running}}' "$CT" 2>/dev/null) || die "docker container '$CT' does not exist"
if [ "$running" = "true" ]; then
    if [ $STOP = 1 ]; then
        log "stopping swss and syncd"
        sudo systemctl stop swss syncd
    else
        die "syncd container is running. Stop it first: sudo systemctl stop swss syncd   (or rerun rollback.sh with --stop)"
    fi
fi

# --- library ---------------------------------------------------------------------
# install.sh leaves: libsai.so.1 -> libsaivs.so.0.0.0, vendor libsai.so.1.0 untouched, libsai.so.1.vendor copy.
# Restore the original shape of libsai.so.1 (recorded in $BK/libsai.so.1.orig_link: a symlink target,
# default libsai.so.1.0, or REGULAR) with a tar stream, then make sure the content behind it is the
# vendor library. /usr/lib/libsaivs.so.0.0.0 cannot be deleted in a stopped container; it stays (unused).
if docker cp "$CT:/usr/lib/libsai.so.1.vendor" "$TMP/vendor" >/dev/null 2>&1; then
    src_desc="container:/usr/lib/libsai.so.1.vendor"
elif [ -f "$BK/libsai.so.1.vendor" ]; then
    cp "$BK/libsai.so.1.vendor" "$TMP/vendor"
    src_desc="$BK/libsai.so.1.vendor"
else
    die "no vendor backup found (neither in container nor $BK). Was install.sh ever run? Nothing changed."
fi
VENDOR_SHA=$(sha "$TMP/vendor")
orig_link=$(cat "$BK/libsai.so.1.orig_link" 2>/dev/null || echo libsai.so.1.0)
case "$orig_link" in libsaivs.so*) orig_link=libsai.so.1.0 ;; esac   # never point the restored link at our file

docker cp "$CT:/usr/lib/libsai.so.1" "$TMP/link" >/dev/null
cur_link=$([ -L "$TMP/link" ] && readlink "$TMP/link" || echo REGULAR)
docker cp -L "$CT:/usr/lib/libsai.so.1" "$TMP/current" >/dev/null
if [ "$cur_link" = "$orig_link" ] && [ "$(sha "$TMP/current")" = "$VENDOR_SHA" ]; then
    log "libsai.so.1 in container is already the vendor library ($([ "$orig_link" = REGULAR ] && echo 'regular file' || echo "-> $orig_link"))"
else
    mkdir -p "$TMP/stage"
    if [ "$orig_link" = REGULAR ]; then
        cp "$TMP/vendor" "$TMP/stage/libsai.so.1"; chmod 755 "$TMP/stage/libsai.so.1"
        tar -C "$TMP/stage" --owner=0 --group=0 -cf - libsai.so.1 | docker cp - "$CT:/usr/lib" >/dev/null
    else
        # restore the symlink; if the file it points to is not the vendor content (older installs
        # overwrote libsai.so.1.0 in place), put the vendor content there too
        ln -s "$orig_link" "$TMP/stage/libsai.so.1"
        tar -C "$TMP/stage" --owner=0 --group=0 -cf - libsai.so.1 | docker cp - "$CT:/usr/lib" >/dev/null
        docker cp -L "$CT:/usr/lib/libsai.so.1" "$TMP/behind" >/dev/null 2>&1 || : > "$TMP/behind"
        if [ "$(sha "$TMP/behind")" != "$VENDOR_SHA" ]; then
            tname=$(basename "$orig_link")
            cp "$TMP/vendor" "$TMP/stage/$tname"; chmod 755 "$TMP/stage/$tname"
            tar -C "$TMP/stage" --owner=0 --group=0 -cf - "$tname" | docker cp - "$CT:$(dirname "/usr/lib/$orig_link")" >/dev/null
            log "restored vendor content into /usr/lib/$orig_link"
        fi
    fi
    docker cp "$CT:/usr/lib/libsai.so.1" "$TMP/after_link" >/dev/null
    after_link=$([ -L "$TMP/after_link" ] && readlink "$TMP/after_link" || echo REGULAR)
    [ "$after_link" = "$orig_link" ] || die "verification failed: libsai.so.1 is $after_link, expected $orig_link"
    docker cp -L "$CT:/usr/lib/libsai.so.1" "$TMP/after" >/dev/null
    [ "$(sha "$TMP/after")" = "$VENDOR_SHA" ] || die "verification failed: content behind libsai.so.1 is not the vendor library"
    log "restored /usr/lib/libsai.so.1 ($([ "$orig_link" = REGULAR ] && echo 'regular file' || echo "-> $orig_link")) from $src_desc (sha256 $VENDOR_SHA)"
fi

# --- profile ---------------------------------------------------------------------
if [ -f "$HWSKU/sai.profile.vendor" ]; then
    if cmp -s "$HWSKU/sai.profile.vendor" "$HWSKU/sai.profile"; then
        log "$HWSKU/sai.profile already matches vendor profile"
    else
        sudo install -m 644 -o root -g root "$HWSKU/sai.profile.vendor" "$HWSKU/sai.profile"
        log "restored $HWSKU/sai.profile from sai.profile.vendor (backup kept)"
    fi
else
    if grep -q '^SAI_VS_' "$HWSKU/sai.profile"; then
        die "$HWSKU/sai.profile has SAI_VS_ keys but no sai.profile.vendor backup; fix by hand"
    fi
    log "no sai.profile.vendor backup; profile has no SAI_VS_ keys, leaving it"
fi
sed 's/^/   | /' "$HWSKU/sai.profile"

if [ $RM_LANEMAP = 1 ] && [ -f "$HWSKU/lanemap.ini" ]; then
    sudo rm -f "$HWSKU/lanemap.ini"
    log "removed $HWSKU/lanemap.ini"
fi

echo "date=$(date -Is) rolled back to vendor (sha256 $VENDOR_SHA)" >> "$BK/../installed.txt" 2>/dev/null || true
log "rollback complete. syncd container is still stopped."
