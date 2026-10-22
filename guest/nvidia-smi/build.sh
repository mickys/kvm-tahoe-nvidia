#!/bin/bash
# Build nvidia-smi.c in the Tahoe guest (it needs the macOS SDK) and install it as /usr/local/bin/nvidia-smi.
# Run on the workstation: guest/nvidia-smi/build.sh [--no-install]
# Headers: NVIDIA open-gpu-kernel-modules 615.78.08 = the RM of NullMoth 2.5.x (vendor/open-gpu-kernel-modules-615, sparse checkout).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd); . "$HERE/../../scripts/config.sh"
OGKM=$REPO/vendor/open-gpu-kernel-modules-615
[ -d "$OGKM/src/common/sdk/nvidia/inc" ] || {
  git clone -q --depth 1 --branch 615.78.08 --filter=blob:none --sparse https://github.com/NVIDIA/open-gpu-kernel-modules.git "$OGKM"
  git -C "$OGKM" sparse-checkout set src/common/sdk/nvidia/inc src/nvidia/arch/nvalloc/unix/include kernel-open/common/inc; }
tar -C "$OGKM" -cf - src/common/sdk/nvidia/inc src/nvidia/arch/nvalloc/unix/include -C "$HERE" nvidia-smi.c |
  ssh "$GUEST_SSH" 'rm -rf ~/nvsmi-build && mkdir -p ~/nvsmi-build && tar -xf - -C ~/nvsmi-build && cd ~/nvsmi-build &&
    clang -O2 -Wall -Wno-unused-function -o nvidia-smi nvidia-smi.c \
      -I src/common/sdk/nvidia/inc -I src/nvidia/arch/nvalloc/unix/include -framework IOKit -framework CoreFoundation 2>&1 | grep -v "^nvrm:"; ls -la nvidia-smi'
if [ "${1:-}" != "--no-install" ]; then
  "$REPO/vm/ssh-askpass-env.sh" | ssh "$GUEST_SSH" 'sudo -S -p "" install -m 755 ~/nvsmi-build/nvidia-smi /usr/local/bin/nvidia-smi && echo installed /usr/local/bin/nvidia-smi'
fi
