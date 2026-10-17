#!/usr/bin/env python3
"""Type into the Tahoe VM through QMP send-key (guest keyboard layout: U.S.). Run in VM_DIR on the host while
vm/run.sh runs the VM (under libvirt use `virsh send-key` instead: libvirt owns the QMP socket).

  qmp-type.py combo meta_l spc        press keys together (QEMU qcodes)
  qmp-type.py text "Terminal"         type a string
  qmp-type.py stdin                   type one line read from stdin, then Return (for passwords: never on argv)
  qmp-type.py shot screen.png         QMP screendump (png)
"""
import json, socket, sys, time

SHIFTED = {'_': 'minus', '+': 'equal', '?': 'slash', '>': 'dot', '<': 'comma', ':': 'semicolon', '"': 'apostrophe',
           '~': 'grave_accent', '{': 'bracket_left', '}': 'bracket_right', '|': 'backslash', '!': '1', '@': '2',
           '#': '3', '$': '4', '%': '5', '^': '6', '&': '7', '*': '8', '(': '9', ')': '0'}
PLAIN = {' ': 'spc', '\n': 'ret', '-': 'minus', '=': 'equal', '/': 'slash', '.': 'dot', ',': 'comma',
         ';': 'semicolon', "'": 'apostrophe', '`': 'grave_accent', '[': 'bracket_left', ']': 'bracket_right',
         '\\': 'backslash'}

s = socket.socket(socket.AF_UNIX); s.connect("qmp.sock"); f = s.makefile("rw"); f.readline()
def cmd(c, a=None):
    f.write(json.dumps({"execute": c, **({"arguments": a} if a else {})}) + "\n"); f.flush()
    while True:
        r = json.loads(f.readline())
        if "return" in r or "error" in r:
            return r
cmd("qmp_capabilities")

def keys(*names):
    r = cmd("send-key", {"keys": [{"type": "qcode", "data": n} for n in names], "hold-time": 60})
    if "error" in r:
        sys.exit(f"send-key {names}: {r['error']}")
    time.sleep(0.06)

def type_text(t):
    for ch in t:
        if ch.isalpha() and ch.isascii():
            keys('shift', ch.lower()) if ch.isupper() else keys(ch)
        elif ch.isdigit():
            keys(ch)
        elif ch in PLAIN:
            keys(PLAIN[ch])
        elif ch in SHIFTED:
            keys('shift', SHIFTED[ch])
        else:
            sys.exit(f"no key for {ch!r}")

mode = sys.argv[1]
if mode == "combo":
    keys(*sys.argv[2:])
elif mode == "text":
    type_text(sys.argv[2])
elif mode == "stdin":
    type_text(sys.stdin.readline().rstrip("\n")); keys('ret')
elif mode == "shot":
    print(cmd("screendump", {"filename": sys.argv[2], "format": "png"}))
