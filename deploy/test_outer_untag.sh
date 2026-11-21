#!/bin/bash
# test_outer_untag.sh - test TD2+ EGR_DVP_ATTRIBUTE.EN_EFILTER modes on network
# VPs so the VXLAN outer frame leaves the underlay port UNTAGGED (no rebuild).
#
# Background (sai-td2p/DESIGN.md, section "VXLAN データプレーンで分かったこと"):
#   EGR_DVP_ATTRIBUTE.VXLAN__EN_EFILTER has FOUR values on TD2+:
#     0 = VP_FILTER_DISABLE   -> outer frame keeps the underlay VLAN tag (current)
#     1 = VP_FILTER_VP_GRP    -> build 11: drops copies (TDBGC3) without VP group
#     2 = VP_FILTER_HASH      -> EGR_VP_VLAN_MEMBERSHIP(vp, vlan).SP_TREE/UNTAG
#                                (earlier 15:50 test dropped because SP_TREE=0;
#                                 needs BCM_VLAN_GPORT_ADD_STP_FORWARD = 0x30)
#     3 = VP_DO_NOT_CHECK     -> skip VP check, use physical port's EGR_VLAN
#                                UT_BITMAP (flags 0x0a or 0x0b; stock Broadcom
#                                SAI uses 0x0b in mptnl_xgs_common_sipdip_tnl_add_nh)
#
# Usage (on the PC, switch reachable):
#   SWITCH_PASS=... deploy/test_outer_untag.sh show
#   SWITCH_PASS=... deploy/test_outer_untag.sh apply [vp ...]       # mode 3 (0x0a: EGRESS|VP_DO_NOT_CHECK)
#   SWITCH_PASS=... deploy/test_outer_untag.sh apply-brcm [vp ...]  # mode 3 (0x0b: INGRESS|EGRESS|VP_DO_NOT_CHECK)
#   SWITCH_PASS=... deploy/test_outer_untag.sh apply-hash [vp ...]  # mode 2 with SP_TREE=3 (vlanadd 0x7b1 + vpefilter 6)
#   SWITCH_PASS=... deploy/test_outer_untag.sh revert [vp ...]      # back to 0
#   deploy/test_outer_untag.sh check
#
# The VPs are the network (VXLAN) VP gports, e.g. from `show vxlan remotevtep`
# or `deploy/bsh "l2 show"` (GPORT=0x8000000X).  The unicast network VP and the
# BUM multicast VP are separate; apply to both if both exist.
set -euo pipefail

DEPLOY_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BSH="$DEPLOY_DIR/bsh"
DEFAULT_VPS=(0x80000001 0x80000002)
UNDERLAY_VLAN="${UNDERLAY_VLAN:-100}"

