#!/bin/bash
# qtest.sh RATE_MBPS TOS [TOS...] : one UDP flow per TOS at RATE each to hostA for 16 s; sample 8 s in the middle
SP=$(dirname "$0"); export SSHPASS=${SWITCH_PASS:?set SWITCH_PASS}; S=admin@${SWITCH_HOST:-192.168.52.1}
rate=$1; shift; i=0; pids=""
for tos in "$@"; do python3 $SP/udpblast.py 192.168.52.50 $tos 16 $((5001+i)) $rate > $SP/blast.$i.out & pids="$pids $!"; i=$((i+1)); done
sleep 4
sshpass -e ssh -o ConnectTimeout=8 -o LogLevel=ERROR $S "QPORT=${QPORT:-40}; "'bash sai-td2p/rx.sh 8 & bash sai-td2p/qsample.sh ${QPORT:-40} 8 | grep -E "PERQ_BYTE|DROP"; wait'
wait $pids; cat $SP/blast.*.out; rm -f $SP/blast.*.out
