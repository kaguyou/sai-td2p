#!/bin/bash
# build.sh - build libsaivs.so.0.0.0 (sonic-sairedis vslib + hwbcm + static OpenBCM SDK)
#            inside the `saibuild` podman container, then sanity-check the result.
#            COLD-ONLY variant (sdk-6.5.27, no BCM_WARM_BOOT_SUPPORT). The default variant going
#            forward is deploy/build_wb.sh (warm-boot SDK sdk-6.5.27-wb). Both write
#            deploy/build/libsaivs.so.0.0.0 (last one wins, see libsaivs.so.0.0.0.variant); this
#            script writes its nm lists as nm-*-cold.txt (untracked), the tracked nm-*.txt are build_wb.sh's.
#
# Run on the PC:
#     deploy/build.sh [-j N] [--dry-run] [--check-only] [--force-archive] [--no-meta]
#
#   -j N             make parallelism (default 12)
#   --dry-run        `make -n` only: nothing compiled or linked, merged archive not written
#   --check-only     skip building, only run the nm/ldd checks on vslib/.libs/libsaivs.so.0.0.0
#                    and copy it to deploy/build/libsaivs.so.0.0.0
#   --force-archive  rebuild the merged SDK archive even if it looks up to date
#   --no-meta        do not run `make -C meta` first
#   env C_STD        extra CFLAGS for the C (hwbcm) objects, default "-std=gnu99"
#   env SAI_TD2P_DIR this repo (sdk_cflags.txt, sdk_incs.txt, deploy/build/), default = the
#                    checkout this script lives in
#   env SAIREDIS_DIR configured sonic-sairedis tree with sairedis/ applied, default $HOST_ROOT/sonic-sairedis
#   env SDK_DIR      cold OpenBCM SDK, default $HOST_ROOT/openbcm-src/sdk-6.5.27
#   env LINK_LIST    SDK link list, default $HOST_ROOT/as5812-bdeshim/sdk_link_list.txt
#   All paths are PC paths under $HOST_ROOT (the only directory mounted in saibuild, as /w).
#
# How the SDK gets into the .so
# -----------------------------
#   ar -M  (CREATE deploy/build/libopenbcm_td2p.a; ADDLIB <each .a of as5812-bdeshim/sdk_link_list.txt>; SAVE)
#   make -C /w/sonic-sairedis/meta -j12
#   make -C /w/sonic-sairedis/vslib -j12 libsaivs.la CFLAGS="-g -O2 -std=gnu99" \
#        LIBS="-Wl,<sdk>/platform_defines.o -Wl,/w/sai-td2p/deploy/build/libopenbcm_td2p.a -Wl,-lm -Wl,-lrt -Wl,-ldl -Wl,-lpthread libSaiVS.a -lpthread -lrt -lm -ldl"
#
# * vslib/Makefile.am links libsaivs.la as
#     $(libsaivs_la_LINK) -rpath $(libdir) $(libsaivs_la_OBJECTS) $(libsaivs_la_LIBADD) $(LIBS)
#   and never defines libsaivs_la_LDFLAGS, so only LIBS= can be overridden from the command line
#   (verified with make -n). LIBS lands after libsaivs_la_LIBADD (= after libSaiVS.a).
# * libtool rewrites -Wl,* flags into $compiler_flags and emits them after all deplibs, so
#   -Wl,--start-group/--end-group would become an empty group (verified with libtool -n). The SDK
#   archives have circular dependencies, so all of them are merged into ONE archive: GNU ld rescans
#   a single archive until no new member is pulled, which gives group semantics.
# * libtool silently DROPS a plain /path/libfoo.a given in LIBS when the output is a shared library
#   (found by the lead on the first real build: 0 bcm_* defined). Prefixing the two files with -Wl,
#   makes libtool hand them to the linker untouched, after libSaiVS.a, which is the right spot.
#   libSaiVS.a is repeated in LIBS so application symbols the SDK wants (bde, bde_create...) are
#   resolvable regardless of pull order (libtool may dedupe it; harmless).
# * CFLAGS on the make line only adds -std=gnu99 to the hwbcm C objects: automake's per-target rule
#   is $(libSaiVS_a_CFLAGS) $(CFLAGS), and A2's libSaiVS_a_CFLAGS carries the SDK defines/includes.
# * SDK_CFLAGS (sdk_cflags.txt + sdk_incs.txt of this checkout) is passed on the make line too:
#   vslib/Makefile.am's default is the warm-boot flag set (sdk_*_wb.txt). A flags stamp in
#   vslib/hwbcm forces the hwbcm objects to be recompiled when the tree was last built with other
#   SDK flags (e.g. by build_wb.sh in the same sonic-sairedis tree).
# * The `tests` program in vslib/Makefile.am would need the SDK as well; it is never built here.
set -euo pipefail

