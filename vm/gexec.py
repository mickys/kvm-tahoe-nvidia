# run a shell command in a libvirt guest through qemu-guest-agent (guest-exec), print its output
import json, subprocess, sys, time, base64
dom, cmd = sys.argv[1], sys.argv[2]
def agent(o):
    r = subprocess.run(["virsh", "qemu-agent-command", dom, json.dumps(o)], capture_output=True, text=True)
    if r.returncode: sys.exit(r.stderr.strip())
    return json.loads(r.stdout)["return"]
pid = agent({"execute": "guest-exec", "arguments": {"path": "/bin/sh", "arg": ["-c", cmd], "capture-output": True}})["pid"]
for _ in range(120):
    st = agent({"execute": "guest-exec-status", "arguments": {"pid": pid}})
    if st.get("exited"): break
    time.sleep(0.5)
for k in ("out-data", "err-data"):
    if k in st: sys.stdout.write(base64.b64decode(st[k]).decode(errors="replace"))
print(f"[exit {st.get('exitcode')}]")
