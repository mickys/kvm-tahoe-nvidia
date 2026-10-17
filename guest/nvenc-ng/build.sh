#!/bin/bash
# Build the nvenc-ng tools in the guest (macOS SDK needed). Run on the workstation: guest/nvenc-ng/build.sh [tool ...]
# Headers: NVIDIA open-gpu-kernel-modules (RM API) and open-gpu-doc (video classes), both from scripts/fetch-vendor.sh.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd); . "$HERE/../../scripts/config.sh"
OGKM=$REPO/vendor/open-gpu-kernel-modules; OGD=$REPO/vendor/open-gpu-doc
tools=${*:-probe}
tar -C "$OGKM" -cf - src/common/sdk/nvidia/inc src/nvidia/arch/nvalloc/unix/include \
  | ssh "$GUEST_SSH" 'rm -rf ~/nvenc-ng/inc && mkdir -p ~/nvenc-ng/inc/ogkm && tar -xf - -C ~/nvenc-ng/inc/ogkm'
tar -C "$OGD" -cf - classes/video | ssh "$GUEST_SSH" 'mkdir -p ~/nvenc-ng/inc/ogd && tar -xf - -C ~/nvenc-ng/inc/ogd'
tar -C "$HERE" -cf - --exclude=build.sh . | ssh "$GUEST_SSH" "cd ~/nvenc-ng && tar -xf - && for t in $tools; do
  clang -O2 -Wall -Wno-unused-function -o \$t \$t.c rm.c -I inc/ogkm/src/common/sdk/nvidia/inc -I inc/ogkm/src/nvidia/arch/nvalloc/unix/include \
    -I inc/ogd/classes/video -framework IOKit -framework CoreFoundation 2>&1 | grep -E 'error|warning' ; ls -la \$t | awk '{print \$5, \$NF}'; done"
