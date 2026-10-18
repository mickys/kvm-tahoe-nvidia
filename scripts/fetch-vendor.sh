#!/bin/bash
# Fetch the third-party source trees into vendor/ (gitignored), at the commits this repo was tested with.
#   OSX-KVM                  Tahoe quirks, SSDT-EC.aml and MCEReporterDisabler.kext for opencore/
#   nvidia-macos-driver      nullmoth's driver source (guest/nvenc builds its Metal plugin from tag v2.3.6)
#   open-gpu-kernel-modules  NVIDIA RM headers, 610.57.04 = the RM inside drivers 2.3.3-2.3.6 (guest/nvidia-smi)
#   Vulkan-Headers           for building the Metal plugin
set -euo pipefail
cd "$(dirname "$0")/.."; mkdir -p vendor
clone() {   # clone <dir> <url> <commit-or-tag> [sparse paths...]
  local d=vendor/$1 url=$2 rev=$3; shift 3
  if [ ! -d "$d/.git" ]; then
    if [ $# -gt 0 ]; then git clone -q --filter=blob:none --no-checkout "$url" "$d" && git -C "$d" sparse-checkout set "$@"
    else git clone -q --filter=blob:none --no-checkout "$url" "$d"; fi
  fi
  git -C "$d" fetch -q --tags origin && git -C "$d" checkout -q "$rev" && echo "$1 at $(git -C "$d" describe --tags --always)"
}
clone OSX-KVM https://github.com/kholia/OSX-KVM.git 4c378a4b5e0b219783683012bec680325eb40719
clone nvidia-macos-driver https://github.com/nullmoth/nvidia-macos-driver.git v2.3.6
clone open-gpu-kernel-modules https://github.com/NVIDIA/open-gpu-kernel-modules.git 610.57.04 \
  src/common/sdk/nvidia/inc src/nvidia/arch/nvalloc/unix/include kernel-open/common/inc
clone Vulkan-Headers https://github.com/KhronosGroup/Vulkan-Headers.git acbb9391c0a597272800cd3f4363661c00a27c77
