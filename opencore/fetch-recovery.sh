#!/bin/bash
# Download the macOS Tahoe recovery image with OpenCore's macrecovery.py into build/recovery/com.apple.recovery.boot,
# which build-esp.sh copies onto the OpenCore image (the installer then boots from the same disk).
# Board Mac-CFF7D910A743CAAF + "latest" = macOS 26 at the time of writing; macrecovery checks the chunklist.
set -euo pipefail
cd "$(dirname "$0")"
M=downloads/OpenCore-1.0.8-RELEASE/Utilities/macrecovery/macrecovery.py
[ -f $M ] || { echo "run fetch-downloads.sh first"; exit 1; }
mkdir -p build/recovery
python3 $M -b Mac-CFF7D910A743CAAF -m 00000000000000000 -os latest -o build/recovery/com.apple.recovery.boot download
ls -la build/recovery/com.apple.recovery.boot
