#!/bin/bash
# Build nullmoth's NVMTLDriver (Metal plugin) from v2.3.3 + our patches in the Tahoe guest, the way
# build/build_plugin_release.sh does (minos 15.0, -DNVMTL_RELEASE=1). Run on the dev VM.
#   guest/nvenc/build-plugin.sh            build stock + patched into guest ~/nvmtl-build/{stock,patched}/NVMTLDriver
# Installing is separate (install-plugin.sh), so a build never touches the running driver.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd); . "$HERE/../../scripts/config.sh"
NM=$REPO/vendor/nvidia-macos-driver; VKH=$REPO/vendor/Vulkan-Headers; REV=${NULLMOTH_REV:-v2.3.3}
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
mkdir -p "$T/stock" "$T/patched"
git -C "$NM" archive "$REV" plugin | tar -x -C "$T/stock"
git -C "$NM" archive "$REV" plugin | tar -x -C "$T/patched"
for p in "$HERE"/0*.patch; do patch -s -d "$T/patched" -p1 < "$p"; echo "applied $(basename "$p")"; done
cp -r "$VKH/include" "$T/vkinclude"
tar -C "$T" -cf - stock patched vkinclude | ssh "$GUEST_SSH" 'rm -rf ~/nvmtl-build && mkdir -p ~/nvmtl-build && tar -xf - -C ~/nvmtl-build && cd ~/nvmtl-build &&
  for v in stock patched; do
    (cd $v/plugin && xcrun clang -arch x86_64 -mmacosx-version-min=15.0 -bundle -fobjc-arc -O1 -DNVMTL_RELEASE=1 \
      -Wno-protocol -Wno-incomplete-implementation -Werror=unguarded-availability-new -I ../../vkinclude -o ../NVMTLDriver NVMTLDevice.m \
      -framework Foundation -framework Metal -framework IOSurface -framework IOKit -framework CoreFoundation 2>../build.err) \
      || { echo "$v: BUILD FAILED"; grep " error:" $v/build.err | head -20; exit 1; }
    codesign -f -s - $v/NVMTLDriver 2>/dev/null
    echo "$v: $(stat -f %z $v/NVMTLDriver) B sha $(shasum -a 256 $v/NVMTLDriver | cut -c1-8) minos $(vtool -show-build $v/NVMTLDriver | awk "/minos/{print \$2; exit}") warnings $(grep -c ": warning:" $v/build.err || true)"
  done
  s=/Library/GPUBundles/NVMTLDriver.bundle/Contents/MacOS/NVMTLDriver
  echo "installed now: $(stat -f %z $s) B sha $(shasum -a 256 $s | cut -c1-8) minos $(vtool -show-build $s | awk "/minos/{print \$2; exit}")"'
