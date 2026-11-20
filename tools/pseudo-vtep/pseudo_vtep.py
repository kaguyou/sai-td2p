#!/usr/bin/env python3
"""pseudo_vtep.py - a fake remote VTEP for one-cable VXLAN tests (no root needed).

Listens on UDP 4789 at LOCAL_VTEP, decodes VXLAN, and pretends to be a host
FAKE_MAC/FAKE_IP behind the tunnel: answers ARP requests and ICMP echo requests
by sending VXLAN-encapsulated replies back to REMOTE_VTEP (the switch).
    pseudo_vtep.py [seconds]
"""
import socket, struct, sys, time

LOCAL_VTEP = "10.1.0.2"; REMOTE_VTEP = "10.1.0.1"; VNI = 2000
FAKE_MAC = bytes.fromhex("00aabbccdd01"); FAKE_IP = socket.inet_aton("10.200.0.99")
DURATION = float(sys.argv[1]) if len(sys.argv) > 1 else 30

def csum(b):
    if len(b) % 2: b += b"\0"
    s = sum(struct.unpack("!%dH" % (len(b)//2), b))
    while s >> 16: s = (s & 0xffff) + (s >> 16)
    return (~s) & 0xffff

def vxlan(inner):
    return struct.pack("!BBHI", 0x08, 0, 0, VNI << 8) + inner

def mac(b): return ":".join("%02x" % x for x in b)

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind((LOCAL_VTEP, 4789)); sock.settimeout(0.5)
print(f"pseudo VTEP {LOCAL_VTEP} up, VNI {VNI}, fake host {mac(FAKE_MAC)}/10.200.0.99", flush=True)
end = time.time() + DURATION; n = 0
while time.time() < end:
    try: data, peer = sock.recvfrom(65535)
    except socket.timeout: continue
    if len(data) < 22: continue
    flags, vni = data[0], struct.unpack("!I", data[4:8])[0] >> 8
    eth = data[8:]; dst, src, et = eth[:6], eth[6:12], struct.unpack("!H", eth[12:14])[0]
    n += 1
    print(f"RX #{n} from {peer[0]} vni={vni} {mac(src)}->{mac(dst)} type=0x{et:04x} len={len(eth)}", flush=True)
    if et == 0x0806 and eth[14:22] == b"\x00\x01\x08\x00\x06\x04\x00\x01":  # ARP request
        sha, spa, tpa = eth[22:28], eth[28:32], eth[38:42]
        if tpa == FAKE_IP:
            arp = b"\x00\x01\x08\x00\x06\x04\x00\x02" + FAKE_MAC + FAKE_IP + sha + spa
            frame = sha + FAKE_MAC + b"\x08\x06" + arp
            sock.sendto(vxlan(frame), (REMOTE_VTEP, 4789))
            print(f"  -> ARP reply {mac(FAKE_MAC)} is 10.200.0.99, to {socket.inet_ntoa(spa)}", flush=True)
    elif et == 0x0800:
        ip = eth[14:]; ihl = (ip[0] & 0xf) * 4
        if ip[9] == 1 and ip[16:20] == FAKE_IP and ip[ihl] == 8:  # ICMP echo to fake host
            icmp = bytearray(ip[ihl:]); icmp[0] = 0; icmp[2:4] = b"\0\0"; icmp[2:4] = struct.pack("!H", csum(bytes(icmp)))
            hdr = bytearray(ip[:ihl]); hdr[12:16], hdr[16:20] = FAKE_IP, ip[12:16]; hdr[8] = 64; hdr[10:12] = b"\0\0"
            hdr[10:12] = struct.pack("!H", csum(bytes(hdr)))
            frame = src + FAKE_MAC + b"\x08\x00" + bytes(hdr) + bytes(icmp)
            sock.sendto(vxlan(frame), (REMOTE_VTEP, 4789))
            print(f"  -> ICMP echo reply to {socket.inet_ntoa(ip[12:16])}", flush=True)
print(f"done, {n} VXLAN packets received", flush=True)
