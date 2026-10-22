# The macOS VM: install, GPU, AMD CPU, remote access

Reference setup: AMD Ryzen 9 3950X host (KVM, Debian, kernel 6.1), Gigabyte RTX 4090 WINDFORCE V2 (AD102,
`10de:2684`, subsystem `1458:4104`) with an HDMI dummy plug as its only output, macOS 26.7.1 Tahoe, nullmoth's
NVIDIA driver 2.3.7 (first set up with 2.3.3). All values below that are specific to a machine come from `config.env`.

## The VM's files (in VM_DIR on the host)

| File | What |
|---|---|
| `OpenCore-*.img` | GPT disk with one EFI system partition: OpenCore + the macOS recovery image (built by `opencore/build-esp.sh`) |
| `tahoe.qcow2` | macOS disk, 256 GB, on AHCI |
| `OVMF_VARS.fd` | UEFI variables (NVRAM): boot entries, csr-active-config, the driver's boot guard counter |
| `serial.log` | macOS kernel log (boot-arg `serial=3`); first place to look when something breaks |
| `qemu.log`, `run.log`, `qmp.sock`, `qemu.pid` | vm/run.sh only (libvirt keeps its own) |

`smbios.json` (repo root, gitignored) holds the MacPro7,1 identity; its `ROM_and_NIC_MAC` is also the NIC's MAC. Keep
it stable once macOS is installed.

## 1. OpenCore image

```sh
scripts/fetch-vendor.sh                 # OSX-KVM (Tahoe quirks, SSDT-EC, MCEReporterDisabler) and the other sources
opencore/fetch-downloads.sh             # OpenCore 1.0.8, Lilu, VirtualSMC, AMFIPass, AMD_Vanilla (SHA256-checked)
opencore/fetch-recovery.sh              # macOS recovery via OpenCore's macrecovery.py
cp smbios.example.json smbios.json      # fill in a MacPro7,1 serial/MLB/UUID (macserial) and a MAC
# install stage, emulated Intel CPU:
opencore/build-esp.sh build/OpenCore-intel-debug.img --cpu intel --debug
# GPU stage on the AMD CPU (nullmoth boot-args, AMD_Vanilla patches with the core count of -smp):
opencore/build-esp.sh build/OpenCore-amd-gpu-debug.img --cpu amd --cores 8 --stage gpu --debug \
    --amd-patches downloads/AMD_Vanilla-patches.plist
```

`make-config.py` starts from OpenCore 1.0.8's Sample.plist, takes OSX-KVM's Tahoe quirks, adds what the driver needs
(SecureBootModel=Disabled, csr-active-config 0x0A43, Lilu + AMFIPass) and MacPro7,1 from smbios.json, then
`ocvalidate` checks it. `--stage gpu` adds the driver's boot-args
(`nvfb=1 nvaccel=1 nvfbheads=4 -nvkmsnosmooth amfi_get_out_of_my_way=0x1 amfi=0x80 ipc_control_port_options=0`;
`--noflip` adds `-nvrmnoflip`, see below). Copy the image to VM_DIR; `vm/run.sh` takes a staged `<image>.new` at the next start.

Lessons from building it:
- A bare FAT image (no partition table) makes OpenCore list itself as the default boot entry; picking it starts
  OpenCore again ("Already started") and halts. Hence the GPT image and `EFI/BOOT/.contentVisibility` = `Disabled`.
- OpenCore 1.0.8 driver entries need `HideVerbose`, so the generator copies the entry keys from Sample.plist.

## 2. Install macOS (no GPU, emulated Intel CPU)

`config.env`: `CPU=intel`, `OC=OpenCore-intel-debug.img`, `GPU_PASSTHROUGH=0`. Then on the host `vm/run.sh start`
and connect a VNC viewer to `host:5900+VNC_DISPLAY` (password in `VM_DIR/vnc-password`). The VM gets vmware-svga,
16 vCPUs (8 cores × 2 threads) and 32 GB. The reference install took about 35 minutes and 4 boots.

