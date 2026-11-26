#!/bin/bash
# qsample.sh PORT SECS : two samples of per-queue TX bytes / drops (diag shell) SECS apart, prints Mbit/s per queue
bs() { docker exec syncd python3 -c 'import socket,sys; s=socket.socket(socket.AF_UNIX); s.connect("/var/run/hwbcm/shell.sock"); s.sendall((sys.argv[1]+"\n").encode()); s.shutdown(socket.SHUT_WR); sys.stdout.write(s.makefile().read())' "$1"; }
snap() { bs "show c all $1" | grep -E "UC_PERQ_BYTE|UCQ_DROP_PKT|TBYT" | awk '{gsub(",","",$3); print $1,$3}'; }
t1=$(date +%s.%N); A=$(snap $1); sleep $2; t2=$(date +%s.%N); B=$(snap $1)
paste <(echo "$A") <(echo "$B") | awk -v dt=$(awk "BEGIN{print $t2-$t1}") '$4-$2>0 { if ($1 ~ /BYT/) printf "%-24s %8.1f Mbit/s\n",$1,($4-$2)*8/dt/1e6; else printf "%-24s %8.0f pkt/s\n",$1,($4-$2)/dt }'
