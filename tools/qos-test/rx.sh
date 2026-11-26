r() { sudo ip netns exec hostA cat /sys/class/net/eth0/statistics/rx_bytes; }
a=$(r); sleep ${1:-10}; b=$(r); echo "hostA rx $(( (b-a)*8/${1:-10}/1000000 )) Mbit/s"
