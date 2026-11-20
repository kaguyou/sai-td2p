#!/usr/bin/env python3
"""Run bcm.user on the switch over SSH (pty) and feed diag-shell commands.
usage: bcm_ssh.py "cmd1" "cmd2" ...   (each waits for the BCM.0> prompt, up to 120s)"""
import paramiko, sys, time, re, socket, os
HOST='fe80::aa2b:b5ff:fe3a:3e8e%enp2s0'
sock=socket.socket(socket.AF_INET6, socket.SOCK_STREAM); sock.settimeout(10)
ai=socket.getaddrinfo(HOST, 22, socket.AF_INET6, socket.SOCK_STREAM)[0]; sock.connect(ai[4])
c=paramiko.SSHClient(); c.set_missing_host_key_policy(paramiko.AutoAddPolicy())
c.connect('ignored', username='admin', password=os.environ['SWITCH_PASS'], sock=sock, look_for_keys=False, allow_agent=False)
ch=c.invoke_shell(width=200, height=50); ch.settimeout(0.5)
def read_until(pat, t):
    end=time.time()+t; buf=''
    while time.time()<end:
        try:
            d=ch.recv(65536).decode('utf-8','replace'); buf+=d
            if re.search(pat, buf): break
        except socket.timeout: pass
    return buf
print(read_until(r'\$ ?$', 8)[-100:])
ch.send('cd ~/bcm && sudo ./bcm.user\n')
out=read_until(r'BCM\.\d+> ?', 90); print(out)
for cmd in sys.argv[1:]:
    t = 180 if cmd.startswith('init') else 60
    ch.send(cmd+'\n'); out=read_until(r'\nBCM\.\d+> ?$', t); print(out)
ch.send('quit\n'); time.sleep(2); print(read_until(r'\$ ?$', 10)[-300:])