- The CPU model is `Skylake-Client` with `vendor=GenuineIntel`; KVM on Zen 2 can't provide `hle`, `rtm`, `pcid`,
  `invpcid` or `erms`, so they are turned off explicitly. Tahoe needs AVX2.
- **Setup Assistant hung on its last step** ("Update Mac Automatically" spinner, 40+ minutes, no disk I/O) and
  ignored the ACPI power button. `vm/run.sh reset` fixed it; the account had already been created.
- Afterwards, in the guest: turn on Remote Login (SSH) and Remote Management/Screen Sharing; turn off automatic
  update download and install (System Settings, or `/Library/Preferences/com.apple.SoftwareUpdate`).
- Guest SSH goes through the host: `127.0.0.1:SSH_FWD_PORT` on the host, e.g. in `~/.ssh/config`:
  `Host tahoe` / `HostName 127.0.0.1` / `Port 2223` / `ProxyJump <KVM host>`.
- `vm/qmp-type.py` types into the VM through QMP (U.S. layout), useful before SSH works.
- Take a qcow2 snapshot now (VM off): `qemu-img snapshot -c pre-driver tahoe.qcow2` plus a copy of `OVMF_VARS.fd`.

## 3. Pass the GPU through

`config.env`: `GPU_PASSTHROUGH=1` (or use libvirt, libvirt/README.md). The GPU must be bound to vfio-pci and its
IOMMU group free. What it took on the 4090, each point found the hard way:

1. **Behind a `pcie-root-port`, with `-global ICH9-LPC.acpi-pci-hotplug-with-bridge-support=off`.** q35's ACPI
   hot-plug on bridges (QEMU ≥ 6.1) replaces the root port's native slot handler, so presence-detect and link-active
   stay 0 (`-trace pci_cfg_read`: LNKSTA `0x84`, SLTSTA `0x0`) and macOS treats the slot as empty. Proxmox sets the
   same global. The root port also has `hotplug=off,power_controller_present=off`.
   The driver needs the root port: it moves BAR1 away from the boot console through the root port's window. Directly
   on `pcie.0` the driver loads and Metal works, but there is no display ("parent root port not found").
2. **BAR1 = 4 GB.** The guest can't resize BAR1 (QEMU exposes only the current Resizable BAR size), so the host sets
   it through sysfs before the start (`GPU_BAR1_LOG2MB=12`; needs Linux ≥ 5.13, docs/host-kernel-upgrade.md).
   The driver needs ≥ 4 GB for the display. Early tests failed with 32 GB (macOS dropped the card) and with 8 GB on
   `pcie.0` (garbage BAR address, QEMU aborted with `KVM_SET_USER_MEMORY_REGION … Invalid argument`). With the root
   port and the AMD profile's `phys-bits=40`, 8, 16 and 32 GB all boot with a display (32 GB ≥ VRAM switches the
   driver to its `static-bar1` scanout policy). 4 GB stays the default: it gave the best desktop update rate.
3. **A 64 GB PCI hole** (`-global q35-pcihost.pci-hole64-size=64G`) and OVMF's 64-bit MMIO window
   (`-fw_cfg name=opt/ovmf/X-PciMmio64Mb,string=65536`).
4. QEMU's stderr goes to `qemu.log`; `-daemonize` had hidden that abort.

Also seen: IONDRVSupport can't be excluded by OpenCore on Tahoe (it's in the System KC), but the driver blocks
IONDRVFramebuffer itself. The 4090's ROM has a UEFI GOP, so OVMF drives the dummy plug at boot and the driver takes
over that console.

## 4. Install the driver

Use the release tarball's `install.sh` (check `SHA256SUMS.txt`). It links the kexts into the Auxiliary Kernel
Collection with kmutil and needs the GPU present on PCI, so run it with the GPU attached. The 1401 Mac app isn't
needed and would rewrite the generated OpenCore config. Then boot the `--stage gpu` image.

