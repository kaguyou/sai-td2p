#!/bin/bash
# rs3.sh SET|DEL START N : blackhole /24 routes index START..START+N-1 into APPL_DB; wait until ASIC_DB count stops moving
op=$1; st=$2; n=$3
python3 - $op $st $n > /tmp/rs3.json <<'PY'
import sys,json
op,st,n=sys.argv[1],int(sys.argv[2]),int(sys.argv[3])
print(json.dumps([{"ROUTE_TABLE:%d.%d.%d.0/24"%(20+(i>>16),(i>>8)&255,i&255):{"blackhole":"true"},"OP":op} for i in range(st,st+n)]))
PY
docker exec -i swss bash -c "cat > /tmp/rs3.json" < /tmp/rs3.json
cnt(){ sonic-db-cli ASIC_DB dbsize; }
base=$(cnt); t0=$(date +%s.%N); docker exec swss swssconfig /tmp/rs3.json >/dev/null 2>&1
last=-1; same=0; tl=$t0
for i in $(seq 1 3000); do c=$(cnt); if [ "$c" = "$last" ]; then same=$((same+1)); else same=0; tl=$(date +%s.%N); fi; last=$c; [ $same -ge 25 ] && break; sleep 0.2; done
awk -v a=$t0 -v c=$tl -v n=$n -v b=$base -v e=$last -v op=$op 'BEGIN{printf "RESULT %s %d: ASIC_DB keys %d -> %d (delta %d) in %.1fs = %.0f/s\n",op,n,b,e,e-b,c-a,(e-b<0?b-e:e-b)/(c-a)}'
docker ps --format '{{.Names}} {{.Status}}' | grep -E 'swss|syncd' | tr '\n' ' '; echo
docker exec syncd ps -o rss= -C syncd | awk '{print "syncd RSS MB",$1/1024}'; docker exec swss ps -o rss= -C orchagent | awk '{print "orchagent RSS MB",$1/1024}'
echo "ERR syncd $(sudo grep -a -c 'ERR syncd' /var/log/syslog) orch $(sudo grep -a -c 'ERR swss#orchagent' /var/log/syslog)"
bash /home/admin/sai-td2p/bshl.sh 'l3 info' | grep -E 'routes|hosts'
