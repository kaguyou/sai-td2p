#!/bin/bash
# pc_hairpin.sh up|down|status - data-plane loss harness on the PC with ONE cable (run as root).
#
# A second "host" lives in netns pcB on a VLAN 300 sub-interface of the PC NIC. A ping from the PC's main
# namespace (192.168.52.2, Vlan100 untagged) to 10.3.0.2 is ROUTED by the switch ASIC from Vlan100 to Vlan300
# and comes back on the same cable, tagged. Neighbours are pinned static on both PC sides so that nothing
# depends on the switch CPU while it reboots (warm-reboot loss measurement).
#   sudo bash pc_hairpin.sh up            then, as a normal user:  ping -i 0.1 10.3.0.2
#   sudo bash pc_hairpin.sh down
# `nmcli con up` on the NIC removes the route and the static neighbour: run `up` again afterwards.
set -e
IF=${IF:-enp2s0}; VID=300; NS=pcB
B_IP=10.3.0.2/24; B_GW=10.3.0.1; A_GW=192.168.52.1; B_MAC=02:00:00:00:03:00
case "${1:-status}" in
up)
    ip netns add $NS 2>/dev/null || true
    if ! ip -n $NS link show $IF.$VID >/dev/null 2>&1; then
        ip link show $IF.$VID >/dev/null 2>&1 || ip link add link $IF name $IF.$VID type vlan id $VID
        ip link set $IF.$VID address $B_MAC
        ip link set $IF.$VID netns $NS
    fi
    ip -n $NS link set lo up
    ip -n $NS link set $IF.$VID up
    ip -n $NS addr replace $B_IP dev $IF.$VID
    ip -n $NS route replace default via $B_GW
    ip route replace 10.3.0.2/32 via $A_GW dev $IF
    # learn the switch MACs through the live CPU path once, then pin them
    ping -c 2 -W 1 -q $A_GW >/dev/null 2>&1 || true
    ip netns exec $NS ping -c 2 -W 1 -q $B_GW >/dev/null 2>&1 || true
    ping -c 2 -W 1 -q 10.3.0.2 >/dev/null 2>&1 || true
    A_MAC=$(ip neigh show $A_GW dev $IF | awk '{for(i=1;i<=NF;i++) if($i=="lladdr") print $(i+1)}')
    G_MAC=$(ip -n $NS neigh show $B_GW | awk '{for(i=1;i<=NF;i++) if($i=="lladdr") print $(i+1)}')
    [ -n "$A_MAC" ] && ip neigh replace $A_GW lladdr $A_MAC dev $IF nud permanent
    [ -n "$G_MAC" ] && ip -n $NS neigh replace $B_GW lladdr $G_MAC dev $IF.$VID nud permanent
    echo "switch MAC: Vlan100 gw $A_GW = ${A_MAC:-UNRESOLVED}, Vlan300 gw $B_GW = ${G_MAC:-UNRESOLVED}"
    ping -c 3 -W 1 10.3.0.2 | tail -2
    ;;
down)
    ip route del 10.3.0.2/32 2>/dev/null || true
    ip neigh del $A_GW dev $IF 2>/dev/null || true
    ip netns del $NS 2>/dev/null || true
    echo "removed"
    ;;
status)
    ip route get 10.3.0.2; ip neigh show $A_GW dev $IF
    ip -n $NS -br addr 2>/dev/null; ip -n $NS neigh 2>/dev/null
    ;;
esac
