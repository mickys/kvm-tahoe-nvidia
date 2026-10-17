#!/bin/bash
# Check the installed QEMU on the KVM host: no library may be missing or come from the system (other than glibc), and with
# LD_BIND_NOW every symbol of every binary must resolve at load time. Run by build.sh and deploy-opt.sh; can be run
# on its own. Usage: verify-on-host.sh [prefix on the host]
set -euo pipefail
cd "$(dirname "$0")"; . ../../scripts/config.sh
PREFIX=${1:-$QEMU_DIR}
ssh "$KVM_HOST" "PREFIX=$PREFIX bash -s" <<'REMOTE'
set -u; shopt -s extglob; cd $PREFIX; bad=0
for f in bin/* libexec/*; do
  file -L $f | grep -q ELF || continue
  ldd $f | grep 'not found' && { echo "missing library: $f"; bad=1; }
  for l in $(ldd $f | awk '/=> \//{print $3}' | xargs -r readlink -f); do
    case $l in
      $PREFIX/lib/*) ;;
      */lib/x86_64-linux-gnu/lib@(c|m|pthread|dl|rt|resolv|util|anl)-2.+([0-9]).so) ;;  # glibc
      *) echo "system library: $f -> $l"; bad=1 ;;
    esac
  done
done
for f in bin/qemu-system-* bin/qemu-img bin/qemu-io bin/qemu-nbd bin/qemu-storage-daemon; do
  LD_BIND_NOW=1 $f --version >/dev/null 2>&1 || { echo "does not start: $f"; bad=1; }
done
echo "$(ls bin/qemu-system-* | wc -l) system emulators, $(ls lib | wc -l) bundled libraries, $(du -sh . | cut -f1)"
bin/qemu-system-x86_64 --version | head -1
exit $bad
REMOTE
