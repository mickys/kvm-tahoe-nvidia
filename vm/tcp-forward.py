#!/usr/bin/env python3
"""Forward TCP from one or more listen addresses to a target (no socat needed on the host).

  tcp-forward.py [--vnc-password-only] 127.0.0.1:5931 192.168.1.10:5931 [more listen addresses]
                                       ^ target         ^ listen addresses

Used to reach the guest's macOS Screen Sharing (QEMU hostfwd 127.0.0.1:5931 -> guest :5900) from the LAN.
--vnc-password-only: rewrite the server's RFB security-type list to offer only type 2 (classic VNC password).
macOS lists Apple's own types first (30 = Diffie-Hellman), and RealVNC picks 30 and fails with "key length too
large". Needs "VNC viewers may control screen with password" on in the guest (kickstart -setvnclegacy).
"""
import socket, sys, threading

args = sys.argv[1:]
vnc_pw_only = "--vnc-password-only" in args
args = [a for a in args if a != "--vnc-password-only"]
th, tp = args[0].rsplit(":", 1)

def pipe(a, b):
    try:
        while (d := a.recv(65536)):
            b.sendall(d)
    except OSError:
        pass
    finally:
        for s in (a, b):
            try: s.shutdown(socket.SHUT_RDWR)
            except OSError: pass

def recv_exact(s, n):
    buf = b""
    while len(buf) < n:
        d = s.recv(n - len(buf))
        if not d:
            raise OSError("closed during handshake")
        buf += d
    return buf

def handle(c):
    try:
        t = socket.create_connection((th, int(tp)), 10)
        t.settimeout(None)
    except OSError:
        c.close(); return
    if vnc_pw_only:
        try:
            c.sendall(recv_exact(t, 12))                 # server ProtocolVersion
            t.sendall(recv_exact(c, 12))                 # client ProtocolVersion
            n = recv_exact(t, 1)[0]                      # RFB 3.7+: number of security types, then the types
            types = recv_exact(t, n) if n else b""
            if 2 in types:
                c.sendall(b"\x01\x02")                   # offer only VNC password authentication
            else:
                c.sendall(bytes([n]) + types)            # (legacy VNC not enabled in the guest: pass through)
        except OSError:
            c.close(); t.close(); return
    for a, b in ((c, t), (t, c)):
        threading.Thread(target=pipe, args=(a, b), daemon=True).start()

def serve(addr):
    h, p = addr.rsplit(":", 1)
    ls = socket.socket(); ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); ls.bind((h, int(p))); ls.listen(8)
    while True:
        c, _ = ls.accept()
        threading.Thread(target=handle, args=(c,), daemon=True).start()

for a in args[1:]:
    threading.Thread(target=serve, args=(a,), daemon=True).start()
threading.Event().wait()
