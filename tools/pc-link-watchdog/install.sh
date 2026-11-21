#!/bin/bash
# install.sh — install the watchdog as a root systemd timer on the PC (run with sudo)
set -e
install -m 755 "$(dirname "$0")/pc-link-watchdog.sh" /usr/local/bin/pc-link-watchdog.sh
cat > /etc/systemd/system/pc-link-watchdog.service <<'U'
[Unit]
Description=Reset enp2s0 autoneg when the link to the switch stays down (switch cold boot quirk)
[Service]
Type=oneshot
ExecStart=/usr/local/bin/pc-link-watchdog.sh enp2s0
U
cat > /etc/systemd/system/pc-link-watchdog.timer <<'U'
[Unit]
Description=pc-link-watchdog every 30 s
[Timer]
OnBootSec=60
OnUnitActiveSec=30
[Install]
WantedBy=timers.target
U
systemctl daemon-reload; systemctl enable --now pc-link-watchdog.timer; systemctl list-timers pc-link-watchdog.timer --no-pager | head -2
