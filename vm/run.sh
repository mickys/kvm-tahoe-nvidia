#!/bin/bash
# Run the Tahoe VM as a plain QEMU process on the KVM host (as root). Everything the VM writes (pidfile, QMP/HMP
# sockets, serial.log, qemu.log, OVMF vars) lives in VM_DIR. Settings: config.env in the repo root.
# (libvirt instead: libvirt/README.md. Don't run both at once: they use the same disks.)
#
#   vm/run.sh start | stop | reset | kill | status
#
# config.env (or the environment) picks: CPU=intel|amd, OC (OpenCore image), MEM, SMP, CORES, GPU_PASSTHROUGH=0|1,
# GPU / GPU_AUDIO, GPU_BAR1_LOG2MB, LAN_LISTEN, SSH_FWD_PORT, SCREENSHARING_FWD_PORT, VNC_DISPLAY. Extra variables:
#   GPU_TOPO=port|rootbus   with the GPU: behind a pcie-root-port (default; the driver moves BAR1 through it, which
#                           the display needs) or directly on pcie.0 (driver loads and Metal works, no display)
#   GPU_ROMBAR=1|0          with GPU_TOPO=rootbus: 0 hides the GPU's option ROM (no UEFI GOP boot console)
#   VNC_LISTEN=0.0.0.0      without the GPU: where QEMU's VNC listens (password in VM_DIR/vnc-password)
#   QEMU_EXTRA="..."        extra QEMU arguments (experiments, traces)
#
# Remote access: guest SSH on host 127.0.0.1:SSH_FWD_PORT; macOS Screen Sharing on 127.0.0.1:SCREENSHARING_FWD_PORT
# and on LAN_LISTEN:SCREENSHARING_FWD_PORT through tcp-forward.py --vnc-password-only (docs/05-remote-access.md).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/../scripts/config.sh"
cd "$VM_DIR"
Q=$QEMU_DIR
CPU=${CPU:-amd}; OC=${OC:-OpenCore-amd-gpu-debug.img}; MEM=${MEM:-32G}; SMP=${SMP:-16}; CORES=${CORES:-8}
GPU_PASSTHROUGH=${GPU_PASSTHROUGH:-0}; VNC_LISTEN=${VNC_LISTEN:-0.0.0.0}; DISK=${DISK:-tahoe.qcow2}
MAC=$(python3 -c "import json;print(json.load(open('$REPO/smbios.json'))['ROM_and_NIC_MAC'])")

qmp() {   # qmp <command> [json-arguments]
  python3 - "$1" "${2:-}" <<'PY'
import json, socket, sys
s = socket.socket(socket.AF_UNIX); s.connect("qmp.sock"); f = s.makefile("rw"); f.readline()
def cmd(c, a=None):
    f.write(json.dumps({"execute": c, **({"arguments": a} if a else {})}) + "\n"); f.flush()
    while True:
        r = json.loads(f.readline())
        if "return" in r or "error" in r: return r
cmd("qmp_capabilities"); print(json.dumps(cmd(sys.argv[1], json.loads(sys.argv[2]) if sys.argv[2] else None)))
PY
}
running() { [ -f qemu.pid ] && kill -0 "$(cat qemu.pid)" 2>/dev/null; }

