#!/bin/bash
# Download what the OpenCore image is built from into opencore/downloads/ and check each file's SHA256.
#   opencore/fetch-downloads.sh
set -euo pipefail
cd "$(dirname "$0")"; mkdir -p downloads; cd downloads
get() {   # get <file> <sha256> <url>
  [ -f "$1" ] || curl -fsSL -o "$1" "$3"
  echo "$2  $1" | sha256sum -c --quiet || { echo "checksum mismatch: $1 (delete it to download again)"; exit 1; }
  case $1 in *.zip) d=${1%.zip}; [ -d "$d" ] || { mkdir -p "$d" && unzip -q -o "$1" -d "$d"; } ;; esac
  echo "ok  $1"
}
get OpenCore-1.0.8-RELEASE.zip 2011e8b7216ecb2645d97ea710df965947edd406846e92050b9bc4190be6d27b \
  https://github.com/acidanthera/OpenCorePkg/releases/download/1.0.8/OpenCore-1.0.8-RELEASE.zip
get OpenCore-1.0.8-DEBUG.zip 3f5f42b85703cf2c08e8dcd9245a52fb4326c9f4ef335c75b49f516cbbe131c6 \
  https://github.com/acidanthera/OpenCorePkg/releases/download/1.0.8/OpenCore-1.0.8-DEBUG.zip
get Lilu-1.7.2-RELEASE.zip 53967d7dcfaab01023a33df2e969a89522f13d6654a6a56ac4711b62dabf3ab8 \
  https://github.com/acidanthera/Lilu/releases/download/1.7.2/Lilu-1.7.2-RELEASE.zip
get VirtualSMC-1.3.8-RELEASE.zip 2e29a8aaf91b4eb0bcdf5913ed17e70f5cf76531122acd4132eff7813a54de8b \
  https://github.com/acidanthera/VirtualSMC/releases/download/1.3.8/VirtualSMC-1.3.8-RELEASE.zip
# AMFIPass (lets the driver's ad-hoc signed code load with AMFI on); distributed with OpenCore Legacy Patcher
get AMFIPass-v1.4.1-RELEASE.zip 07b266145906db41f4b13a7938fbb173ea28888cc1fa65f84417f8820adc961e \
  https://github.com/dortania/OpenCore-Legacy-Patcher/raw/main/payloads/Kexts/Acidanthera/AMFIPass-v1.4.1-RELEASE.zip
# AMD_Vanilla kernel patches (only for make-config.py --cpu amd)
get AMD_Vanilla-patches.plist 4bc820109b3d020c3c547fa23c49e0098e4f4a2c625ed6184dd54e390e84e1ab \
  https://raw.githubusercontent.com/AMD-OSX/AMD_Vanilla/master/patches.plist
