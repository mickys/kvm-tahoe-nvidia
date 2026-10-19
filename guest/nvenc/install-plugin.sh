#!/bin/bash
# Install the plugin built by build-plugin.sh into the Tahoe guest, or put the shipped one back. Run on the dev VM.
#   install-plugin.sh patched|stock     install ~/nvmtl-build/<v>/NVMTLDriver (shipped binary backed up once per driver version)
#   install-plugin.sh restore           shipped NVMTLDriver + original nvrm610.conf of the installed driver version back
#   install-plugin.sh knob NAME=VALUE   set a knob in nvrm610.conf (e.g. NVMTL_NVENCVT=1 for every app, or a process list); knob NAME= removes it
# New apps pick the plugin up at once; WindowServer and running apps after a logout/login (relogin.sh).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd); . "$HERE/../../scripts/config.sh"
what=${1:?patched|stock|restore|knob}; arg=${2:-}
# macOS sudo -S reads more than one line from a pipe, so the script goes over first and the password alone on stdin
cat <<'REMOTE' | ssh "$GUEST_SSH" 'cat > /tmp/nvenc-install.sh'
set -euo pipefail
what=$1; arg=$2
B=/Library/GPUBundles/NVMTLDriver.bundle; X=$B/Contents/MacOS/NVMTLDriver; C=/Library/GPUBundles/nvmtl/nvrm610.conf
K=/Library/NullMoth/nvenc-work; mkdir -p $K
# per driver version: the plugin and the kernel driver share interfaces, so a restore must never cross versions
V=$(cat /Library/NullMoth/driver-version); S=$K/NVMTLDriver.shipped-$V; SC=$K/nvrm610.conf.shipped-$V
[ -f $S ] || { cp -p $X $S; cp -p $C $SC; echo "backed up the $V plugin $(shasum -a 256 $X | cut -c1-8) and nvrm610.conf to $K"; }
case $what in
  patched|stock) install -o root -g wheel -m 755 $(eval echo ~$SUDO_USER)/nvmtl-build/$what/NVMTLDriver $X ;;
  restore) install -o root -g wheel -m 755 $S $X; cp -p $SC $C; echo "restored the $V plugin and nvrm610.conf" ;;
  knob) n=${arg%%=*}; v=${arg#*=}; grep -v "^$n=" $C > $C.new || true; [ -z "$v" ] || echo "$n=$v" >> $C.new
        chown root:wheel $C.new; chmod 644 $C.new; mv $C.new $C; echo "nvrm610.conf: $(grep -c . $C) lines, $n=${v:-<removed>}"; exit 0 ;;
  *) echo "unknown $what"; exit 2 ;;
esac
codesign -f -s - --preserve-metadata=identifier $B 2>&1 | grep -v "replacing existing signature" || true
codesign -v $B && echo "bundle signature ok"
echo "installed $what: $(shasum -a 256 $X | cut -c1-8)"
REMOTE
"$REPO/vm/ssh-askpass-env.sh" | ssh "$GUEST_SSH" "sudo -S -p '' /bin/bash /tmp/nvenc-install.sh '$what' '$arg'; rm -f /tmp/nvenc-install.sh"
