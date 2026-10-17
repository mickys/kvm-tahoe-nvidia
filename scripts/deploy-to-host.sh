#!/bin/bash
# Copy this repo (with config.env and smbios.json, without vendor/ and build output) to the KVM host, where vm/run.sh
# and the libvirt hook run from. No rsync needed (tar through ssh).
#   scripts/deploy-to-host.sh [dir on the host, default /opt/kvm-tahoe-nvidia]
set -euo pipefail
. "$(dirname "$0")/config.sh"
DST=${1:-/opt/kvm-tahoe-nvidia}
tar -C "$REPO" --exclude=./vendor --exclude=./.git --exclude=./opencore/build --exclude=./opencore/downloads \
    --exclude=./host/qemu-build/qemu --exclude=./host/qemu-build/deps --exclude=./host/qemu-build/logs -cf - . \
  | ssh "$KVM_HOST" "mkdir -p '$DST' && tar -xf - -C '$DST' && chmod 600 '$DST/config.env' '$DST/smbios.json' 2>/dev/null; echo deployed to $KVM_HOST:$DST"
