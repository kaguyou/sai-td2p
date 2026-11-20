#!/usr/bin/env python3
"""Send a file to the switch over the serial console (raw tty + head -c), then verify sha256.
usage: serial_send.py <local_file> <remote_path>"""
import serial, sys, time, hashlib, base64, os
src, dst = sys.argv[1], sys.argv[2]
data = open(src,'rb').read()
b64 = base64.b64encode(data)
sha = hashlib.sha256(data).hexdigest()
s = serial.Serial('/dev/ttyUSB0', 115200, timeout=0.5)
def drain(t=2.0):
    end=time.time()+t; buf=b''
    while time.time()<end:
        d=s.read(65536)
        if d: buf+=d; end=time.time()+1.0
    return buf.decode('utf-8','replace')
def send(c,t=3):
    s.write(c.encode()+b'\r\n'); return drain(t)
s.write(b'\r\n'); drain(1.5)
tmp = dst + '.b64'
# raw mode: no echo, no line discipline; head reads exactly len(b64) bytes
s.write(f'stty -echo raw; echo READY_MARK; head -c {len(b64)} > {tmp}; stty sane; echo RECV_DONE\n'.encode())
out=''
t_wait=time.time()
while 'READY_MARK' not in out and time.time()-t_wait<10:
    out+=drain(0.5)
assert 'READY_MARK' in out, out
time.sleep(0.3)
t0=time.time(); CH=2048
for i in range(0, len(b64), CH):
    s.write(b64[i:i+CH])
    s.flush()
    if (i//CH) % 50 == 0:
        print(f'\r{i/len(b64)*100:5.1f}% {(i/1024)/(time.time()-t0+1e-6):.1f} KB/s', end='', flush=True)
print()
out = drain(5)
while 'RECV_DONE' not in out and time.time()-t0 < len(b64)/8000+60:
    out += drain(3)
print(f'sent {len(b64)} b64 bytes in {time.time()-t0:.0f}s; RECV_DONE={"RECV_DONE" in out}')
r = send(f'wc -c {tmp}; base64 -d {tmp} > {dst} && rm -f {tmp} && sha256sum {dst}', 20)
print(r)
ok = sha in r
print('sha256 match:', ok, sha)
sys.exit(0 if ok else 1)