usage() { sed -n '2,28p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-1}"; }

need_switch() {
    # shellcheck source=common.sh
    . "$DEPLOY_DIR/common.sh"
    pick_target
    export SWITCH_HOST
    export SWITCH_PASS
}

dump_tables() {
    echo "--- EGR_DVP_ATTRIBUTE (changed entries; EN_EFILTER is the field of interest) ---"
    "$BSH" "d chg EGR_DVP_ATTRIBUTE" || true
    echo "--- EGR_VP_VLAN_MEMBERSHIP (VP, VLAN, SP_TREE, UNTAG, VALID) ---"
    "$BSH" "d chg EGR_VP_VLAN_MEMBERSHIP" || true
}

cmd_show() {
    need_switch
    dump_tables
    echo
    echo "Read EN_EFILTER in EGR_DVP_ATTRIBUTE: 0=off, 1=vp group, 2=vp-vlan hash, 3=VP_DO_NOT_CHECK (use physical port EGR_VLAN)."
    echo "If the table shows no entry for a VP, that VP has EN_EFILTER=0."
}

cmd_apply_flags() {
    local flags="$1"
    local desc="$2"
    shift 2
    need_switch
    local vps=("$@")
    [ ${#vps[@]} -gt 0 ] || vps=("${DEFAULT_VPS[@]}")
    local vp
    echo "== before =="
    dump_tables
    for vp in "${vps[@]}"; do
        echo "== hw vxlan vpefilter $vp $flags  ($desc) =="
        "$BSH" "hw vxlan vpefilter $vp $flags"
    done
    echo "== after =="
    dump_tables
    echo
    echo "Expected: EN_EFILTER=3 (VP_DO_NOT_CHECK) on the VP(s)."
    echo "Now run:  $0 check"
}

cmd_apply_hash() {
    need_switch
    local vps=("$@")
    [ ${#vps[@]} -gt 0 ] || vps=("${DEFAULT_VPS[@]}")
    local vp
    echo "== before =="
    dump_tables
    for vp in "${vps[@]}"; do
        echo "== hw vxlan vlanadd $UNDERLAY_VLAN $vp 0x7b1  (UNTAGGED|STP_FORWARD|VP_VLAN_MEMBERSHIP|DO_NOT_ADDx3) =="
        "$BSH" "hw vxlan vlanadd $UNDERLAY_VLAN $vp 0x7b1"
        echo "== hw vxlan vpefilter $vp 6  (EGRESS|VP_VLAN_MEMBERSHIP -> EN_EFILTER=2) =="
        "$BSH" "hw vxlan vpefilter $vp 6"
    done
    echo "== after =="
    dump_tables
    echo
    echo "Expected: EN_EFILTER=2 on the VP(s), and EGR_VP_VLAN_MEMBERSHIP has"
    echo "          (vp, vlan $UNDERLAY_VLAN, SP_TREE=3, UNTAG=1, VALID=1)."
    echo "Now run:  $0 check"
}

cmd_revert() {
    need_switch
    local vps=("$@")
    [ ${#vps[@]} -gt 0 ] || vps=("${DEFAULT_VPS[@]}")
    local vp
    for vp in "${vps[@]}"; do
        echo "== hw vxlan vpefilter $vp 0  (off) =="
        "$BSH" "hw vxlan vpefilter $vp 0"
    done
    dump_tables
}

cmd_check() {
    cat <<'EOF'
PC-side check (the point is whether the outer frame is now untagged):

  1. Move the VTEP address from the VLAN sub-interface to the parent:
       sudo ip addr del 10.1.0.2/24 dev enp2s0.100 2>/dev/null || true
       sudo ip addr add 10.1.0.2/24 dev enp2s0
     (the kernel vxlan2000 keeps local 10.1.0.2 and UDP 4789; no need to
      recreate it if it already exists)

  2. Watch the NIC error/otherhost counters and the vxlan device:
       ip -s -s link show enp2s0
       ip -s link show vxlan2000
     Before the fix, tagged frames landed on enp2s0.100 and enp2s0 showed
     "otherhost" growing.  When untagged works, otherhost should stop growing
     and vxlan2000 RX should grow instead.

  3. Generate traffic from the switch (serial or bsh):
       ping -I 10.200.0.1 -c 3 10.200.0.77        # remote Type-2 host
     and/or watch a BUM ARP:
       ping -I 10.200.0.1 -c 3 10.200.0.99

  4. Switch-side confirmation that nothing is dropped at egress:
       deploy/bsh "show c ge0"        # TDBGC3 should NOT grow per packet
       deploy/bsh "cstat ge0"         # optional: RDROP/TDBGC breakdown

If otherhost keeps growing or TDBGC3 increases, revert with:
  SWITCH_PASS=... deploy/test_outer_untag.sh revert
EOF
}

case "${1:-}" in
    show)       shift; cmd_show "$@" ;;
    apply)      shift; cmd_apply_flags 0x0a "EGRESS|VP_DO_NOT_CHECK -> EN_EFILTER=3" "$@" ;;
    apply-brcm) shift; cmd_apply_flags 0x0b "INGRESS|EGRESS|VP_DO_NOT_CHECK -> EN_EFILTER=3" "$@" ;;
    apply-hash) shift; cmd_apply_hash "$@" ;;
    revert)     shift; cmd_revert "$@" ;;
    check)      cmd_check ;;
    -h|--help|help) usage 0 ;;
    *) usage 1 ;;
esac
