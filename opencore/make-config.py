#!/usr/bin/env python3
"""Generate OpenCore config.plist for the Tahoe VM on edi.

Base: OpenCore 1.0.8 Docs/Sample.plist. Quirks for booting Tahoe on QEMU/KVM come from OSX-KVM's config.plist
(kholia/OSX-KVM 4c378a4 "Support for macOS Tahoe"). nullmoth's driver needs SecureBootModel=Disabled,
csr-active-config 0x0A43, Lilu + AMFIPass, and (stage gpu) its boot-args and the IONDRVSupport block.

  --cpu intel   install/first boots: QEMU presents an Intel CPU model (vendor=GenuineIntel), no AMD patches
  --cpu amd     later: -cpu host (Zen 2 visible to macOS) + AMD_Vanilla kernel patches (--cores = cores= of -smp)
  --stage install|gpu   install: no NVIDIA settings; gpu: nullmoth boot-args, Kernel>Block IONDRVSupport
  --debug       verbose boot, kernel log and OpenCore DEBUG log to serial (use with the DEBUG OpenCore build)
"""
import argparse, json, plistlib, sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
OC = HERE / "downloads/OpenCore-1.0.8-RELEASE"
OSXKVM = HERE.parent / "vendor/OSX-KVM/OpenCore/config.plist"
APPLE = "7C436110-AB2A-4BBB-A880-FE41995C9F82"

ap = argparse.ArgumentParser()
ap.add_argument("--cpu", choices=["intel", "amd"], required=True)
ap.add_argument("--cores", type=int, default=8, help="cores per package (the cores= of -smp), for the AMD_Vanilla core-count patches")
ap.add_argument("--stage", choices=["install", "gpu"], default="install")
ap.add_argument("--debug", action="store_true")
ap.add_argument("--amd-patches", type=Path, help="AMD_Vanilla patches.plist (required for --cpu amd)")
ap.add_argument("--stock-pat", action="store_true", help="with --cpu amd: keep AMD_Vanilla's 'Fix PAT' patches as shipped "
                "(PAT entry 6 = uncached-minus, so every write-combined mapping in macOS runs uncached). By default the "
                "patches are changed to keep entry 6 write-combining, as XNU itself sets it")
ap.add_argument("-o", "--output", type=Path, required=True)
a = ap.parse_args()

c = plistlib.load(open(OC / "Docs/Sample.plist", "rb"))
kvm = plistlib.load(open(OSXKVM, "rb"))
smb = json.load(open(HERE.parent / "smbios.json"))   # repo root, gitignored (smbios.example.json)

def take_quirks(section):
    """Sample's quirk keys, with OSX-KVM's value wherever OSX-KVM sets that key."""
    q = c[section]["Quirks"]
    for k, v in kvm[section]["Quirks"].items():
        if k in q:
            q[k] = v

# ACPI: only the QEMU fake EC + USBX (from OSX-KVM, paths under \_SB.PCI0 as on q35)
c["ACPI"]["Add"] = [{"Comment": "EC + USBX for QEMU q35 (from OSX-KVM)", "Enabled": True, "Path": "SSDT-EC.aml"}]
c["ACPI"]["Delete"] = []
c["ACPI"]["Patch"] = []

# Booter: OSX-KVM's quirks. ResizeAppleGpuBars: -1 = leave the BAR size macOS sees alone (QEMU's vfio exposes only
# the host's current ReBAR size anyway).
c["Booter"]["MmioWhitelist"] = []
c["Booter"]["Patch"] = []
take_quirks("Booter")
c["Booter"]["Quirks"]["ResizeAppleGpuBars"] = -1

c["DeviceProperties"]["Add"] = {}
c["DeviceProperties"]["Delete"] = {}

# Kernel
# entries are built from Sample.plist's own entries, so keys added by newer OpenCore versions are present
KEXT_T, DRV_T, TOOL_T = dict(c["Kernel"]["Add"][0]), dict(c["UEFI"]["Drivers"][0]), dict(c["Misc"]["Tools"][0])
def kext(path, exe, comment, min_kernel=""):
    return {**KEXT_T, "Arch": "x86_64", "BundlePath": path, "Comment": comment, "Enabled": True,
            "ExecutablePath": exe, "MaxKernel": "", "MinKernel": min_kernel, "PlistPath": "Contents/Info.plist"}
c["Kernel"]["Add"] = [
    kext("Lilu.kext", "Contents/MacOS/Lilu", "Lilu 1.7.2"),
    kext("VirtualSMC.kext", "Contents/MacOS/VirtualSMC", "VirtualSMC 1.3.8"),
    kext("AMFIPass.kext", "Contents/MacOS/AMFIPass", "AMFIPass 1.4.1 (nullmoth driver)"),
    kext("MCEReporterDisabler.kext", "", "AppleMCEReporter off (MacPro7,1; required on AMD)"),
]
c["Kernel"]["Block"] = []
if a.stage == "gpu":
    # nullmoth: otherwise the firmware framebuffer takes display index 0 from NVRMFB
    c["Kernel"]["Block"] = [{"Arch": "Any", "Comment": "nullmoth: no IONDRVSupport", "Enabled": True,
                             "Identifier": "com.apple.iokit.IONDRVSupport", "MaxKernel": "", "MinKernel": "",
                             "Strategy": "Exclude"}]
