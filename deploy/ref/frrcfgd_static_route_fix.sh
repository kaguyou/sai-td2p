#!/bin/bash
# Live `config route add` (CONFIG_DB STATIC_ROUTE) never reached FRR with frr_mgmt_framework_config=true.
# Cause: frrcfgd.py maps STATIC_ROUTE -> ['staticd'] and sends "vrf X / ip route ..." straight to staticd, but
# FRR 10.3 staticd has no such command (static routes live in mgmtd) -> "% [STATIC] Unknown command".
# At bgp start the routes come from the staticd template, so they are applied fine after restart/reboot.
# This patches the running container (lost when the bgp container is recreated; re-run after `systemctl restart bgp`).
# Run on the switch: sudo bash frrcfgd_static_route_fix.sh
F=/usr/local/lib/python3.11/dist-packages/frrcfgd/frrcfgd.py
docker exec bgp sed -i "s/'STATIC_ROUTE': \['staticd'\]/'STATIC_ROUTE': ['mgmtd']/" $F
docker exec bgp grep -n "'STATIC_ROUTE': \[" $F
docker exec bgp supervisorctl restart frrcfgd
