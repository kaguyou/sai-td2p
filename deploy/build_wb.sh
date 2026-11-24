#!/bin/bash
# build_wb.sh - build warm-boot-enabled libsaivs.so.0.0.0
#               (sonic-sairedis vslib + hwbcm + static OpenBCM sdk-6.5.27-wb)
#               inside the `saibuild` podman container, then sanity-check the result.
#               This is the default variant going forward; deploy/build.sh is the cold-only
#               build against sdk-6.5.27. Both write deploy/build/libsaivs.so.0.0.0 (last one
#               wins, see libsaivs.so.0.0.0.variant); the tracked nm-*.txt lists come from here.
#
# Run on the PC:
#     deploy/build_wb.sh [-j N] [--dry-run] [--check-only] [--force-archive] [--no-meta]
#
#   -j N             make parallelism (default 12)
#   --dry-run        `make -n` only: nothing compiled or linked, merged archive not written
#   --check-only     skip building, only run the nm/ldd checks on vslib/.libs/libsaivs.so.0.0.0
#                    and copy it to deploy/build/libsaivs.so.0.0.0
#   --force-archive  rebuild the merged SDK archive even if it looks up to date
#   --no-meta        do not run `make -C meta` first
#   env C_STD        extra CFLAGS for the C (hwbcm) objects, default "-std=gnu99"
#   env SAI_TD2P_DIR this repo (sdk_cflags_wb.txt, sdk_incs_wb.txt, deploy/build/), default = the
#                    checkout this script lives in
#   env SAIREDIS_DIR configured sonic-sairedis tree with sairedis/ applied, default $HOST_ROOT/sonic-sairedis
#   env SDK_DIR      warm-boot OpenBCM SDK, default $HOST_ROOT/openbcm-src/sdk-6.5.27-wb
#   env LINK_LIST    SDK link list, default $HOST_ROOT/as5812-bdeshim/sdk_link_list_wb.txt
#   All paths are PC paths under $HOST_ROOT (the only directory mounted in saibuild, as /w).
#   SAIREDIS_DIR may be shared with deploy/build.sh: a flags stamp in vslib/hwbcm forces the hwbcm
#   objects to be recompiled whenever the SDK flags differ from the last build in that tree.
set -euo pipefail

HOST_ROOT=${HOST_ROOT:-/home/kaguyou/homelab}   # mounted as /w inside saibuild
CT_ROOT=/w
CT_NAME=saibuild
# include paths inside sdk_*_wb.txt were captured from this SDK root (rewritten when SDK_DIR differs)
CAPTURED_SDK=$CT_ROOT/openbcm-src/sdk-6.5.27-wb

