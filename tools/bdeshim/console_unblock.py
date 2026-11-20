# Feed zero bytes to the stuck 'head -c' on the switch console until RECV_DONE appears, then restore the tty.
import serial, time, sys
s = serial.Serial('/dev/ttyUSB0', 115200, timeout=0.3)
t0=time.time(); sent=0; out=b''
while time.time()-t0 < 1500:
    s.write(b'\0'*2048); sent+=2048
    d=s.read(4096)
    if d:
        out+=d
        if b'RECV_DONE' in out: break
print(f'sent {sent} bytes in {time.time()-t0:.0f}s, done={b"RECV_DONE" in out}')
s.write(b'\r'); time.sleep(1); s.write(b'stty sane\r'); time.sleep(1); print(s.read(4096).decode('utf-8','replace')[-200:])