- **Zero-copy scan-out (no `-nvrmnoflip`):** on driver 2.3.3 every screen capture read black with zero-copy, so
  `-nvrmnoflip` (the copy path) was the default here until 2026-10-21. Since the kernel's page-off waits for the GPU
  (2.3.7+) capture works with zero-copy, and the copy path costs a page-off of the whole 8 MB composite under the
  accelerator lock for every display frame (~100/s, ~2 ms each): on a GTX 1650 at 1080p/165 Hz, Parsec 55 -> 65 fps and
  a full-screen GPU app 140 -> 160 fps with zero-copy. `make-config.py --noflip` restores the copy path; live:
  `sudo sysctl -w debug.nvaccel_iop_flip=0|1`; check with `sysctl debug.nvaccel_iop_flip`.
- **Boot guard:** the driver counts display-less starts in NVRAM `nullmoth-nv-starts` and turns itself off after 2.
  Before an experimental boot: `sudo nvram -d 7C436110-AB2A-4BBB-A880-FE41995C9F82:nullmoth-nv-starts`.
- **Our plugin patches** (`guest/nvenc/`, against the release tag; `build-plugin.sh` builds stock + patched in the guest,
  `install-plugin.sh patched` installs, `relogin.sh` makes WindowServer load it):
  - Hardware encoding needs no patch since driver 2.3.7: the stock plugin loads `libnvencvt.dylib` (the open-source
    H.264/HEVC encoder, github.com/mickys/nvenc) from its bundle when `NVMTL_NVENCVT=<process list|1>` is in
    `nvrm610.conf`, provided the library and `Contents/MacOS` are owned by root and not group/world-writable
    (`sudo install -o root -g wheel -m 755 libnvencvt.dylib /Library/GPUBundles/NVMTLDriver.bundle/Contents/MacOS/`).
    No re-signing is needed: the plugin still loads with the bundle's seal broken by the extra file. Command-line tools
    (ffmpeg, avconvert) never create a Metal device, so they need `DYLD_INSERT_LIBRARIES=…/libnvencvt.dylib`. Up to
    2.3.6 this was our patch `0001`.
  - `0001` surface-in from the surface's own pages: when WindowServer composites a window drawn by the CPU (WebKit
    views, CoreGraphics apps), the plugin copied the surface's pages into VRAM with the CPU, and the pages were faulted
    in again every frame. Now the locked pages are imported as host memory and the GPU copies them (CPU copy as the
    fallback). Driver 2.3.7, Safari animation in a full-screen 4K window (3840x2110): 21 → 34 compositor updates/s,
    19.7 → 7.7 ms per upload; a 3200x1800 window is unchanged. `NVMTL_SURFIN_UPLOAD=cpu` turns it off.
  - WebKit's GPU process can't load the plugin at all (its sandbox maps code only from system paths, not
    `/Library/GPUBundles`), so Safari paints on the CPU; that is the remaining limit there.
- Results and tests: docs/driver-test-2.3.3.md.

## 5. The AMD CPU profile

`config.env`: `CPU=amd`, `OC=OpenCore-amd-gpu-debug.img` (built with `--cpu amd --cores 8 --amd-patches`). QEMU gets
`-cpu host,host-phys-bits=off,phys-bits=40,+topoext,+invtsc,vmware-cpuid-freq=on,kvm=on,-kvm-pv-eoi,-kvm-pv-unhalt`
and macOS sees "AMD Ryzen 9 3950X", 8C/16T, TSC 3.5 GHz. AMD_Vanilla's core-count patches must match `cores=` of
`-smp`.

- **`-kvm-pv-eoi,-kvm-pv-unhalt` are required.** With KVM's default paravirtual flags every AMD boot panicked during
  IOKit matching (page fault in `_copypv+0x1cc`, write to kernel text), with or without topoext, 8×2 or 16×1.
  LongQT-sea's OpenCore-ISO documents the same flags.
