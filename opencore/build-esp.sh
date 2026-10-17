#!/bin/bash
# Assemble the OpenCore EFI tree for the Tahoe VM and pack it into a FAT32 disk image (in the tahoe-tools container).
# Usage: build-esp.sh <out.img> [make-config.py args...]   e.g. build-esp.sh build/OpenCore-intel.img --cpu intel --debug
# --debug also selects OpenCore's DEBUG build. If build/recovery/com.apple.recovery.boot exists it is copied to the
# image root, so the installer boots from the same disk.
set -euo pipefail
cd "$(dirname "$0")"
OUT=$1; shift
docker build -q -t tahoe-tools -f Dockerfile.tools . >/dev/null
D=downloads; REL=RELEASE; [[ " $* " == *" --debug "* ]] && REL=DEBUG
T=build/esp-tree; rm -rf $T; mkdir -p $T/EFI/BOOT $T/EFI/OC/{ACPI,Drivers,Kexts,Tools,Resources}
OCD=$D/OpenCore-1.0.8-$REL/X64/EFI
cp $OCD/BOOT/BOOTx64.efi $T/EFI/BOOT/
# hide our own bootstrap from the picker (otherwise the default entry starts OpenCore again and halts)
printf Disabled > $T/EFI/BOOT/.contentVisibility
cp $OCD/OC/OpenCore.efi $OCD/OC/.contentFlavour $OCD/OC/.contentVisibility $T/EFI/OC/ 2>/dev/null || cp $OCD/OC/OpenCore.efi $T/EFI/OC/
for d in OpenRuntime OpenHfsPlus ResetNvramEntry ToggleSipEntry; do cp $OCD/OC/Drivers/$d.efi $T/EFI/OC/Drivers/; done
cp $OCD/OC/Tools/OpenShell.efi $T/EFI/OC/Tools/
cp ../vendor/OSX-KVM/OpenCore/EFI/OC/ACPI/SSDT-EC.aml $T/EFI/OC/ACPI/   # QEMU fake EC + USBX, from OSX-KVM
cp -r $D/Lilu-1.7.2-RELEASE/Lilu.kext $D/AMFIPass-v1.4.1-RELEASE/AMFIPass.kext $D/VirtualSMC-1.3.8-RELEASE/Kexts/VirtualSMC.kext \
  ../vendor/OSX-KVM/OpenCore/EFI/OC/Kexts/MCEReporterDisabler.kext $T/EFI/OC/Kexts/
python3 make-config.py -o $T/EFI/OC/config.plist "$@"
chmod +x $D/OpenCore-1.0.8-RELEASE/Utilities/ocvalidate/ocvalidate.linux
$D/OpenCore-1.0.8-RELEASE/Utilities/ocvalidate/ocvalidate.linux $T/EFI/OC/config.plist | tail -3
[ -d build/recovery/com.apple.recovery.boot ] && cp -r build/recovery/com.apple.recovery.boot $T/
# GPT disk with one EFI System Partition (1 MiB offset): OpenCore recognises its own disk and hides it from the
# picker, and macOS sees a real ESP (nullmoth's install.sh looks for OpenCore on EFI/FAT32 partitions)
SIZE=$(( $(du -sm $T | cut -f1) + 300 ))
mkdir -p "$(dirname $OUT)"; rm -f $OUT
docker run --rm -v "$PWD:/w" -w /w tahoe-tools sh -c "
  set -e; truncate -s $((SIZE + 2))M $OUT
  echo 'label: gpt
start=2048, type=C12A7328-F81F-11D2-BA4B-00A0C93EC93B, name=\"EFI\"' | sfdisk -q $OUT
  truncate -s ${SIZE}M part.img && mkfs.vfat -F 32 -n OPENCORE part.img >/dev/null
  mcopy -s -i part.img $T/* ::/ && mdir -i part.img ::/ | tail -3
  dd if=part.img of=$OUT bs=1M seek=1 conv=notrunc status=none && rm part.img
  sfdisk -l $OUT | tail -2"
echo "$OUT: ${SIZE} MB"
