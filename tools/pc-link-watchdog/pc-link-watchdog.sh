#!/bin/bash
# pc-link-watchdog.sh — PC side workaround for the one-sided 1G link after a switch cold boot.
#
# Seen 2026-11-21 (3/3 cold reboots of the AS5812-54T, never on a swss restart): while the SDK
# initialises the BCM84858 PHYs the PC's Realtek RTL8168h (r8169) sees the link flap 4-5 times,
# then reports "Link is Down" for good, although the switch PHY shows a complete 1000BASE-T link
# (AN complete, local+remote receiver OK). Nothing on the switch side recovers it (autoneg
# restart, enable off 15 s/on, PMA reset, forced 1G round trip, SGMII round trip); only a reset
# of the PC NIC does (ethtool -r / nmcli con up).
#
# Usage: run as root from a systemd timer every 30 s, or by hand:  sudo pc-link-watchdog.sh
IF=${1:-enp2s0}
F=/run/pc-link-watchdog.$IF
if [ "$(cat /sys/class/net/$IF/carrier 2>/dev/null)" = "1" ]; then rm -f "$F"; exit 0; fi
# no carrier: count consecutive misses, reset the NIC after the 2nd (>= 30 s without link)
n=$(( $(cat "$F" 2>/dev/null || echo 0) + 1 )); echo $n > "$F"
if [ $n -ge 2 ]; then
  logger -t pc-link-watchdog "$IF has had no carrier for $((n*30)) s, restarting autoneg (ethtool -r)"
  ethtool -r "$IF"; echo 0 > "$F"
fi
