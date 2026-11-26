#!/usr/bin/env python3
# udpblast.py DST TOS SECONDS [PORT] [RATE_MBPS] : UDP sender with a given IP TOS byte, prints sent Mbit/s
import socket,sys,time
dst=sys.argv[1]; tos=int(sys.argv[2],0); dur=float(sys.argv[3]); port=int(sys.argv[4]) if len(sys.argv)>4 else 5001
rate=float(sys.argv[5]) if len(sys.argv)>5 else 0
s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM); s.setsockopt(socket.IPPROTO_IP,socket.IP_TOS,tos)
s.setsockopt(socket.SOL_SOCKET,socket.SO_SNDBUF,4<<20); s.connect((dst,port))
p=b'x'*1400; n=0; t0=time.time(); end=t0+dur
while True:
    now=time.time()
    if now>=end: break
    if rate and n*1428*8/1e6 > rate*(now-t0): time.sleep(0.0005); continue
    for _ in range(50):
        try: s.send(p); n+=1
        except OSError: pass
el=time.time()-t0
print(f"tos={tos:#x} sent {n} pkts {n*1428*8/el/1e6:.0f} Mbit/s (L3) in {el:.1f}s")
