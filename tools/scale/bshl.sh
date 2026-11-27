#!/bin/bash
# bshl "cmd": diag shell command on the box itself
docker exec syncd python3 -c 'import socket,sys; s=socket.socket(socket.AF_UNIX); s.connect("/var/run/hwbcm/shell.sock"); s.sendall((sys.argv[1]+"\n").encode()); s.shutdown(socket.SHUT_WR); sys.stdout.write(s.makefile().read())' "$*"
