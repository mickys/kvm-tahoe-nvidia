#!/bin/bash
# Render the libvirt domain XML and the libvirt hook from config.env + smbios.json into libvirt/out/ (gitignored).
#   libvirt/render.sh            -> libvirt/out/$VM_NAME.xml and libvirt/out/qemu (the hook)
# REPO_ON_HOST (default: this repo's path) is where the repo lives on the KVM host; the hook runs vm/tcp-forward.py
# from there.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/../scripts/config.sh"
export REPO REPO_ON_HOST=${REPO_ON_HOST:-$REPO}
mkdir -p "$HERE/out"
python3 - "$HERE" <<'PY'
import json, os, re, sys
here = sys.argv[1]; e = os.environ
cfg = {}
for line in open(os.path.join(e["REPO"], "config.env")):      # same values bash saw (config.env is plain KEY=value)
    m = re.match(r'\s*([A-Z_][A-Z0-9_]*)=("([^"]*)"|(\S*))', line)
    if m: cfg[m.group(1)] = m.group(3) if m.group(3) is not None else m.group(4)
def bdf(s):
    d, b, sf = s.split(":"); sl, f = sf.split(".")
    return d, b, sl, f
mem = cfg.get("MEM", "32G"); unit = mem[-1].upper()
kib = int(mem[:-1]) * {"G": 1024 * 1024, "M": 1024}[unit] if unit in "GM" else int(mem) // 1024
smp, cores = int(cfg.get("SMP", 16)), int(cfg.get("CORES", 8))
v = dict(cfg)
v.update(MEM_KIB=str(kib), THREADS=str(smp // cores), REPO=e["REPO_ON_HOST"],
         MAC=json.load(open(os.path.join(e["REPO"], "smbios.json")))["ROM_and_NIC_MAC"])
for k, s in (("GPU", cfg["GPU"]), ("AUD", cfg["GPU_AUDIO"])):
    d, b, sl, f = bdf(s); v.update({k + "_DOMAIN": d, k + "_BUS": b, k + "_SLOT": sl, k + "_FUNC": f})
v.setdefault("GPU_BAR1_RESTORE_LOG2MB", "")
def render(src, dst, mode=0o644):
    t = open(os.path.join(here, src)).read()
    missing = sorted(set(re.findall(r"@([A-Z0-9_]+)@", t)) - set(v))
    if missing: sys.exit(f"{src}: no value for {missing}")
    open(dst, "w").write(re.sub(r"@([A-Z0-9_]+)@", lambda m: v[m.group(1)], t)); os.chmod(dst, mode); print(dst)
render("tahoe.xml.in", os.path.join(here, "out", cfg["VM_NAME"] + ".xml"))
render("hooks/qemu.in", os.path.join(here, "out", "qemu"), 0o755)
PY
