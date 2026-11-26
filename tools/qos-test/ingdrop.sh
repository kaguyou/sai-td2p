#!/bin/bash
# ingdrop.sh : 2 flows 150M each (DSCP8->UC0, DSCP46->UC5) into a 100M port for 14 s; ingress vs egress drops (BCM ports 31 in, 40 out)
SP=$(dirname "$0"); export SSHPASS=${SWITCH_PASS:?set SWITCH_PASS}; S=admin@${SWITCH_HOST:-192.168.52.1}; O="-o LogLevel=ERROR -o ConnectTimeout=8"
bs='b() { docker exec syncd python3 -c "import socket,sys; s=socket.socket(socket.AF_UNIX); s.connect(\"/var/run/hwbcm/shell.sock\"); s.sendall((sys.argv[1]+\"\n\").encode()); s.shutdown(socket.SHUT_WR); sys.stdout.write(s.makefile().read())" "$1"; }; snap() { b "show c all 31" | grep -E "DROP_PKT_ING|RUC\." ; b "show c all 40" | grep -E "UCQ_DROP_PKT\((0|5)\)|UC_PERQ_PKT\((0|5)\)"; }'
sshpass -e ssh $O $S "$bs; snap" | awk '{gsub(",","",$3); print $1,$3}' > $SP/ing.a
python3 $SP/udpblast.py 192.168.52.50 0x20 14 5001 150 >/dev/null & python3 $SP/udpblast.py 192.168.52.50 0xb8 14 5002 150 >/dev/null & wait; sleep 3
sshpass -e ssh $O $S "$bs; snap" | awk '{gsub(",","",$3); print $1,$3}' > $SP/ing.b
paste $SP/ing.a $SP/ing.b | awk '{printf "%-26s %10d\n",$1,$4-$2}'