# ---------------------------------------------------------------- outer (PC) --
if [ "${1:-}" != "--inner" ]; then
    to_ct() {
        local p
        p=$(realpath -m "$1")
        case "$p" in
            "$HOST_ROOT"/*) echo "$CT_ROOT${p#"$HOST_ROOT"}" ;;
            *) echo "ERROR: $p is not under $HOST_ROOT (only that is mounted in $CT_NAME)" >&2; return 1 ;;
        esac
    }
    command -v podman >/dev/null 2>&1 || { echo "ERROR: podman not found" >&2; exit 1; }
    state=$(podman inspect -f '{{.State.Running}}' "$CT_NAME" 2>/dev/null || true)
    [ "$state" = "true" ] || { echo "ERROR: container $CT_NAME is not running (podman start $CT_NAME)" >&2; exit 1; }
    CT_SAI_TD2P=$(to_ct "${SAI_TD2P_DIR:-$(dirname "$(realpath "$0")")/..}") || exit 1
    CT_SAIREDIS=$(to_ct "${SAIREDIS_DIR:-$HOST_ROOT/sonic-sairedis}") || exit 1
    CT_SDK=$(to_ct "${SDK_DIR:-$HOST_ROOT/openbcm-src/sdk-6.5.27-wb}") || exit 1
    CT_LINK_LIST=$(to_ct "${LINK_LIST:-$HOST_ROOT/as5812-bdeshim/sdk_link_list_wb.txt}") || exit 1
    # podman exec starts the inner half under conmon, not as our child: pass our
    # niceness on explicitly so `nice -n 19 deploy/build_wb.sh` really runs niced.
    exec podman exec -e HOST_ROOT="$HOST_ROOT" -e C_STD="${C_STD:--std=gnu99}" \
        -e SAI_TD2P_DIR="$CT_SAI_TD2P" -e SAIREDIS_DIR="$CT_SAIREDIS" -e SDK_DIR="$CT_SDK" -e LINK_LIST="$CT_LINK_LIST" \
        "$CT_NAME" nice -n "$(nice)" bash "$CT_SAI_TD2P/deploy/build_wb.sh" --inner "$@"
fi
shift

# ------------------------------------------------------- inner (container) --
JOBS=12
DRY=0
CHECK_ONLY=0
FORCE_ARCHIVE=0
DO_META=1
C_STD="${C_STD:--std=gnu99}"
while [ $# -gt 0 ]; do
    case "$1" in
        -j) JOBS="$2"; shift ;;
        -j*) JOBS="${1#-j}" ;;
        --dry-run) DRY=1 ;;
        --check-only) CHECK_ONLY=1 ;;
        --force-archive) FORCE_ARCHIVE=1 ;;
        --no-meta) DO_META=0 ;;
        -h|--help) sed -n '2,26p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

# container paths, set by the outer half
SAI_TD2P=${SAI_TD2P_DIR:?run deploy/build_wb.sh from the PC, not with --inner}
SR=${SAIREDIS_DIR:?}
SDK=${SDK_DIR:?}
LINK_LIST=${LINK_LIST:?}
SDKB=$SDK/build/unix-user/x86-smp_generic_64-2_6
OUT=$SAI_TD2P/deploy/build
MERGED=$OUT/libopenbcm_td2p_wb.a
BUILT_SO=$SR/vslib/.libs/libsaivs.so.0.0.0
SO=$OUT/libsaivs.so.0.0.0

CFLAGS_WB_FILE=$SAI_TD2P/sdk_cflags_wb.txt
INCS_WB_FILE=$SAI_TD2P/sdk_incs_wb.txt

# target = SONiC 202505, Debian 12 (bookworm): glibc 2.36, libstdc++ 12
TARGET_GLIBC_MAX=2.36
TARGET_GLIBCXX_MAX=3.4.30
TARGET_CXXABI_MAX=1.3.13

die() { echo "ERROR: $*" >&2; exit 1; }
log() { echo "==> $*"; }
to_host() { echo "${1/#$CT_ROOT/$HOST_ROOT}"; }

[ -d "$SR/vslib" ] || die "$SR/vslib not found (am I inside $CT_NAME?)"
[ -f "$LINK_LIST" ] || die "$LINK_LIST not found"
[ -d "$SDKB" ] || die "$SDKB not found (warm-boot SDK not built yet?)"
[ -f "$CFLAGS_WB_FILE" ] || die "$CFLAGS_WB_FILE not found"
[ -f "$INCS_WB_FILE" ] || die "$INCS_WB_FILE not found"
mkdir -p "$OUT"

# ------------------------------------------------------------ SDK archives --
# .a names from the SDK's own link line, in order, duplicates removed (ADDLIB twice would duplicate members)
mapfile -t SDK_LIBS < <(tr ' ' '\n' < "$LINK_LIST" | grep -E '\.a$' | awk '!seen[$0]++')
[ ${#SDK_LIBS[@]} -gt 0 ] || die "no .a entries in $LINK_LIST"
missing=0
for a in "${SDK_LIBS[@]}"; do
    [ -f "$SDKB/$a" ] || { echo "   missing SDK lib: $SDKB/$a" >&2; missing=1; }
done
[ -f "$SDKB/platform_defines.o" ] || { echo "   missing $SDKB/platform_defines.o" >&2; missing=1; }
[ $missing = 0 ] || die "warm-boot SDK build incomplete (see above). Finish Phase 0 first."
log "${#SDK_LIBS[@]} unique WB SDK archives from $(to_host "$LINK_LIST"), all present in $(to_host "$SDKB")"

archive_stale() {
    [ $FORCE_ARCHIVE = 1 ] && return 0
    [ -f "$MERGED" ] || return 0
    local a
    for a in "${SDK_LIBS[@]}"; do
        [ "$SDKB/$a" -nt "$MERGED" ] && return 0
    done
    [ "$LINK_LIST" -nt "$MERGED" ] && return 0
    return 1
}

make_mri() {
    echo "CREATE $MERGED.tmp"
    local a
    for a in "${SDK_LIBS[@]}"; do echo "ADDLIB $SDKB/$a"; done
    echo "SAVE"
    echo "END"
}

if [ $CHECK_ONLY = 0 ]; then
    if archive_stale; then
        if [ $DRY = 1 ]; then
            log "[dry-run] would (re)build merged WB SDK archive $MERGED with ar -M (${#SDK_LIBS[@]} ADDLIB lines)"
        else
            log "building merged WB SDK archive $(to_host "$MERGED")"
            rm -f "$MERGED.tmp"
            make_mri | ar -M
            ranlib "$MERGED.tmp"
            mv -f "$MERGED.tmp" "$MERGED"
            echo "   $(stat -c %s "$MERGED") bytes, $(ar t "$MERGED" | wc -l) members"
        fi
    else
        log "merged WB SDK archive up to date: $(to_host "$MERGED")"
    fi
fi

# ------------------------------------------------------------------- build --
CFLAGS_BASE=$(sed -n 's/^CFLAGS = *//p' "$SR/vslib/Makefile" | head -1)
CFLAGS_VALUE="$CFLAGS_BASE $C_STD"
SDK_CFLAGS_VALUE="$(cat "$CFLAGS_WB_FILE") $(cat "$INCS_WB_FILE")"
if [ "$SDK" != "$CAPTURED_SDK" ]; then
    SDK_CFLAGS_VALUE=$(printf '%s' "$SDK_CFLAGS_VALUE" | sed -E "s#${CAPTURED_SDK//./\\.}(/| |\$)#$SDK\1#g")
fi
# -Wl,-lm ... after the archive: libtool emits plain -lm before the -Wl, items, so the SDK's libm
# references would stay unbound (unversioned exp/log/pow/sqrt, libm not in NEEDED) without this.
LIBS_VALUE="-Wl,$SDKB/platform_defines.o -Wl,$MERGED -Wl,-lm -Wl,-lrt -Wl,-ldl -Wl,-lpthread libSaiVS.a -lpthread -lrt -lm -ldl"

# hwbcm/*.o must never be stale objects built with other SDK flags (deploy/build.sh, cold SDK, may
# use the same sonic-sairedis tree). Same stamp file and signature scheme as deploy/build.sh.
FLAGS_STAMP="$SR/vslib/hwbcm/.sdk_cflags_stamp"
FLAGS_SIG=$(printf '%s' "$SDK_CFLAGS_VALUE" | sha256sum | awk '{print $1}')
if [ $CHECK_ONLY = 0 ] && [ $DRY = 0 ]; then
    if [ ! -f "$FLAGS_STAMP" ] || [ "$(cat "$FLAGS_STAMP")" != "$FLAGS_SIG" ]; then
        log "SDK flags changed since the last build in $(to_host "$SR"): recompiling hwbcm/*.o (warm-boot SDK flags)"
        rm -f "$SR"/vslib/hwbcm/*.o "$SR"/vslib/libSaiVS.a
        echo "$FLAGS_SIG" > "$FLAGS_STAMP"
    fi
fi

if [ $CHECK_ONLY = 0 ]; then
    if [ $DO_META = 1 ]; then
        if [ $DRY = 1 ]; then
            log "[dry-run] skipping: make -C $SR/meta -j$JOBS"
        else
            log "make -C $SR/meta -j$JOBS"
            make -C "$SR/meta" -j"$JOBS"
        fi
    fi
    log "make -C $SR/vslib -j$JOBS libsaivs.la (with sdk_cflags_wb.txt + sdk_incs_wb.txt)"
    if [ $DRY = 1 ]; then
        make -C "$SR/vslib" -j"$JOBS" -n -W libSaiVS.a libsaivs.la \
            SDK_CFLAGS="$SDK_CFLAGS_VALUE" CFLAGS="$CFLAGS_VALUE" LIBS="$LIBS_VALUE" > "$OUT/dry-run.log" || true
        echo "   $(grep -c -- ' -c ' "$OUT/dry-run.log" || true) compile steps would run (full list: $(to_host "$OUT/dry-run.log"))"
        echo "   link line (head ... tail):"
        grep -E 'libtool.*--mode=link' "$OUT/dry-run.log" | sed -E 's/^(.{120}).*(.{420})$/   \1 ... \2/'
        log "[dry-run] done. The libtool line must end with: libSaiVS.a $LIBS_VALUE"
        exit 0
    fi
    if pgrep -f "libtool.*--mode=link.*libsaivs" >/dev/null 2>&1; then
        die "another libsaivs link is running in the container right now; retry when it is done"
    fi
    make -C "$SR/vslib" -j"$JOBS" libsaivs.la \
        SDK_CFLAGS="$SDK_CFLAGS_VALUE" CFLAGS="$CFLAGS_VALUE" LIBS="$LIBS_VALUE"
fi

[ -f "$BUILT_SO" ] || die "$BUILT_SO not found after build"
[ -s "$BUILT_SO" ] || die "$BUILT_SO is empty: a link is probably in progress in another shell. Retry later."

# ------------------------------------------------------------------ output --
cp -f "$BUILT_SO" "$SO"
ln -sfn "$(basename "$SO")" "$OUT/libsaivs.so"
ln -sfn "$(basename "$SO")" "$OUT/libsaivs.so.0"
rm -f "$OUT/libsaivs.so.sha256"
sha256sum "$SO" | awk '{print $1}' > "$SO.sha256"
printf 'variant=warmboot\nsdk=%s\nsairedis=%s\nsha256=%s\n' "$(to_host "$SDK")" "$(to_host "$SR")" "$(cat "$SO.sha256")" > "$SO.variant"
log "result"
echo "   built:     $(to_host "$BUILT_SO")"
echo "   copy:      $(to_host "$SO")"
echo "   size:      $(stat -c %s "$SO") bytes ($(du -h "$SO" | cut -f1))"
echo "   sha256:    $(cat "$SO.sha256")"
echo "   variant:   warmboot ($(to_host "$SO.variant"))"
echo "   soname:    $(readelf -d "$SO" | awk '/SONAME/{print $5}')"

# ------------------------------------------------------------------ checks --
FAIL=0
fail() { echo "   FAIL: $*"; FAIL=1; }
pass() { echo "   ok:   $*"; }
warn() { echo "   WARN: $*"; }

NM_UNDEF="$OUT/nm-undefined.txt"
NM_DEF="$OUT/nm-defined.txt"
nm -D --undefined-only "$SO" > "$NM_UNDEF"
nm -D --defined-only "$SO" > "$NM_DEF"
echo "   nm output saved: $(to_host "$NM_UNDEF"), $(to_host "$NM_DEF")"

log "check: undefined symbols (nm -D --undefined-only)"
mapfile -t UNDEF < <(awk '$1=="U"{print $2}' "$NM_UNDEF")
echo "   ${#UNDEF[@]} strong undefined symbols"
SDK_RE='^(_?bcm_|_?soc_|_?sal_|BCM_|SOC_|SAL_|sh_process_command|sysconf_|bde$|bde_|linux_bde|_?knet|hwbcm_|phymod_|portmod_|cint_|_?shr_|sal$|var_(set|get|unset)$|diag_)'
LIBM_RE='^(exp|exp2|expf|log|log2|log10|logf|pow|powf|sqrt|sqrtf|cbrt|floor|ceil|round|trunc|fmod|fabs|ldexp|frexp|modf|sin|cos|tan|atan|atan2|asin|acos|sinh|cosh|tanh|hypot|lround|lrint|rint|nearbyint|copysign|__.*_finite)$'
bad=(); others=()
for s in "${UNDEF[@]}"; do
    base="${s%%@*}"
    if [[ "$base" =~ $SDK_RE ]]; then bad+=("$s"); continue; fi
    case "$s" in
        *@*) ;;
        _Z*) ;;
        redis*|sai_metadata*|sai_serialize*|sai_deserialize*) ;;
        rtnl_*|nl_*) ;;
        *) [[ "$base" =~ $LIBM_RE ]] || others+=("$s") ;;
    esac
done
if [ ${#bad[@]} -gt 0 ]; then
    fail "${#bad[@]} SDK/hwbcm symbols undefined (SDK not linked in?):"
    printf '         %s\n' "${bad[@]}" | head -30
else
    pass "no undefined bcm_*/soc_*/sal_*/bde_*/hwbcm_* symbols"
fi
if [ ${#others[@]} -gt 0 ]; then
    warn "${#others[@]} unversioned non-C++ undefined symbols, make sure syncd's process provides them:"
    printf '         %s\n' "${others[@]}" | head -40
else
    pass "all other undefined symbols are versioned glibc/libstdc++, C++ (swsscommon/saimeta), hiredis, sai_metadata, libnl or libm"
fi
echo "   unversioned non-C++ undefined (for the record): $(awk '$1=="U" && $2 !~ /@/ && $2 !~ /^_Z/ {print $2}' "$NM_UNDEF" | tr '\n' ' ' | cut -c1-300)"

log "check: symbol version tags vs target (glibc <= $TARGET_GLIBC_MAX, GLIBCXX <= $TARGET_GLIBCXX_MAX, CXXABI <= $TARGET_CXXABI_MAX)"
ver_check() {
    local tag="$1" max="$2" have
    have=$( { grep -oE "@${tag}_[0-9.]+" "$NM_UNDEF" || true; } | sed "s/@${tag}_//" | sort -uV | tail -1)
    [ -n "$have" ] || { pass "$tag: none referenced"; return; }
    if [ "$(printf '%s\n%s\n' "$have" "$max" | sort -V | tail -1)" = "$max" ]; then
        pass "$tag max $have <= $max"
    else
        fail "$tag max $have > $max (built against a newer toolchain than the switch has)"
    fi
}
ver_check GLIBC "$TARGET_GLIBC_MAX"
ver_check GLIBCXX "$TARGET_GLIBCXX_MAX"
ver_check CXXABI "$TARGET_CXXABI_MAX"

log "check: exported symbols (nm -D --defined-only, type T/W/D/B)"
declare -A DEFSET=()
nbcm=0; nhw=0; nsync=0
while read -r _addr type sym _rest; do
    [ -n "$sym" ] || continue
    DEFSET["$sym"]=$type
    case "$sym" in
        bcm_*) nbcm=$((nbcm+1)) ;;
        hwbcm_*) nhw=$((nhw+1)) ;;
        _bcm_esw_*_sync) nsync=$((nsync+1)) ;;
    esac
