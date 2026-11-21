#!/bin/bash
# Re-apply the EVPN BGP peering toward the PC after a bgp container restart (bgpcfgd regenerates frr.conf).
vtysh -c 'configure terminal' -c 'router bgp 65100' \
  -c 'neighbor 192.168.52.2 remote-as 65200' -c 'neighbor 192.168.52.2 description pc-vtep' -c 'neighbor 192.168.52.2 timers 3 9' \
  -c 'address-family ipv4 unicast' -c 'neighbor 192.168.52.2 activate' -c 'network 10.1.0.1/32' -c 'exit-address-family' \
  -c 'address-family l2vpn evpn' -c 'neighbor 192.168.52.2 activate' -c 'advertise-all-vni' -c 'exit-address-family' -c 'end'
vtysh -c 'show bgp l2vpn evpn summary' | tail -3
