#!/bin/bash
# EVPN peering toward the PC VTEP, the SONiC way: frrcfgd (frr_mgmt_framework_config=true) generates FRR
# config from these CONFIG_DB tables, so it survives bgp restarts and reboots after `config save -y`.
# Legacy BGP_NEIGHBOR|<ip> entries (bgpcfgd style) must be removed first: frrcfgd keys are vrf-qualified.
R="redis-cli -n 4"
$R keys "BGP_NEIGHBOR|*" | grep -vE "\|default\|" | xargs -r -n 50 $R del >/dev/null
$R hset "BGP_GLOBALS|default" local_asn 65100 router_id 10.1.0.1 default_ipv4_unicast false log_nbr_state_changes true
$R hset "BGP_GLOBALS_AF|default|ipv4_unicast" NULL NULL
$R hset "BGP_GLOBALS_AF|default|l2vpn_evpn" advertise-all-vni true
$R hset "BGP_GLOBALS_AF_NETWORK|default|ipv4_unicast|10.1.0.1/32" NULL NULL
$R hset "BGP_NEIGHBOR|default|192.168.52.2" asn 65200 local_addr 192.168.52.1 name pc-vtep admin_status up
# FRR's ebgp-requires-policy has no frrcfgd knob: permit-all route maps on both AFs
$R hset "ROUTE_MAP|ALLOW_ALL|10" route_operation permit
for af in ipv4_unicast l2vpn_evpn; do
  $R hset "BGP_NEIGHBOR_AF|default|192.168.52.2|$af" admin_status true "route_map_in@" ALLOW_ALL "route_map_out@" ALLOW_ALL
done
$R hset "DEVICE_METADATA|localhost" frr_mgmt_framework_config true
sudo config save -y
sudo systemctl restart bgp
