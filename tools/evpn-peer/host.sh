#!/bin/bash
# A second host behind the PC VTEP: veth pair, one end in br200 (so the bridge learns a real MAC
# on an access port and FRR advertises it as an EVPN type-2), the other end as host 10.200.0.77.
set -x
ip link show vhost0 >/dev/null 2>&1 || ip link add vhost0 address 02:77:77:77:77:77 type veth peer name vhost0b
ip link set vhost0b master br200 || true
ip link set vhost0b up
ip addr replace 10.200.0.77/24 dev vhost0
ip link set vhost0 up
# make the bridge learn the MAC right away and let zebra pick up the neighbor
ping -c 2 -W 1 -I vhost0 10.200.0.99 >/dev/null 2>&1 || true
bridge fdb show br br200 | grep -i 02:77
podman exec frr-evpn vtysh -c "show evpn mac vni 2000" | tail -5