HOST_ROOT=${HOST_ROOT:-/home/kaguyou/homelab}   # mounted as /w inside saibuild
CT_ROOT=/w
CT_NAME=saibuild
# include paths inside sdk_cflags.txt/sdk_incs.txt were captured from this SDK root (rewritten when SDK_DIR differs)
CAPTURED_SDK=$CT_ROOT/openbcm-src/sdk-6.5.27

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
    CT_SDK=$(to_ct "${SDK_DIR:-$HOST_ROOT/openbcm-src/sdk-6.5.27}") || exit 1
    CT_LINK_LIST=$(to_ct "${LINK_LIST:-$HOST_ROOT/as5812-bdeshim/sdk_link_list.txt}") || exit 1
    exec podman exec -e HOST_ROOT="$HOST_ROOT" -e C_STD="${C_STD:--std=gnu99}" \
        -e SAI_TD2P_DIR="$CT_SAI_TD2P" -e SAIREDIS_DIR="$CT_SAIREDIS" -e SDK_DIR="$CT_SDK" -e LINK_LIST="$CT_LINK_LIST" \
        "$CT_NAME" bash "$CT_SAI_TD2P/deploy/build.sh" --inner "$@"
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
        -h|--help) sed -n '2,24p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

# container paths, set by the outer half
SAI_TD2P=${SAI_TD2P_DIR:?run deploy/build.sh from the PC, not with --inner}
SR=${SAIREDIS_DIR:?}
SDK=${SDK_DIR:?}
LINK_LIST=${LINK_LIST:?}
SDKB=$SDK/build/unix-user/x86-smp_generic_64-2_6
OUT=$SAI_TD2P/deploy/build
CFLAGS_FILE=$SAI_TD2P/sdk_cflags.txt
INCS_FILE=$SAI_TD2P/sdk_incs.txt
MERGED=$OUT/libopenbcm_td2p.a
BUILT_SO=$SR/vslib/.libs/libsaivs.so.0.0.0
SO=$OUT/libsaivs.so.0.0.0

# target = SONiC 202505, Debian 12 (bookworm): glibc 2.36, libstdc++ 12
TARGET_GLIBC_MAX=2.36
TARGET_GLIBCXX_MAX=3.4.30
TARGET_CXXABI_MAX=1.3.13

die() { echo "ERROR: $*" >&2; exit 1; }
log() { echo "==> $*"; }
to_host() { echo "${1/#$CT_ROOT/$HOST_ROOT}"; }

