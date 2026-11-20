import serial, time, re, os
def open_port():
    return serial.Serial('/dev/ttyUSB0', 115200, timeout=0.5)
def drain(s, t=2.0, stop=None):
    end=time.time()+t; buf=b''
    while time.time()<end:
        d=s.read(65536)
        if d:
            buf+=d; end=time.time()+1.0
            if stop and re.search(stop, buf.decode('utf-8','replace')): break
    return buf.decode('utf-8','replace')
def login(s, user='admin', pw=os.environ['SWITCH_PASS'], tries=8):
    """Bring the console to a shell prompt whatever state it is in."""
    for i in range(tries):
        s.write(b'\r'); out=drain(s, 4)
        if re.search(r'admin@\S+:\S*\$ ?$', out.strip()) or 'admin@sonic' in out[-80:]:
            return True
        if 'Password:' in out[-40:]:
            s.write(pw.encode()+b'\r'); out=drain(s, 10)
            if 'admin@' in out: return True
            continue
        if 'login:' in out[-40:]:
            s.write(user.encode()+b'\r'); out=drain(s, 4)
            if 'Password:' in out:
                s.write(pw.encode()+b'\r'); out=drain(s, 10)
                if 'admin@' in out: return True
            continue
        time.sleep(3)
    return False
def sh(s, cmd, t=6):
    s.write(cmd.encode()+b'\r'); return drain(s, t)
