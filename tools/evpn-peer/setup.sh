#!/bin/bash
# PC side of the real-EVPN test: Linux VTEP 10.1.0.2 (VNI 2000 -> br200) + FRR as EVPN peer of the switch.
set -x
firewall-cmd --add-port=179/tcp >/dev/null && echo "firewall: 179/tcp open"
firewall-cmd --add-port=4789/udp >/dev/null 2>&1 || true
ip link show br200 >/dev/null 2>&1 || ip link add br200 type bridge
ip link set br200 address 00:aa:bb:cc:dd:01
ip addr replace 10.200.0.99/24 dev br200
ip link set br200 up
ip link show vxlan2000 >/dev/null 2>&1 || ip link add vxlan2000 type vxlan id 2000 local 10.1.0.2 dstport 4789 nolearning
ip link set vxlan2000 master br200 || true
ip link set vxlan2000 up
echo "vtep: vxlan2000 (vni 2000, local 10.1.0.2) in br200 = fake host 00:aa:bb:cc:dd:01 / 10.200.0.99"
podman rm -f frr-evpn >/dev/null 2>&1 || true
podman run -d --name frr-evpn --network host --privileged \
  -v $(cd "$(dirname "$0")" && pwd)/frr.conf:/etc/frr/frr.conf:ro -v $(cd "$(dirname "$0")" && pwd)/daemons:/etc/frr/daemons:ro \
  quay.io/frrouting/frr:10.4.1 >/dev/null && echo "frr-evpn container started (AS 65200, peer 192.168.52.1)"
sleep 4
podman exec frr-evpn vtysh -c "show bgp summary" | tail -6
