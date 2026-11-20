#!/usr/bin/env python3
"""Drive bcm.user on the switch over the serial console.
usage: bcm_session.py start            -> launch bcm.user (cwd ~/bcm), print banner
       bcm_session.py cmd "<bcm cmd>" [timeout]  -> send one diag-shell command, print output
       bcm_session.py shell "<bash cmd>" -> send a bash command (when bcm.user is not running)
       bcm_session.py quit             -> exit bcm.user"""
import serial, sys, time, re
s = serial.Serial('/dev/ttyUSB0', 115200, timeout=0.3)
def drain(t=2.0, stop=None):
    end=time.time()+t; buf=b''
    while time.time()<end:
        d=s.read(65536)
        if d:
            buf+=d; end=time.time()+1.0
            if stop and re.search(stop, buf.decode('utf-8','replace')): break
    return buf.decode('utf-8','replace')
PROMPT = r'BCM\.\d+> ?$|BCM\.\d+>\s*$'
op = sys.argv[1]
if op == 'start':
    s.write(b'\r\n'); drain(1)
    s.write(b'cd ~/bcm && sudo ./bcm.user\n')
    print(drain(float(sys.argv[2]) if len(sys.argv)>2 else 40, stop=PROMPT))
elif op == 'cmd':
    t = float(sys.argv[3]) if len(sys.argv)>3 else 30
    s.write(sys.argv[2].encode()+b'\n')
    print(drain(t, stop=PROMPT))
elif op == 'shell':
    s.write(sys.argv[2].encode()+b'\n')
    print(drain(float(sys.argv[3]) if len(sys.argv)>3 else 8))
elif op == 'quit':
    s.write(b'quit\n'); print(drain(10))