[ -d "$SR/vslib" ] || die "$SR/vslib not found (am I inside $CT_NAME?)"
[ -f "$LINK_LIST" ] || die "$LINK_LIST not found"
[ -d "$SDKB" ] || die "$SDKB not found (SDK not built yet?)"
[ -f "$CFLAGS_FILE" ] || die "$CFLAGS_FILE not found"
[ -f "$INCS_FILE" ] || die "$INCS_FILE not found"
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
[ $missing = 0 ] || die "SDK build incomplete (see above). Finish the SDK build first."
log "${#SDK_LIBS[@]} unique SDK archives from $(to_host "$LINK_LIST"), all present in $(to_host "$SDKB")"

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
            log "[dry-run] would (re)build merged SDK archive $MERGED with ar -M (${#SDK_LIBS[@]} ADDLIB lines)"
        else
            log "building merged SDK archive $(to_host "$MERGED")"
            rm -f "$MERGED.tmp"
            make_mri | ar -M
            ranlib "$MERGED.tmp"
            mv -f "$MERGED.tmp" "$MERGED"
            echo "   $(stat -c %s "$MERGED") bytes, $(ar t "$MERGED" | wc -l) members"
        fi
    else
        log "merged SDK archive up to date: $(to_host "$MERGED")"
    fi
fi

# ------------------------------------------------------------------- build --
CFLAGS_BASE=$(sed -n 's/^CFLAGS = *//p' "$SR/vslib/Makefile" | head -1)
CFLAGS_VALUE="$CFLAGS_BASE $C_STD"
# -Wl,-lm ... after the archive: libtool emits plain -lm before the -Wl, items, so the SDK's libm
# references would stay unbound (unversioned exp/log/pow/sqrt, libm not in NEEDED) without this.
LIBS_VALUE="-Wl,$SDKB/platform_defines.o -Wl,$MERGED -Wl,-lm -Wl,-lrt -Wl,-ldl -Wl,-lpthread libSaiVS.a -lpthread -lrt -lm -ldl"
SDK_CFLAGS_VALUE="$(cat "$CFLAGS_FILE") $(cat "$INCS_FILE")"
if [ "$SDK" != "$CAPTURED_SDK" ]; then
    SDK_CFLAGS_VALUE=$(printf '%s' "$SDK_CFLAGS_VALUE" | sed -E "s#${CAPTURED_SDK//./\\.}(/| |\$)#$SDK\1#g")
fi

# hwbcm/*.o must never be stale objects built with other SDK flags (build_wb.sh, warm-boot SDK, may
# use the same sonic-sairedis tree). Same stamp file and signature scheme as build_wb.sh.
FLAGS_STAMP="$SR/vslib/hwbcm/.sdk_cflags_stamp"
FLAGS_SIG=$(printf '%s' "$SDK_CFLAGS_VALUE" | sha256sum | awk '{print $1}')
if [ $CHECK_ONLY = 0 ] && [ $DRY = 0 ]; then
    if [ ! -f "$FLAGS_STAMP" ] || [ "$(cat "$FLAGS_STAMP")" != "$FLAGS_SIG" ]; then
        log "SDK flags changed since the last build in $(to_host "$SR"): recompiling hwbcm/*.o (cold SDK flags)"
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
    log "make -C $SR/vslib -j$JOBS libsaivs.la CFLAGS=\"$CFLAGS_VALUE\" LIBS=\"$LIBS_VALUE\""
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
printf 'variant=cold\nsdk=%s\nsairedis=%s\nsha256=%s\n' "$(to_host "$SDK")" "$(to_host "$SR")" "$(cat "$SO.sha256")" > "$SO.variant"
log "result"
echo "   built:     $(to_host "$BUILT_SO")"
echo "   copy:      $(to_host "$SO")"
echo "   size:      $(stat -c %s "$SO") bytes ($(du -h "$SO" | cut -f1))"
echo "   sha256:    $(cat "$SO.sha256")"
echo "   variant:   cold ($(to_host "$SO.variant"))"
echo "   soname:    $(readelf -d "$SO" | awk '/SONAME/{print $5}')"

# ------------------------------------------------------------------ checks --
# NOTE: no `printf ... | grep -q` here: with pipefail, grep -q quitting early sends printf SIGPIPE and the
# pipeline reports failure although the symbol was found (this is what produced a bogus "not exported"
# once the library had 32k bcm_* symbols). Lookups go through associative arrays instead.
FAIL=0
fail() { echo "   FAIL: $*"; FAIL=1; }
pass() { echo "   ok:   $*"; }
warn() { echo "   WARN: $*"; }

