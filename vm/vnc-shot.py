#!/usr/bin/env python3
"""Grab one frame of the guest's macOS Screen Sharing through the host's LAN forwarder (VNC password auth, raw encoding).

  vnc-shot.py out.png [host:port]       default: first LAN_LISTEN:SCREENSHARING_FWD_PORT from config.env;
                                        password from KVM_HOST:VM_DIR/vnc-password; needs openssl and ImageMagick
"""
import os, socket, struct, subprocess, sys

import os, re
cfg = {}
for line in open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "config.env")):
    m = re.match(r'\s*([A-Z_][A-Z0-9_]*)=("([^"]*)"|(\S*))', line)
    if m: cfg[m.group(1)] = m.group(3) if m.group(3) is not None else m.group(4)
out = sys.argv[1]
host, port = (sys.argv[2] if len(sys.argv) > 2 else f"{cfg['LAN_LISTEN'].split()[0]}:{cfg['SCREENSHARING_FWD_PORT']}").rsplit(":", 1)
pw = subprocess.run(["ssh", cfg["KVM_HOST"], f"cat {cfg['VM_DIR']}/vnc-password"],
                    capture_output=True, text=True, check=True).stdout.strip().encode()[:8].ljust(8, b"\0")
key = bytes(int(f"{b:08b}"[::-1], 2) for b in pw)          # VNC DES keys are bit-reversed
s = socket.create_connection((host, int(port)), 10); s.settimeout(30)
def rx(n):
    b = bytearray()
    while len(b) < n:
        d = s.recv(min(1 << 20, n - len(b)))
        if not d: raise EOFError
        b += d
    return bytes(b)
rx(12); s.sendall(b"RFB 003.008\n"); rx(rx(1)[0]); s.sendall(b"\x02")
chal = rx(16)
s.sendall(subprocess.run(["openssl", "enc", "-des-ecb", "-nopad", "-K", key.hex(), "-provider", "legacy",
                          "-provider", "default"], input=chal, capture_output=True).stdout)
if struct.unpack(">I", rx(4))[0] != 0: sys.exit("VNC auth failed")
s.sendall(b"\x01"); hdr = rx(24); w, h = struct.unpack(">HH", hdr[:4]); rx(struct.unpack(">I", hdr[20:24])[0])
s.sendall(struct.pack(">BxxxBBBBHHHBBBxxx", 0, 32, 24, 0, 1, 255, 255, 255, 16, 8, 0))
s.sendall(struct.pack(">BxHi", 2, 1, 0)); s.sendall(struct.pack(">BBHHHH", 3, 0, 0, 0, w, h))
fb = bytearray(w * h * 3)
while rx(1)[0] != 0: pass
rx(1)
for _ in range(struct.unpack(">H", rx(2))[0]):
    x, y, rw, rh, enc = struct.unpack(">HHHHi", rx(12)); data = rx(rw * rh * 4)
    for row in range(rh):
        src = data[row * rw * 4:(row + 1) * rw * 4]; rgb = bytearray(rw * 3)
        rgb[0::3] = src[2::4]; rgb[1::3] = src[1::4]; rgb[2::3] = src[0::4]
        o = ((y + row) * w + x) * 3; fb[o:o + rw * 3] = rgb
subprocess.run(["convert", "-", "-resize", "1920x1080>", out], input=f"P6 {w} {h} 255\n".encode() + fb, check=True)
print(f"{w}x{h} -> {out}")
