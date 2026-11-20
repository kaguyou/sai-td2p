# shellcheck shell=bash
# Shared helpers for the sai-td2p deploy scripts. Source this file, do not run it.
#
# Everything that talks to the switch goes through here. Target selection tries
# IPv4 first, then the IPv6 link-local address (works even when IPv4 is flaky).
# Override with environment variables:
#   SWITCH_HOST  skip probing, use this host literally
#   SWITCH_V4 / SWITCH_V6 / SWITCH_USER / SWITCH_PASS

DEPLOY_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

SWITCH_USER="${SWITCH_USER:-admin}"
SWITCH_PASS="${SWITCH_PASS:?set SWITCH_PASS (switch admin password) in the environment}"
SWITCH_V4="${SWITCH_V4:-192.168.0.2}"
SWITCH_V6="${SWITCH_V6:-fe80::aa2b:b5ff:fe3a:3e8e%enp2s0}"
SWITCH_HOST="${SWITCH_HOST:-}"
SWITCH_SCP=""

# Paths on the switch
HWSKU_HOST_DIR=/usr/share/sonic/device/x86_64-accton_as5812_54t-r0/Accton-AS5812-54T
REMOTE_DIR=/home/admin/sai-td2p            # staging + backups on the switch
SYNCD_CT=syncd                             # docker container name
SYNCD_IMAGE=docker-syncd-brcm:latest

SSH_OPTS=(-o StrictHostKeyChecking=accept-new -o ConnectTimeout=8 -o LogLevel=ERROR -o BatchMode=no)

die()  { echo "ERROR: $*" >&2; exit 1; }
log()  { echo "[$(date +%H:%M:%S)] $*"; }
need() { command -v "$1" >/dev/null 2>&1 || die "missing tool on this PC: $1"; }

# Sets SWITCH_HOST (for ssh) and SWITCH_SCP (bracketed when IPv6, for scp).
pick_target() {
    need sshpass; need ssh; need scp
    export SSHPASS="$SWITCH_PASS"
    if [ -z "$SWITCH_HOST" ]; then
        local cand
        for cand in "$SWITCH_V4" "$SWITCH_V6"; do
            if sshpass -e ssh "${SSH_OPTS[@]}" "$SWITCH_USER@$cand" true 2>/dev/null; then
                SWITCH_HOST="$cand"
                break
            fi
        done
    fi
    [ -n "$SWITCH_HOST" ] || die "switch unreachable via $SWITCH_V4 and $SWITCH_V6 (check: ip -br addr show enp2s0; nmcli con up sonic-mgmt)"
    case "$SWITCH_HOST" in
        *:*) SWITCH_SCP="[$SWITCH_HOST]" ;;
        *)   SWITCH_SCP="$SWITCH_HOST" ;;
    esac
    log "switch: $SWITCH_USER@$SWITCH_HOST"
}

# rssh <command...>  : run a command on the switch
rssh() { sshpass -e ssh "${SSH_OPTS[@]}" "$SWITCH_USER@$SWITCH_HOST" "$@"; }

# rscp_to <local> <remote-path>
rscp_to() { sshpass -e scp "${SSH_OPTS[@]}" -q "$1" "$SWITCH_USER@$SWITCH_SCP:$2"; }

# rscp_from <remote-path> <local>
rscp_from() { sshpass -e scp "${SSH_OPTS[@]}" -q "$SWITCH_USER@$SWITCH_SCP:$1" "$2"; }

# rssh_script <local-script> [args...] : run a local bash script on the switch via stdin
rssh_script() {
    local script="$1"; shift
    local quoted=""
    local a
    for a in "$@"; do quoted+=" $(printf '%q' "$a")"; done
    sshpass -e ssh "${SSH_OPTS[@]}" "$SWITCH_USER@$SWITCH_HOST" "bash -s --$quoted" < "$script"
}

# sha256 of a local file (hash only)
sha256_of() { sha256sum "$1" | awk '{print $1}'; }

print_start_howto() {
    cat <<'EOF'

---------------------------------------------------------------------------
Start (on the switch, as admin):
    sudo systemctl reset-failed swss syncd
    sudo systemctl start swss

Watch logs (second terminal):
    sudo tail -f /var/log/syslog | grep -E 'syncd|orchagent'
    docker logs -f syncd

Checks once it is up (give it ~60-90 s):
    show interfaces status
    docker exec syncd cat /etc/sai.d/sai.profile
    redis-cli -n 1 keys 'ASIC_STATE:SAI_OBJECT_TYPE_PORT*' | wc -l      # expect 72 (+1 CPU port)
    redis-cli -n 1 keys 'ASIC_STATE:SAI_OBJECT_TYPE_SWITCH*'

Stop again:
    sudo systemctl stop swss syncd
---------------------------------------------------------------------------
EOF
}