NM_UNDEF="$OUT/nm-undefined-cold.txt"
NM_DEF="$OUT/nm-defined-cold.txt"
nm -D --undefined-only "$SO" > "$NM_UNDEF"
nm -D --defined-only "$SO" > "$NM_DEF"
echo "   nm output saved: $(to_host "$NM_UNDEF"), $(to_host "$NM_DEF")"

log "check: undefined symbols (nm -D --undefined-only)"
# columns: "U sym" or "w sym" (weak). Keep strong undefined only.
mapfile -t UNDEF < <(awk '$1=="U"{print $2}' "$NM_UNDEF")
echo "   ${#UNDEF[@]} strong undefined symbols"
# anything that smells like SDK/hwbcm must not be undefined (would mean the SDK did not link in)
SDK_RE='^(_?bcm_|_?soc_|_?sal_|BCM_|SOC_|SAL_|sh_process_command|sysconf_|bde$|bde_|linux_bde|_?knet|hwbcm_|phymod_|portmod_|cint_|_?shr_|sal$|var_(set|get|unset)$|diag_)'
# plain libm names: resolved from libm.so.6 in the syncd process even when nm shows them unversioned
LIBM_RE='^(exp|exp2|expf|log|log2|log10|logf|pow|powf|sqrt|sqrtf|cbrt|floor|ceil|round|trunc|fmod|fabs|ldexp|frexp|modf|sin|cos|tan|atan|atan2|asin|acos|sinh|cosh|tanh|hypot|lround|lrint|rint|nearbyint|copysign|__.*_finite)$'
bad=(); others=()
for s in "${UNDEF[@]}"; do
    base="${s%%@*}"
    if [[ "$base" =~ $SDK_RE ]]; then bad+=("$s"); continue; fi
    case "$s" in
        *@*) ;;                                                      # versioned: glibc / libstdc++ / libm
        _Z*) ;;                                                      # C++ mangled: swsscommon / saimeta (transfer_attributes...) / syncd
        redis*|sai_metadata*|sai_serialize*|sai_deserialize*) ;;   # hiredis, libsaimetadata (loaded by syncd)
        rtnl_*|nl_*) ;;                                              # libnl-3/libnl-route-3 (dependency of libswsscommon)
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
nbcm=0; nhw=0
while read -r _addr type sym _rest; do
    [ -n "$sym" ] || continue
    DEFSET["$sym"]=$type
    case "$sym" in bcm_*) nbcm=$((nbcm+1)) ;; hwbcm_*) nhw=$((nhw+1)) ;; esac
done < "$NM_DEF"
echo "   ${#DEFSET[@]} defined dynamic symbols"
for s in sai_api_initialize sai_api_query sai_api_uninitialize sai_log_set; do
    if [ -n "${DEFSET[$s]:-}" ]; then pass "$s exported (${DEFSET[$s]})"; else fail "$s not exported"; fi
done
if [ "$nbcm" -gt 0 ]; then pass "$nbcm bcm_* symbols defined (SDK is inside the .so)"; else fail "no bcm_* symbols defined: SDK archive was not linked in"; fi
if [ "$nhw" -gt 0 ]; then pass "$nhw hwbcm_* symbols defined"; else warn "no hwbcm_* symbols defined (hwbcm sources not in Makefile.am yet?)"; fi
if [ -n "${DEFSET[bde_create]:-}" ]; then pass "bde_create defined"; else warn "bde_create not defined (SDK needs it from the application)"; fi
if [ -n "${DEFSET[bde]:-}" ]; then pass "bde defined (${DEFSET[bde]})"; else warn "bde not defined (SDK needs it from the application)"; fi

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
