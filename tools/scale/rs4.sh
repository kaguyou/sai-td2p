#!/bin/bash
# rs4.sh v6|v6h|nbr SET|DEL START N : scale objects straight into APPL_DB; wait until ASIC_DB stops moving
#   v6  = blackhole 2001:db8:X:Y::/64 routes   v6h = blackhole /128 routes   nbr = IPv4 neighbors on $IF (default Ethernet35)
kind=$1; op=$2; st=$3; n=$4; IF=${IF:-Ethernet35}
python3 - $kind $op $st $n $IF > /tmp/rs4.json <<'PY'
import sys,json
kind,op,st,n,ifn=sys.argv[1],sys.argv[2],int(sys.argv[3]),int(sys.argv[4]),sys.argv[5]
o=[]
for i in range(st,st+n):
    if kind=="v6": k,v="ROUTE_TABLE:2001:db8:%x:%x::/64"%(i>>16,i&0xffff),{"blackhole":"true"}
    elif kind=="v6h": k,v="ROUTE_TABLE:2001:db9::%x:%x/128"%(i>>16,i&0xffff),{"blackhole":"true"}
    else: k,v="NEIGH_TABLE:%s:11.%d.%d.%d"%(ifn,(i>>16)&255,(i>>8)&255,i&255),{"neigh":"02:11:%02x:%02x:%02x:%02x"%(0,(i>>16)&255,(i>>8)&255,i&255),"family":"IPv4"}
    o.append({k:v,"OP":op})
print(json.dumps(o))
PY
docker exec -i swss bash -c "cat > /tmp/rs4.json" < /tmp/rs4.json
cnt(){ sonic-db-cli ASIC_DB dbsize; }
base=$(cnt); t0=$(date +%s.%N); docker exec swss swssconfig /tmp/rs4.json >/dev/null 2>&1
last=-1; same=0; tl=$t0
for i in $(seq 1 3000); do c=$(cnt); if [ "$c" = "$last" ]; then same=$((same+1)); else same=0; tl=$(date +%s.%N); fi; last=$c; [ $same -ge 25 ] && break; sleep 0.2; done
awk -v a=$t0 -v c=$tl -v n=$n -v b=$base -v e=$last -v op=$op -v k=$kind 'BEGIN{d=e-b; if(d<0)d=-d; printf "RESULT %s %s %d: ASIC_DB keys %d -> %d (delta %d) in %.1fs = %.0f/s\n",k,op,n,b,e,e-b,c-a,d/(c-a)}'
docker ps --format '{{.Names}} {{.Status}}' | grep -E 'swss|syncd' | tr '\n' ' '; echo
echo "ERR syncd $(sudo grep -a -c 'ERR syncd' /var/log/syslog) orch $(sudo grep -a -c 'ERR swss#orchagent' /var/log/syslog)"
bash /home/admin/sai-td2p/bshl.sh 'l3 info' | grep -E 'routes|hosts|nexthops'
bash /home/admin/sai-td2p/bshl.sh 'l3 ip6route show' | wc -l