- **`+topoext`**, or QEMU warns that this AMD family doesn't support hyperthreading with threads=2.
- **`host-phys-bits=off,phys-bits=40` are required for the display.** Otherwise the guest sees 48 physical address
  bits (the 3950X has 43 usable), the driver puts BAR1 at `0x380e00000000`, its aperture list "could not be built (no
  memory)", and WindowServer crash-loops in `CoreDisplay_CreateDisplayForCGXDisplayDevice`: no desktop, so no Screen
  Sharing or Parsec. `phys-bits=40` alone has no effect. Check with QMP
  `qom-get /machine/unattached/device[0] phys-bits`.
- **PAT entry 6 must stay write-combining.** AMD_Vanilla's "_mtrr_update_action | Fix PAT" patch writes the fixed PAT
  `0x0007010600070106`, whose entry 6 is uncached-minus. XNU maps write-combined memory through entry 6, so with the
  stock patch every write-combined mapping runs uncached: CPU writes into VRAM through BAR1 reach only 0.37-1.4 GB/s,
  and the driver's display copy path gets that much slower. `make-config.py` therefore changes the patch to write
  `0x0001010600070106` (entry 6 write-combining, as XNU sets it); `--stock-pat` keeps the shipped value. Measured on
  the RTX 4090 at 4K: a frame copied into VRAM through BAR1 takes 3-5.5 ms instead of 86 ms, and the desktop under
  full-screen motion goes from 4.3-4.8 to 12-17 display updates per second. A host MTRR made no difference either
  way. To check the guest's PAT, trace its writes on the host during boot: enable the `kvm:kvm_msr` trace event
  with the filter `ecx == 0x277`.
- About This Mac still says "Intel Xeon W": it names the CPU from the Mac model (MacPro7,1), not from CPUID. Tools
  that ask the CPU (sysctl, fastfetch) show the Ryzen. RestrictEvents (`revcpuname=`) would change the label.

## 6. Remote access

With the GPU driving the display there is no emulated screen. Options:

- **macOS Screen Sharing.** Forwarded to host `127.0.0.1:SCREENSHARING_FWD_PORT` and, through
  `vm/tcp-forward.py --vnc-password-only`, to `LAN_LISTEN:SCREENSHARING_FWD_PORT` (vm/run.sh and the libvirt hook
  start it). macOS offers Apple's Diffie-Hellman auth (type 30) first, and RealVNC fails on it with "key length too
  large"; the forwarder offers only classic VNC password auth (type 2). Set that password in the guest:
  `sudo /System/Library/CoreServices/RemoteManagement/ARDAgent.app/Contents/Resources/kickstart -configure -clientopts
  -setvnclegacy -vnclegacy yes -setvncpw -vncpw <8 chars>`. Apple's Screen Sharing app works without the forwarder.
- **Parsec** (host on the Mac). Two things were needed:
  - An audio device. The VM has none and macOS doesn't drive the GPU's HDMI audio; Parsec 150-101 crashed on connect
    in ScreenCaptureKit's audio queue. Fix: [BlackHole 2ch](https://existential.audio/blackhole/) as the default
    output (`killall coreaudiod`, no reboot).
  - Hardware encoding: see docs/nvenc-enable-plan.md. With nullmoth's stock driver Parsec falls back to software
    HEVC at ~4 fps for 4K; with the NVENC patches (guest/nvenc) it encodes H.264 on the GPU at ~9 ms per frame.
    Turn H.265 off in the Parsec client, and keep an even capture height (below).
- **Auto-login**, so Parsec (a per-user app) runs after a reboot: `/etc/kcpassword` + `autoLoginUser` in
  `/Library/Preferences/com.apple.loginwindow` (`sysadminctl -autologin status`; FileVault must be off). At the login
  window nothing can change display modes from SSH: CoreGraphics answers only inside the console GUI session.
- **`vm/vnc-shot.py out.png`** grabs one frame through the forwarder (handy from scripts). The idle lock screen stops
  repainting or goes black; `caffeinate -u -t 5` in the guest wakes it.