case "${1:-}" in
start)
  running && { echo "already running (pid $(cat qemu.pid))"; exit 1; }
  case $CPU in
    intel) CPUARG="Skylake-Client,-hle,-rtm,-pcid,-invpcid,-erms,vendor=GenuineIntel,+invtsc,vmware-cpuid-freq=on,kvm=on" ;;
    # host-phys-bits=off,phys-bits=40: otherwise the guest sees the raw CPUID width (48 on Zen 2, of which 43 work),
    # the PCI window lands far up and the driver can't place BAR1 (no display, WindowServer crash loop).
    # topoext: threads=2 on AMD. -kvm-pv-eoi,-kvm-pv-unhalt: with them every AMD boot panicked in IOKit matching.
    amd)   CPUARG="host,host-phys-bits=off,phys-bits=40,+topoext,+invtsc,vmware-cpuid-freq=on,kvm=on,-kvm-pv-eoi,-kvm-pv-unhalt" ;;
    *) echo "CPU must be intel or amd"; exit 1 ;;
  esac
  # an OpenCore image staged while the VM was running (it holds the image open) replaces the old one now
  [ -f "$OC.new" ] && mv "$OC.new" "$OC" && echo "using the newly staged $OC"
  [ -f "$OC" ] || { echo "missing $VM_DIR/$OC"; exit 1; }
  [ -f "$DISK" ] || $Q/bin/qemu-img create -q -f qcow2 "$DISK" 256G
  pkill -f "tcp-forward.py --vnc-password-only 127.0.0.1:$SCREENSHARING_FWD_PORT" 2>/dev/null || true
  [ -f vnc-password ] || (umask 077; python3 -c "import secrets as s; print(''.join(s.choice('ABCDEFGHJKLMNPQRSTUVWXYZabcdefghjkmnpqrstuvwxyz23456789') for _ in range(8)), end='')" > vnc-password)
  [ -f OVMF_VARS.fd ] || cp $Q/share/qemu/edk2-i386-vars.fd OVMF_VARS.fd
  args=(
    -name tahoe,debug-threads=on -machine q35,accel=kvm -cpu "$CPUARG"
    -m "$MEM" -smp "$SMP",sockets=1,cores="$CORES",threads=$((SMP / CORES))
    -drive if=pflash,format=raw,readonly=on,file=$Q/share/qemu/edk2-x86_64-code.fd
    -drive if=pflash,format=raw,file=OVMF_VARS.fd
    -device isa-applesmc,osk="ourhardworkbythesewordsguardedpleasedontsteal(c)AppleComputerInc"
    -smbios type=2
    -device qemu-xhci,id=xhci -device usb-kbd,bus=xhci.0 -device usb-tablet,bus=xhci.0
    -device ich9-ahci,id=sata
    -drive id=oc,if=none,format=raw,file="$OC" -device ide-hd,bus=sata.0,drive=oc,bootindex=0
    -drive id=hd,if=none,format=qcow2,file="$DISK",discard=unmap,detect-zeroes=unmap
    -device ide-hd,bus=sata.1,drive=hd,rotation_rate=1
    -netdev user,id=net0,hostfwd=tcp:127.0.0.1:$SSH_FWD_PORT-:22,hostfwd=tcp:127.0.0.1:$SCREENSHARING_FWD_PORT-:5900
    -device virtio-net-pci,netdev=net0,mac="$MAC"
    -qmp unix:qmp.sock,server=on,wait=off -monitor unix:hmp.sock,server=on,wait=off
    -serial file:serial.log -pidfile qemu.pid
  )
  if [ "$GPU_PASSTHROUGH" = 1 ]; then
    for d in "$GPU" "$GPU_AUDIO"; do
      [ "$(basename "$(readlink /sys/bus/pci/devices/$d/driver)")" = vfio-pci ] || { echo "$d is not on vfio-pci"; exit 1; }
    done
    grp=$(basename "$(readlink /sys/bus/pci/devices/$GPU/iommu_group)")
    fuser -s /dev/vfio/$grp && { echo "/dev/vfio/$grp is in use (another VM has the GPU?)"; exit 1; }
    # BAR1 size: the guest can't resize it (QEMU exposes only the current Resizable BAR size), so set it here.
    # sysfs resizing needs Linux >= 5.13 and no driver bound, hence the unbind/rebind of vfio-pci around it.
    G=/sys/bus/pci/devices/$GPU; want=${GPU_BAR1_LOG2MB:-12}
    cur=$(python3 -c "l=open('$G/resource').read().split(chr(10))[1].split(); print((int(l[1],16)-int(l[0],16)+1).bit_length()-21)")
    if [ "$cur" != "$want" ]; then
      echo "$GPU" > $G/driver/unbind && echo "$want" > $G/resource1_resize
      echo "$GPU" > /sys/bus/pci/drivers/vfio-pci/bind
      echo "BAR1 resized: 2^$cur MB -> 2^$want MB"
    fi
    ulimit -l unlimited   # VFIO pins all guest RAM
    # a 64 GB PCI hole and OVMF's 64-bit MMIO window big enough for BAR1
    args+=(-vga none -display none
           -global q35-pcihost.pci-hole64-size=64G -fw_cfg name=opt/ovmf/X-PciMmio64Mb,string=65536)
    if [ "${GPU_TOPO:-port}" = port ]; then
      # Behind a PCIe root port, as on bare metal. q35's ACPI PCI hot-plug on bridges (default since QEMU 6.1) replaces
      # the root port's native slot handler, so presence-detect and link-active stay 0 and macOS sees an empty slot.
      args+=(-global ICH9-LPC.acpi-pci-hotplug-with-bridge-support=off
             -device pcie-root-port,id=rp1,chassis=1,slot=1,hotplug=off,power_controller_present=off
             -device vfio-pci,host=$GPU,bus=rp1,addr=0x0.0x0,multifunction=on
             -device vfio-pci,host=$GPU_AUDIO,bus=rp1,addr=0x0.0x1)
    else
      args+=(-device vfio-pci,host=$GPU,bus=pcie.0,addr=0x10.0x0,multifunction=on,rombar=${GPU_ROMBAR:-1}
             -device vfio-pci,host=$GPU_AUDIO,bus=pcie.0,addr=0x10.0x1)
    fi
  else
    args+=(-vga none -device vmware-svga -vnc "$VNC_LISTEN":"$VNC_DISPLAY",password=on)
  fi
  [ -n "${QEMU_EXTRA:-}" ] && read -r -a extra <<< "$QEMU_EXTRA" && args+=("${extra[@]}")
  echo "$(date -Is) start CPU=$CPU OC=$OC MEM=$MEM SMP=$SMP GPU_PASSTHROUGH=$GPU_PASSTHROUGH" >> run.log
  # not -daemonize: that sends QEMU's stderr to /dev/null after startup, and runtime errors (a vfio abort) get lost
  rm -f qemu.pid
  echo "=== $(date -Is) start" >> qemu.log
  setsid nohup $Q/bin/qemu-system-x86_64 "${args[@]}" >> qemu.log 2>&1 < /dev/null &
  for i in $(seq 1 50); do [ -s qemu.pid ] && break; kill -0 $! 2>/dev/null || { tail -5 qemu.log; exit 1; }; sleep 0.2; done
  [ -s qemu.pid ] || { echo "QEMU did not start"; tail -5 qemu.log; exit 1; }
  for i in $(seq 1 25); do [ -S qmp.sock ] && qmp query-status >/dev/null 2>&1 && break; sleep 0.2; done
  listen=(); for a in $LAN_LISTEN; do listen+=("$a:$SCREENSHARING_FWD_PORT"); done
  [ ${#listen[@]} -gt 0 ] && (setsid nohup python3 "$HERE/tcp-forward.py" --vnc-password-only \
     "127.0.0.1:$SCREENSHARING_FWD_PORT" "${listen[@]}" > tcp-forward.log 2>&1 < /dev/null &)
  if [ "$GPU_PASSTHROUGH" != 1 ]; then
    qmp change-vnc-password "{\"password\": \"$(cat vnc-password)\"}" >/dev/null
    echo "started, pid $(cat qemu.pid). VNC $VNC_LISTEN:$((5900 + VNC_DISPLAY)) (password in $VM_DIR/vnc-password)"
  else
    echo "started, pid $(cat qemu.pid), GPU passthrough (no emulated display). serial log: $VM_DIR/serial.log"
  fi
  ;;
reset) running && qmp system_reset && echo "reset sent" || echo "not running" ;;   # for a hung macOS restart
stop)  running && qmp system_powerdown && echo "ACPI power button sent" || echo "not running" ;;
kill)  running && { qmp quit >/dev/null || kill "$(cat qemu.pid)"; echo "stopped"; } || echo "not running"
       pkill -f "tcp-forward.py --vnc-password-only 127.0.0.1:$SCREENSHARING_FWD_PORT" 2>/dev/null || true ;;
status)
  if running; then echo "running, pid $(cat qemu.pid)"; qmp query-status; else echo "not running"; fi ;;
*) sed -n '2,19p' "$0"; exit 1 ;;
esac