done < "$NM_DEF"
echo "   ${#DEFSET[@]} defined dynamic symbols"
for s in sai_api_initialize sai_api_query sai_api_uninitialize sai_log_set; do
    if [ -n "${DEFSET[$s]:-}" ]; then pass "$s exported (${DEFSET[$s]})"; else fail "$s not exported"; fi
done
if [ "$nbcm" -gt 0 ]; then pass "$nbcm bcm_* symbols defined (SDK is inside the .so)"; else fail "no bcm_* symbols defined: SDK archive was not linked in"; fi
if [ "$nhw" -gt 0 ]; then pass "$nhw hwbcm_* symbols defined"; else warn "no hwbcm_* symbols defined (hwbcm sources not in Makefile.am yet?)"; fi
if [ -n "${DEFSET[bde_create]:-}" ]; then pass "bde_create defined"; else warn "bde_create not defined (SDK needs it from the application)"; fi
if [ -n "${DEFSET[bde]:-}" ]; then pass "bde defined (${DEFSET[bde]})"; else warn "bde not defined (SDK needs it from the application)"; fi

log "check: warm-boot SDK & hwbcm symbols"
WB_SYMS=(
    soc_scache_init
    soc_scache_recover
    soc_scache_commit
    soc_stable_set
    soc_stable_size_set
    soc_switch_stable_register
    soc_scache_hsbuf_crc32_append
    soc_knet_wb_cleanup
    soc_shutdown
    _bcm_shutdown
    hwbcm_init_ex
    hwbcm_warm_sync
    hwbcm_warm_deinit
    hwbcm_knet_warm_recover
)
for s in "${WB_SYMS[@]}"; do
    if [ -n "${DEFSET[$s]:-}" ]; then
        pass "wb symbol $s defined (${DEFSET[$s]})"
    else
        fail "wb symbol $s NOT defined in $SO"
    fi
done
if [ "$nsync" -gt 0 ]; then
    pass "$nsync _bcm_esw_*_sync warm-boot sync symbols defined"
else
    fail "no _bcm_esw_*_sync symbols defined (warm-boot SDK not linked?)"
fi

log "check: ldd"
ldd "$SO" | sed 's/^/   /'
if ldd "$SO" | grep -q 'not found'; then fail "ldd reports missing libraries"; else pass "ldd: all NEEDED libraries resolve (in the build container)"; fi
echo "   NEEDED: $(readelf -d "$SO" | awk '/NEEDED/{gsub(/[][]/,"",$5); printf "%s ", $5}')"

echo
if [ $FAIL = 0 ]; then
    log "ALL CHECKS PASSED  ->  $(to_host "$SO")"
else
    log "SOME CHECKS FAILED (see FAIL lines). Library left at $(to_host "$SO")"
    exit 1
fi
