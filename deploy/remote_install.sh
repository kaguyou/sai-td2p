#!/bin/bash
# remote_install.sh - runs ON THE SWITCH (fed over ssh by install.sh via `bash -s`).
# Do not run by hand on the PC.
#
#   bash -s -- <lib-path-on-switch> <extra-profile-path> <gen_lanemap.py-path> [--stop] [--skip-load-test] [--devnode-only]
#              [--plugin <warm-reboot_plugin-path-on-switch>]
#
# Everything is idempotent and never deletes the vendor library:
#   1. /dev/linux-user-bde                 create (major from /proc/devices), chmod 666
#   2. syncd container must be stopped      (--stop runs `systemctl stop swss syncd`)
#   3. load test in a throw-away container  LD_PRELOAD=our lib, LD_BIND_NOW=1, `syncd -h`
#   4. libsai inside the stopped container  first run: copy vendor lib -> /usr/lib/libsai.so.1.vendor
#                                           (+ host copy in /home/admin/sai-td2p/backup), then
#                                           docker cp our lib over the file libsai.so.1 points to
#   5. hwsku dir (host, sudo)               sai.profile -> sai.profile.vendor (once), write lanemap.ini,
#                                           write sai.profile = vendor profile minus SAI_VS_* + extra keys
#   6. platform dir (host, sudo, --plugin)  warm-reboot_plugin + fast-reboot_plugin (kexec DMA guard)
set -euo pipefail

LIB="${1:?lib path}"; EXTRA="${2:?extra profile}"; GEN="${3:?gen_lanemap.py}"; shift 3
STOP=0; LOADTEST=1; DEVNODE_ONLY=0; PLUGIN=""
while [ $# -gt 0 ]; do
    case "$1" in
        --stop) STOP=1 ;;
        --skip-load-test) LOADTEST=0 ;;
        --devnode-only) DEVNODE_ONLY=1 ;;
        --plugin) PLUGIN="${2:?--plugin needs a path}"; shift ;;
        *) echo "remote: unknown flag $1" >&2; exit 2 ;;
    esac
    shift
done

PLATDIR=/usr/share/sonic/device/x86_64-accton_as5812_54t-r0
HWSKU=$PLATDIR/Accton-AS5812-54T
BK=/home/admin/sai-td2p/backup
CT=syncd
IMAGE=docker-syncd-brcm:latest

log() { echo "[switch] $*"; }
die() { echo "[switch] ERROR: $*" >&2; exit 1; }
sha() { sha256sum "$1" | awk '{print $1}'; }

TMP=$(mktemp -d /tmp/sai-td2p.XXXXXX)
trap 'rm -rf "$TMP"' EXIT

