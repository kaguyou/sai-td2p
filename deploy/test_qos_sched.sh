#!/bin/bash
# test_qos_sched.sh - QoS scheduler / shaper / WRED / buffer verification helper
# for Accton AS5812-54T (BCM56864 Trident2+, non-ETS 8 cos/port, mmu_lossless=0).
#
# Applies a self-contained test overlay to one front-panel port (default Ethernet0,
# BCM logical port 1 / xe0):
#   - DSCP_TO_TC_MAP|AZURE  : DSCP 10 -> TC 1 -> UC1, DSCP 46 -> TC 5 -> UC5, DSCP 3 -> TC 3 -> UC3
#   - SCHEDULER|test_dwrr_w1_100m : DWRR weight 1, max-rate shaper 100 Mbps (pir=12500000 B/s) -> queue 1
#   - SCHEDULER|test_dwrr_w4      : DWRR weight 4 (no shaper)                                  -> queue 5
#   - WRED_PROFILE|test_wred_ecn  : green/yellow/red 100 KB..400 KB, 10% drop prob, ecn_all    -> queue 3
#
# Usage (on the PC, switch reachable; `plan` works offline without switch access):
#   deploy/test_qos_sched.sh plan   [EthernetN] [xeM] [bcm_port]
#   SWITCH_PASS=... deploy/test_qos_sched.sh apply  [EthernetN]
#   SWITCH_PASS=... deploy/test_qos_sched.sh verify [EthernetN] [xeM] [bcm_port]
#   SWITCH_PASS=... deploy/test_qos_sched.sh revert [EthernetN]
#
# SAFETY WARNING (TD2+ non-ETS mode):
#   NEVER run `cos port config`, `cos clear`, `cos config`, `cos strict|weight|drr`
#   or `cos bandwidth` in the diag shell! `cos port config` adds a gport and flips
#   the chip to ETS mode, resetting the CPU/KNET LLS tree. Use ONLY read-only
#   commands (`cos port show`, `cos bandwidth_show`, `cos discard_show`, `d chg`, `show c`).
set -euo pipefail

DEPLOY_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BSH="$DEPLOY_DIR/bsh"

IFACE="${2:-Ethernet0}"
BCM_NAME="${3:-xe0}"
BCM_PORT="${4:-1}"