c["Kernel"]["Force"] = []
take_quirks("Kernel")
c["Kernel"]["Emulate"]["DummyPowerManagement"] = True
c["Kernel"]["Emulate"]["Cpuid1Data"] = b""
c["Kernel"]["Emulate"]["Cpuid1Mask"] = b""
# OSX-KVM's TRIM patch for QEMU SATA disks
c["Kernel"]["Patch"] = [p for p in kvm["Kernel"]["Patch"] if p.get("Comment", "").startswith("Enable TRIM")]
if a.cpu == "amd":
    if not a.amd_patches:
        sys.exit("--cpu amd needs --amd-patches (AMD_Vanilla patches.plist)")
    amd = plistlib.load(open(a.amd_patches, "rb"))["Kernel"]["Patch"]
    cores = a.cores.to_bytes(1, "little")
    for p in amd:
        # AMD_Vanilla: the "Force cpuid_cores_per_package" patches carry the core count in their Replace data
        # (the byte after the B8/BA/... opcode, 0x00 in the template)
        if "cpuid_cores_per_package" in p.get("Comment", ""):
            r = bytearray(p["Replace"])
            r[1] = cores[0]
            p["Replace"] = bytes(r)
        # AMD_Vanilla: "_mtrr_update_action | Fix PAT" replaces XNU's PA6 := WC with a fixed PAT 0x0007010600070106
        # (mov eax/edx, 0x00070106; wrmsr). XNU maps write-combined memory through PAT entry 6, so with it every WC
        # mapping (the GPU's BAR1 included) runs uncached-minus. Write edx = 0x00010106 instead: PA4 WB, PA5 WC,
        # PA6 WC, PA7 UC (PA0-PA3 unchanged). Measured: 4K CPU copy into VRAM through BAR1 86 ms -> 3-5.5 ms.
        if not a.stock_pat and "Fix PAT" in p.get("Comment", ""):
            r = bytes(p["Replace"]); old = bytes.fromhex("ba06010700")
            if old in r:
                p["Replace"] = r.replace(old, bytes.fromhex("ba06010100"))
    c["Kernel"]["Patch"] += amd
    c["Kernel"]["Quirks"]["ProvideCurrentCpuInfo"] = True   # AMD_Vanilla requires it
c["Kernel"]["Quirks"]["PanicNoKextDump"] = True
c["Kernel"]["Quirks"]["PowerTimeoutKernelPanic"] = True

# Misc
c["Misc"]["Boot"].update({"PickerMode": "Builtin", "ShowPicker": True, "Timeout": 5, "HideAuxiliary": False,
                          "PollAppleHotKeys": True})
c["Misc"]["Entries"] = []
c["Misc"]["Tools"] = [{**TOOL_T, "Arguments": "", "Auxiliary": True, "Comment": "OpenShell", "Enabled": True,
                       "Flavour": "OpenShell:UEFIShell:Shell", "FullNvramAccess": False, "Name": "UEFI Shell",
                       "Path": "OpenShell.efi", "RealPath": False, "TextMode": False}]
c["Misc"]["Security"].update({"SecureBootModel": "Disabled", "Vault": "Optional", "ScanPolicy": 0,
                              "DmgLoading": "Signed", "AllowSetDefault": True, "BlacklistAppleUpdate": True})
if a.debug:
    # 0x49 = logging on + serial + file on the ESP; serial needs Misc>Serial>Init
    c["Misc"]["Debug"].update({"Target": 0x49, "AppleDebug": True, "ApplePanic": True, "DisableWatchDog": True})
    c["Misc"]["Serial"]["Init"] = True

# NVRAM
boot_args = ["keepsyms=1"]
if a.debug:
    boot_args += ["-v", "debug=0x100", "serial=3"]
if a.stage == "gpu":
    boot_args += ["nvfb=1", "nvaccel=1", "nvfbheads=4", "-nvkmsnosmooth", "amfi_get_out_of_my_way=0x1",
                  "amfi=0x80", "ipc_control_port_options=0",
                  # copy path instead of zero-copy scan-out: with zero-copy, macOS screen capture (Screen Sharing,
                  # the only way to see this headless VM's desktop) reads black. Same as sysctl debug.nvaccel_iop_flip=0
                  "-nvrmnoflip"]
nv = c["NVRAM"]["Add"][APPLE]
nv["boot-args"] = " ".join(boot_args)
nv["csr-active-config"] = bytes.fromhex("430A0000")   # nullmoth's tested SIP value
nv["prev-lang:kbd"] = b"en-US:0"
nv["run-efi-updater"] = "No"
dele = c["NVRAM"]["Delete"].setdefault(APPLE, [])
for k in ("boot-args", "csr-active-config", "prev-lang:kbd"):
    if k not in dele:
        dele.append(k)
c["NVRAM"]["WriteFlash"] = True

# PlatformInfo: MacPro7,1 (nullmoth's choice for desktops with a graphics card; runs 15 and 26)
g = c["PlatformInfo"]["Generic"]
g.update({"SystemProductName": smb["SystemProductName"], "SystemSerialNumber": smb["SystemSerialNumber"],
          "MLB": smb["MLB"], "SystemUUID": smb["SystemUUID"],
          "ROM": bytes.fromhex(smb["ROM_and_NIC_MAC"].replace(":", "")), "SpoofVendor": True})
c["PlatformInfo"]["UpdateSMBIOSMode"] = "Create"
c["PlatformInfo"]["Automatic"] = True

# UEFI
def drv(path, comment):
    return {**DRV_T, "Arguments": "", "Comment": comment, "Enabled": True, "LoadEarly": False, "Path": path}
c["UEFI"]["Drivers"] = [drv("OpenRuntime.efi", "required"), drv("OpenHfsPlus.efi", "HFS+ (recovery image)"),
                        drv("ResetNvramEntry.efi", "picker entry"), drv("ToggleSipEntry.efi", "picker entry")]
take_quirks("UEFI")
c["UEFI"]["Output"]["Resolution"] = "1920x1080"

plistlib.dump(c, open(a.output, "wb"))
print(f"{a.output}: cpu={a.cpu} stage={a.stage} debug={a.debug} boot-args='{nv['boot-args']}'")