# --- 1. device node ----------------------------------------------------------
ensure_devnode() {
    local major cur
    major=$(awk '$2=="linux-user-bde"{print $1}' /proc/devices)
    [ -n "$major" ] || die "linux-user-bde not in /proc/devices: kernel module not loaded (sudo systemctl start opennsl-modules)"
    if [ -c /dev/linux-user-bde ]; then
        cur=$((16#$(stat -c %t /dev/linux-user-bde)))
        if [ "$cur" != "$major" ]; then
            log "/dev/linux-user-bde has major $cur, module has $major: recreating"
            sudo rm -f /dev/linux-user-bde
            sudo mknod /dev/linux-user-bde c "$major" 0
        fi
    else
        log "creating /dev/linux-user-bde c $major 0"
        sudo mknod /dev/linux-user-bde c "$major" 0
    fi
    sudo chmod 666 /dev/linux-user-bde
    log "$(ls -la /dev/linux-user-bde)"
    log "note: /dev is not persistent; after a reboot run install.sh --devnode-only before starting syncd"
}
ensure_devnode
[ $DEVNODE_ONLY = 1 ] && exit 0

# --- 2. container state ------------------------------------------------------
running=$(docker inspect -f '{{.State.Running}}' "$CT" 2>/dev/null) \
    || die "docker container '$CT' does not exist. Create it once with the vendor SAI: sudo systemctl start syncd; sudo systemctl stop swss syncd"
if [ "$running" = "true" ]; then
    if [ $STOP = 1 ]; then
        log "stopping swss and syncd"
        sudo systemctl stop swss syncd
    else
        die "syncd container is running. Stop it first: sudo systemctl stop swss syncd   (or rerun install.sh with --stop)"
    fi
fi
log "syncd container is stopped (Privileged=$(docker inspect -f '{{.HostConfig.Privileged}}' "$CT"))"

# --- library sanity ----------------------------------------------------------
[ -f "$LIB" ] || die "$LIB not found"
python3 - "$LIB" <<'PY' || die "$LIB is not an ELF x86-64 shared object"
import struct, sys
with open(sys.argv[1], 'rb') as f:
    h = f.read(20)
ok = h[:4] == b'\x7fELF' and h[4] == 2 and struct.unpack('<H', h[16:18])[0] == 3 and struct.unpack('<H', h[18:20])[0] == 62
sys.exit(0 if ok else 1)
PY
chmod 755 "$LIB"
NEW_SHA=$(sha "$LIB")
log "new library: $LIB ($(stat -c %s "$LIB") bytes, sha256 $NEW_SHA)"

# --- 3. load test ------------------------------------------------------------
if [ $LOADTEST = 1 ]; then
    # syncd_main() talks to redis (Logger::linkToDbNative, WarmStart) BEFORE it parses -h, so in a
    # throw-away container without redis it dies with a DB error. That is fine: with LD_BIND_NOW=1
    # the dynamic loader resolves every symbol of libsai.so.1 before main() runs, so a library
    # problem shows up as a loader error ("symbol lookup error", "undefined symbol", ...) and
    # anything else means the library loaded and bound completely.
    log "load test: fresh container from $IMAGE, 'syncd -h' with LD_BIND_NOW=1 against our lib"
    if docker run --rm --entrypoint /bin/bash -v "$LIB":/x/libsaivs.so:ro "$IMAGE" -c '
            set -e
            t=$(readlink -f /usr/lib/libsai.so.1)
            cp /x/libsaivs.so "$t"
            echo "   libsai.so.1 -> $t replaced in throw-away container"
            echo "   syncd NEEDS: $(ldd /usr/bin/syncd | grep -oE "libsai[a-z]*\.so[.0-9]*" | sort -u | tr "\n" " ")"
            if ldd /usr/lib/libsai.so.1 | grep -q "not found"; then echo "   ldd: missing libs:"; ldd /usr/lib/libsai.so.1 | grep "not found"; exit 3; fi
            echo "   ldd: all NEEDED of libsai.so.1 resolve"
            set +e
            out=$(timeout 60 env LD_BIND_NOW=1 /usr/bin/syncd -h 2>&1 >/dev/null); rc=$?
            set -e
            if echo "$out" | grep -qE "symbol lookup error|undefined symbol|cannot open shared object|error while loading shared libraries|relocation error|cannot be preloaded|wrong ELF class"; then
                echo "   LOADER ERROR:"; echo "$out" | grep -E "symbol|shared|reloc|ELF" | head -20 | sed "s/^/      /"; exit 4
            fi
            echo "   loader bound all symbols of libsai.so.1 (syncd then exited rc=$rc; a redis/DB error here is expected without redis)"
            echo "$out" | tail -2 | sed "s/^/      last stderr: /"'; then
        log "load test passed"
    else
        die "load test FAILED: the library does not load inside $IMAGE (unresolved symbols or ABI mismatch). Nothing was changed."
    fi
fi

# --- 4. install into the stopped container --------------------------------------
# Layout afterwards:  /usr/lib/libsaivs.so.0.0.0   our library (regular file)
#                     /usr/lib/libsai.so.1          symlink -> libsaivs.so.0.0.0   (what syncd NEEDs)
#                     /usr/lib/libsai.so.1.0        vendor library, untouched
#                     /usr/lib/libsai.so.1.vendor   copy of the vendor library (regular file)
# Both entries are written with one tar stream (`docker cp - CT:/usr/lib`): docker's untar removes
# an existing entry before creating the new one, so the old libsai.so.1 symlink is replaced instead
# of being followed (a plain `docker cp file CT:/usr/lib/libsai.so.1` would write THROUGH the symlink
# into libsai.so.1.0).
OURNAME=libsaivs.so.0.0.0
mkdir -p "$BK"
docker cp "$CT:/usr/lib/libsai.so.1" "$TMP/link" >/dev/null
if [ -L "$TMP/link" ]; then orig_link=$(readlink "$TMP/link"); else orig_link=REGULAR; fi
log "in container: /usr/lib/libsai.so.1 is currently $([ "$orig_link" = REGULAR ] && echo 'a regular file' || echo "-> $orig_link")"

docker cp -L "$CT:/usr/lib/libsai.so.1" "$TMP/current" >/dev/null
CUR_SHA=$(sha "$TMP/current")

if docker cp "$CT:/usr/lib/libsai.so.1.vendor" "$TMP/vendor" >/dev/null 2>&1; then
    log "vendor backup already present in container: /usr/lib/libsai.so.1.vendor (sha256 $(sha "$TMP/vendor"))"
else
    [ "$CUR_SHA" != "$NEW_SHA" ] || die "current libsai.so.1 is already this library and no vendor backup exists; refusing to continue"
    [ "$orig_link" != "$OURNAME" ] || die "libsai.so.1 already points to $OURNAME but no vendor backup exists; refusing to continue"
    log "first install: backing up vendor library -> container:/usr/lib/libsai.so.1.vendor"
    docker cp "$TMP/current" "$CT:/usr/lib/libsai.so.1.vendor" >/dev/null
    cp "$TMP/current" "$TMP/vendor"
    echo "$orig_link" > "$BK/libsai.so.1.orig_link"
fi
if [ ! -f "$BK/libsai.so.1.vendor" ]; then
    cp "$TMP/vendor" "$BK/libsai.so.1.vendor"
    log "host copy of vendor library: $BK/libsai.so.1.vendor"
fi
[ -f "$BK/libsai.so.1.orig_link" ] || echo "${orig_link/$OURNAME/libsai.so.1.0}" > "$BK/libsai.so.1.orig_link"

if [ "$orig_link" = "$OURNAME" ] && [ "$CUR_SHA" = "$NEW_SHA" ]; then
    log "container already has this exact library as /usr/lib/$OURNAME with libsai.so.1 -> $OURNAME, skipping copy"
else
    mkdir -p "$TMP/stage"
    cp "$LIB" "$TMP/stage/$OURNAME"
    chmod 755 "$TMP/stage/$OURNAME"
    ln -s "$OURNAME" "$TMP/stage/libsai.so.1"
    tar -C "$TMP/stage" --owner=0 --group=0 -cf - "$OURNAME" libsai.so.1 | docker cp - "$CT:/usr/lib" >/dev/null
    # verify: symlink shape and content
    docker cp "$CT:/usr/lib/libsai.so.1" "$TMP/after_link" >/dev/null
    [ -L "$TMP/after_link" ] && [ "$(readlink "$TMP/after_link")" = "$OURNAME" ] \
        || die "verification failed: /usr/lib/libsai.so.1 in container is not a symlink to $OURNAME (run rollback.sh)"
    docker cp -L "$CT:/usr/lib/libsai.so.1" "$TMP/after" >/dev/null
    [ "$(sha "$TMP/after")" = "$NEW_SHA" ] || die "verification failed: content behind libsai.so.1 does not match uploaded library (run rollback.sh)"
    docker cp -L "$CT:/usr/lib/libsai.so.1.vendor" "$TMP/vendor_after" >/dev/null
    [ "$(sha "$TMP/vendor_after")" = "$(sha "$TMP/vendor")" ] || die "vendor backup in container changed unexpectedly"
    log "installed /usr/lib/$OURNAME and /usr/lib/libsai.so.1 -> $OURNAME (verified sha256; vendor libsai.so.1.0 untouched)"
fi
{ echo "date=$(date -Is)"; echo "lib=$LIB"; echo "sha256=$NEW_SHA"; echo "container_file=/usr/lib/$OURNAME"; echo "symlink=/usr/lib/libsai.so.1 -> $OURNAME"; echo "orig_link=$(cat "$BK/libsai.so.1.orig_link")"; } > "$BK/../installed.txt"

# --- 5. hwsku: sai.profile + lanemap.ini (host side, read-only in container) ----
[ -f "$HWSKU/sai.profile" ] || die "$HWSKU/sai.profile missing"
if [ ! -f "$HWSKU/sai.profile.vendor" ]; then
    if grep -q '^SAI_VS_' "$HWSKU/sai.profile"; then
        die "$HWSKU/sai.profile already contains SAI_VS_ keys but no sai.profile.vendor backup exists; fix by hand"
    fi
    sudo cp -p "$HWSKU/sai.profile" "$HWSKU/sai.profile.vendor"
    log "backed up vendor profile -> $HWSKU/sai.profile.vendor"
fi
BASE="$HWSKU/sai.profile.vendor"

cfg_line=$(grep '^SAI_INIT_CONFIG_FILE=' "$BASE" || true)
cfg_ct="${cfg_line#*=}"
cfg_host="${cfg_ct/\/usr\/share\/sonic\/hwsku/$HWSKU}"
[ -f "$cfg_host" ] || die "config.bcm from profile not found on host: $cfg_host"

# RIOT (routing in/out of VXLAN VFIs) for the SVI on VNI-mapped VLANs: the
# SDK only enables it from config.bcm. Backup once, append once.
if ! grep -q '^riot_enable=1' "$cfg_host"; then
    [ -f "$cfg_host.pre-riot" ] || sudo cp -p "$cfg_host" "$cfg_host.pre-riot"
    printf '\n# sai-td2p: RIOT for the VXLAN L3 gateway (SVI on VNI-mapped VLANs)\nriot_enable=1\nriot_overlay_l3_intf_mem_size=4096\nriot_overlay_l3_egress_mem_size=16384\n' \
        | sudo tee -a "$cfg_host" >/dev/null
    log "enabled RIOT in $cfg_host (backup $cfg_host.pre-riot)"
else
    log "RIOT already enabled in $cfg_host"
fi

# stable_size (warm-boot scache size) is listed twice in the vendor config.bcm. The SDK config
# parser keeps the FIRST definition and ignores later duplicates (sal_config_file_process with
# override_duplicates=0), so 0x2000000 (32 MB) is in effect. Left alone on purpose: the scache was
# laid out with that size at the last cold boot, so changing it needs a cold boot of its own.
n_ss=$(grep -c '^stable_size=' "$cfg_host" || true)
[ "$n_ss" -le 1 ] || log "note: $cfg_host has $n_ss stable_size lines; the first ($(grep -m1 '^stable_size=' "$cfg_host" | cut -d= -f2 | cut -d' ' -f1)) wins"

# Lossy by default like the Broadcom SAI SKUs: with the SDK default
# mmu_lossless=1 PG7 is lossless and over-subscribed traffic is dropped at
# ingress (not counted as queue drops) or paused. SONiC programs lossless PGs
# itself (PFC) when a buffer config asks for them.
if ! grep -q '^mmu_lossless=' "$cfg_host"; then
    printf '# sai-td2p: lossy MMU default (SONiC buffer config adds lossless PGs)\nmmu_lossless=0\n' | sudo tee -a "$cfg_host" >/dev/null
    log "set mmu_lossless=0 in $cfg_host"
fi

python3 -I "$GEN" "$HWSKU/port_config.ini" -c "$cfg_host" -o "$TMP/lanemap.ini"
sudo install -m 644 -o root -g root "$TMP/lanemap.ini" "$HWSKU/lanemap.ini"
log "wrote $HWSKU/lanemap.ini ($(grep -vc '^#' "$HWSKU/lanemap.ini") ports)"

{ grep -v '^SAI_VS_' "$BASE" | sed '/^[[:space:]]*$/d'; grep -v '^[[:space:]]*$' "$EXTRA"; } > "$TMP/sai.profile"
if cmp -s "$TMP/sai.profile" "$HWSKU/sai.profile"; then
    log "$HWSKU/sai.profile already up to date"
else
    sudo install -m 644 -o root -g root "$TMP/sai.profile" "$HWSKU/sai.profile"
    log "wrote $HWSKU/sai.profile"
fi
sed 's/^/   | /' "$HWSKU/sai.profile"

# the lane map path in the profile is via the /usr/share/sonic/platform mount; prove it maps to the same file
lm_ct=$(grep '^SAI_VS_INTERFACE_LANE_MAP_FILE=' "$EXTRA" | cut -d= -f2)
lm_host="${lm_ct/\/usr\/share\/sonic\/platform/\/usr\/share\/sonic\/device\/x86_64-accton_as5812_54t-r0}"
lm_host="${lm_host/\/usr\/share\/sonic\/hwsku/$HWSKU}"
[ -f "$lm_host" ] || die "lane map path in profile ($lm_ct) does not resolve on host ($lm_host)"
log "lane map: container $lm_ct == host $lm_host"

# --- 6. platform warm-reboot / fast-reboot plugin (host side) --------------------
# /usr/local/bin/warm-reboot (== fast-reboot) runs ${DEVPATH}/${PLATFORM}/<name>_plugin after
# syncd/docker are stopped, right before `kexec -e`. Ours checks that no CMIC DMA engine is still
# armed (KNET rings, FIFO/SBUS/stat DMA) and falls back to rmmod knet / a cold reboot if one is.
if [ -n "$PLUGIN" ]; then
    [ -f "$PLUGIN" ] || die "plugin $PLUGIN not found"
    bash -n "$PLUGIN" || die "plugin $PLUGIN has a shell syntax error"
    for name in warm-reboot_plugin fast-reboot_plugin; do
        if cmp -s "$PLUGIN" "$PLATDIR/$name"; then
            log "$PLATDIR/$name already up to date"
        else
            if [ -e "$PLATDIR/$name" ] && [ ! -e "$PLATDIR/$name.orig" ]; then
                sudo cp -p "$PLATDIR/$name" "$PLATDIR/$name.orig"
                log "backed up existing $PLATDIR/$name -> $name.orig"
            fi
            sudo install -m 755 -o root -g root "$PLUGIN" "$PLATDIR/$name"
            log "installed $PLATDIR/$name (sha256 $(sha "$PLATDIR/$name"))"
        fi
    done
fi

log "install complete. syncd container is still stopped."
