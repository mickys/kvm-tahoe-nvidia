#!/bin/bash
# On the KVM host, as root, from the deployed repo: install the libvirt hook and define the domain.
#   libvirt/install.sh            (run libvirt/render.sh first, on the workstation or here)
# A hook file that didn't exist when libvirtd started is only picked up after a libvirtd restart; running domains
# keep running through that restart.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd); . "$HERE/../scripts/config.sh"
[ -f "$HERE/out/$VM_NAME.xml" ] && [ -f "$HERE/out/qemu" ] || { echo "run libvirt/render.sh first"; exit 1; }
H=/etc/libvirt/hooks/qemu; new_hook=0
mkdir -p /etc/libvirt/hooks
if [ -f $H ] && ! cmp -s $H "$HERE/out/qemu"; then
  grep -q "$VM_NAME" $H || { echo "$H exists and is not ours: merge libvirt/out/qemu into it by hand"; exit 1; }
  cp -p $H $H.bak.$(date +%Y%m%d%H%M%S)
fi
[ -f $H ] || new_hook=1
install -m 755 "$HERE/out/qemu" $H
virsh define "$HERE/out/$VM_NAME.xml"
[ $new_hook = 1 ] && echo "new hook installed: restart libvirtd before the first start (systemctl restart libvirtd)"
echo "start with: virsh start $VM_NAME   (stop: virsh shutdown $VM_NAME)"