usage() { sed -n '2,23p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-1}"; }

need_switch() {
    # shellcheck source=common.sh
    . "$DEPLOY_DIR/common.sh"
    pick_target
    export SWITCH_HOST
    export SWITCH_PASS
}

emit_test_json() {
    local port="$1"
    cat <<EOF
{
    "SCHEDULER": {
        "test_dwrr_w1_100m": {
            "type": "DWRR",
            "weight": "1",
            "meter_type": "bytes",
            "pir": "12500000",
            "pbs": "125000"
        },
        "test_dwrr_w4": {
            "type": "DWRR",
            "weight": "4"
        }
    },
    "WRED_PROFILE": {
        "test_wred_ecn": {
            "ecn": "ecn_all",
            "wred_green_enable": "true",
            "green_min_threshold": "104000",
            "green_max_threshold": "416000",
            "green_drop_probability": "10",
            "wred_yellow_enable": "true",
            "yellow_min_threshold": "104000",
            "yellow_max_threshold": "416000",
            "yellow_drop_probability": "10",
            "wred_red_enable": "true",
            "red_min_threshold": "104000",
            "red_max_threshold": "416000",
            "red_drop_probability": "10"
        }
    },
    "QUEUE": {
        "${port}|1": {
            "scheduler": "test_dwrr_w1_100m"
        },
        "${port}|3": {
            "wred_profile": "test_wred_ecn"
        },
        "${port}|5": {
            "scheduler": "test_dwrr_w4"
        }
    }
}
EOF
}

cmd_plan() {
    cat <<EOF
================================================================================
QoS Scheduler / Shaper / WRED / Buffer Hardware Verification Plan (TD2+ BCM56864)
Target port: ${IFACE} (BCM ${BCM_NAME}, logical port ${BCM_PORT})
================================================================================

Step 1: Verify SONiC hwsku templates and reload base QoS/buffer config
  On switch:
    ls -l /usr/share/sonic/device/x86_64-accton_as5812_54t-r0/Accton-AS5812-54T/
    sudo config qos reload
    redis-cli -n 6 HGETALL "BUFFER_MAX_PARAM_TABLE|global"   # expects mmu_size ~16777216
    journalctl -u swss -u syncd --since "2 min ago" | grep -iE "qos:|Failed|ERR"

Step 2: Apply scheduler + 100 Mbps shaper + WRED test overlay on ${IFACE}
  From PC:
    SWITCH_PASS=... deploy/test_qos_sched.sh apply ${IFACE}

  JSON overlay pushed to CONFIG_DB via \`sudo config load -y\`:
$(emit_test_json "$IFACE" | sed 's/^/    /')

Step 3: Verify ASIC programming in the OpenBCM diag shell (READ-ONLY commands!)
  From PC (or run \`SWITCH_PASS=... deploy/test_qos_sched.sh verify ${IFACE} ${BCM_NAME} ${BCM_PORT}\`):

  3a) DWRR scheduling discipline & weights (cos 1 = 1, cos 5 = 4):
      deploy/bsh "cos port show PortBitMap=${BCM_NAME}"
      # Expected:
      #   Schedule mode: deficit round-robin
      #   COSQ 1 = 1 Kbytes
      #   COSQ 5 = 4 Kbytes

  3b) Per-queue max-rate shaper (cos 1 = 100,000 kbps == 100 Mbps):
      deploy/bsh "cos bandwidth_show" | grep -E "port|${BCM_NAME} "
      # Expected for ${BCM_NAME} q=1: KbpsMin=0, KbpsMax=100000 (or nearest MTRO granularity)
      # Expected for ${BCM_NAME} q=5: KbpsMin=0, KbpsMax=0 (unshaped)

  3c) WRED / ECN profile on UC queue 3:
      deploy/bsh "cos discard_show Gport=${BCM_PORT} Queue=3"
      # Expected:
      #   GREEN  (bytes) : Min 104000, Max 416000, Gain 0, DropProb 10, Ena 1, Cap 0
      #   YELLOW (bytes) : Min 104000, Max 416000, Gain 0, DropProb 10, Ena 1, Cap 0
      #   RED    (bytes) : Min 104000, Max 416000, Gain 0, DropProb 10, Ena 1, Cap 0
      # Also verify ECN_MARKING_EN=1 and WRED_EN=1 directly in HW memory:
      deploy/bsh "d chg MMU_WRED_CONFIG_X_PIPE"
      deploy/bsh "d chg MMU_WRED_CONFIG_Y_PIPE"

  3d) Ingress PG & Egress Queue buffer thresholds (TD2+ MMU tables):
      deploy/bsh "d chg THDI_PORT_PG_CONFIG_X"
      deploy/bsh "d chg MMU_THDU_XPIPE_CONFIG_QUEUE"
      deploy/bsh "d chg MMU_THDU_XPIPE_Q_TO_QGRP_MAP"  # verify DISABLE_QUEUING=0 on active UC queues

Step 4: Data-plane verification with traffic & queue counters
  4a) Clear counters:
      sonic-clear queuecounters
      deploy/bsh "clear c ${BCM_NAME}"

  4b) 100 Mbps shaper check on UC1 (DSCP 10 -> TC 1 -> UC1):
      # Send UDP/iperf3 with TOS 0x28 (DSCP 10) at >200 Mbps exiting ${IFACE}:
      #   iperf3 -c <dst> -u -b 500M --tos 0x28 -t 10
      # Expected: receiver measures ~100 Mbps; queue 1 drops excess packets.
      show queue counters ${IFACE}
      deploy/bsh "show c ${BCM_NAME}" | grep -E "UCQ1|DROP|WRED"

  4c) DWRR weight ratio check (UC1 weight 1 vs UC5 weight 4):
      # Remove the 100 Mbps shaper on test_dwrr_w1_100m (or test with two unshaped DWRR queues
      # weight 1 vs 4) and oversubscribe ${IFACE} simultaneously with DSCP 10 (UC1) and DSCP 46 (UC5):
      #   iperf3 -c <dst> -u -b 8G --tos 0x28 -t 10 &   # DSCP 10 -> UC1 (weight 1)
      #   iperf3 -c <dst> -u -b 8G --tos 0xb8 -t 10 &   # DSCP 46 -> UC5 (weight 4)
      # Expected: UC5 gets ~4x throughput of UC1 (~80% vs ~20% of egress link).
      show queue counters ${IFACE}

  4d) WRED / ECN check on UC3 (DSCP 3 -> TC 3 -> UC3):
      # Send ECN-capable (ECT) flow with DSCP 3 (TOS 0x0e = DSCP 3 + ECT(0)) during congestion:
      # Expected: receiver sees CE marks (\`tcpdump -v 'ip[1]&3=3'\`); non-ECT sees WRED drops
      # in \`show queue counters ${IFACE}\` and \`show c ${BCM_NAME}\` (WRED_PKT / DROP).

Step 5: Revert test overlay (and verify defaults restored in diag shell)
  From PC:
    SWITCH_PASS=... deploy/test_qos_sched.sh revert ${IFACE}
    SWITCH_PASS=... deploy/test_qos_sched.sh verify ${IFACE} ${BCM_NAME} ${BCM_PORT}
EOF
}

cmd_apply() {
    need_switch
    local remote_json="/tmp/test_qos_sched_${IFACE}.json"
    log "pushing QoS scheduler/shaper/WRED test JSON for ${IFACE} -> ${remote_json}"
    emit_test_json "$IFACE" | rssh "cat > '$remote_json' && sudo config load -y '$remote_json'"
    log "applied test profiles to ${IFACE} (UC1=DWRR w=1 + 100Mbps, UC3=WRED+ECN, UC5=DWRR w=4)"
    log "now run: deploy/test_qos_sched.sh verify ${IFACE} ${BCM_NAME} ${BCM_PORT}"
}

cmd_verify() {
    need_switch
    echo "=== 1. Diag shell: cos port show PortBitMap=${BCM_NAME} ==="
    "$BSH" "cos port show PortBitMap=${BCM_NAME}" || true
    echo
    echo "=== 2. Diag shell: cos bandwidth_show (${BCM_NAME}) ==="
    "$BSH" "cos bandwidth_show" | grep -E "port|-----|${BCM_NAME} " || true
    echo
    echo "=== 3. Diag shell: cos discard_show Gport=${BCM_PORT} Queue=3 ==="
    "$BSH" "cos discard_show Gport=${BCM_PORT} Queue=3" || true
    echo
    echo "=== 4. Diag shell: changed WRED config entries (X/Y pipe) ==="
    "$BSH" "d chg MMU_WRED_CONFIG_X_PIPE" || true
    "$BSH" "d chg MMU_WRED_CONFIG_Y_PIPE" || true
    echo
    echo "=== 5. SONiC: show queue counters ${IFACE} ==="
    rssh "show queue counters '${IFACE}'" || true
    echo
    echo "=== 6. Diag shell: per-queue / drop counters (${BCM_NAME}) ==="
    "$BSH" "show c ${BCM_NAME}" | grep -iE "UCQ|MCQ|DROP|WRED|PERQ" || true
}

cmd_revert() {
    need_switch
    log "reverting test QUEUE / SCHEDULER / WRED_PROFILE entries on ${IFACE}"
    rssh "sudo redis-cli -n 4 HDEL 'QUEUE|${IFACE}|1' scheduler >/dev/null; \
          sudo redis-cli -n 4 HDEL 'QUEUE|${IFACE}|3' wred_profile >/dev/null; \
          sudo redis-cli -n 4 HDEL 'QUEUE|${IFACE}|5' scheduler >/dev/null; \
          sleep 1; \
          sudo redis-cli -n 4 DEL 'SCHEDULER|test_dwrr_w1_100m' 'SCHEDULER|test_dwrr_w4' 'WRED_PROFILE|test_wred_ecn' >/dev/null; \
          rm -f '/tmp/test_qos_sched_${IFACE}.json'"
    log "reverted test overlay on ${IFACE}. Run 'sudo config qos reload' on the switch to re-apply base templates."
}

case "${1:-plan}" in
    plan)       cmd_plan ;;
    apply)      cmd_apply ;;
    verify)     cmd_verify ;;
    revert)     cmd_revert ;;
    -h|--help|help) usage 0 ;;
    *)          usage 1 ;;
esac