## 7. Display modes and the dummy plug

`guest/display/` has JXA scripts to run inside the console session
(`sudo launchctl asuser $(id -u <user>) sudo -u <user> osascript -l JavaScript <script> …`):
`list-modes.js` / `setmode.js` (public CoreGraphics API) and `cgs-mode.js list|set W H HZ SCALE` (private CGS API,
which also sees the HiDPI modes).

- The HDMI dummy plug only offers 4K timings (4096/3840×2160 at 24–60 Hz). Every lower resolution is an Apple
  scaled mode on a 3840×2160 raster, and **the driver's NVKMS refuses every scaled mode** (serial.log:
  `switchMode 0x80001000: test 1920x1080 -> raster 3840x2160 … -> 0`). macOS reports success and keeps the old mode.
  Native timing switches work. A 1080p dummy plug would give a real 1080p mode.
- **Use 3840×2160 @ 50 Hz HiDPI ("looks like" 1920×1080): `cgs-mode.js set 1920 1080 50 2`.** At 4096×2160, Parsec
  scales the capture to 3840×2025, and nullmoth's encoder rejects odd heights (VideoToolbox -12908).

## 8. Guest tools

- `guest/nvidia-smi/`: an nvidia-smi for macOS (the nvidia-smi 550 table) that reads the GPU's resource manager
  through the driver's user client: temperature, P-state, clocks, PCIe link, VRAM, utilization, processes with their
  VRAM. Power draw, power limits, fan RPM and the extra temperatures come from the RM's user shared data page (class
  `0x00de`, public header `cl00de.h`), as NVIDIA's NVML reads them. The fan % and the default / min / max power limits
  come from two undocumented RM controls whose sizes and layouts were probed on RM 615.78.08: `0x2080852f` (cooler
  status, 844 bytes; input mask in dword 1; fan 0's level in dword 8 as 16.16, RTX 4090 idle 33 % like NVIDIA's
  driver) and `0x2080a618` (power-domain table, 20512 bytes; 65-dword records, type 0x30 id 0 = min / default / max mW).
  If an RM rejects them, those values print N/A. `--list` prints every value as key : value. `build.sh` compiles it in
  the guest (needs the Command Line Tools).
- `guest/bench/metal-bench.m`: FP32 FMA, VRAM and PCIe bandwidth, dispatch round trip, each with a result check.
- `guest/nvenc/`: the NVENC patches, plugin build/install scripts, `vttest.m` (VideoToolbox encode + PSNR) and
  `nvdec-probe.m` (layout measurement).
- Homebrew no longer installs on Intel Macs, so CLI tools come from their release binaries (fastfetch has a universal
  build).

## Known issues

- **macOS restart hangs** after all volumes are unmounted (serial.log ends with `IOPlatformHaltRestartAction ->
  AppleSMC` and `SMCWDT::setWatchdogTimer ERROR … kSMCBadCommand`). `vm/run.sh reset` (QMP `system_reset`) is safe at
  that point. Suspects: VirtualSMC together with QEMU's isa-applesmc, the ACPI reset path.
- **Shutdown sometimes hangs in the SMC halt** with the GPU attached. Once `vfs_unmountall(system) end` is in
  serial.log, `vm/run.sh kill` is safe. A plain `sudo shutdown -h now` in the guest usually powers off within seconds.
- **Occasional boot failure in boot.efi:** `EB.MM.AKMR Err(0xE)` / `STOP 0x15`, "Boot failed - Aborted", then OpenCore
  waits in its picker (serial.log ends at `OCB: Showing menu...`). The kernel's memory region couldn't be allocated
  (a random slide collision). `vm/run.sh reset` (or `virsh reset`) boots normally.
- "Memory Modules Misconfigured" notification (MacPro7,1 expects its DIMM layout); harmless.
- Copy-engine queue on Ada: the driver logs `copy-engine queue could not be built (VkResult -13)` and uses a graphics
  channel instead (also reported upstream, #93).
