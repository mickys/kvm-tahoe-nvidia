# nullmoth NVIDIA driver 2.3.3 on macOS Tahoe 26.7.1 in a KVM VM with an RTX 4090 (2026-10-17)

## Setup

- **Host:** edi (Ryzen 9 3950X), kernel 6.1, QEMU 11.1.2 (our build), VM run by `vm/run.sh`.
- **Guest:** macOS 26.7.1 (25G241), MacPro7,1, OpenCore 1.0.8 DEBUG, SIP 0x0A43, Lilu 1.7.2 + AMFIPass 1.4.1 +
  VirtualSMC 1.3.8.
- **CPU:** emulated Intel (`Skylake-Client`, `vendor=GenuineIntel`), 16 vCPU (8 cores x 2 threads), 32 GB.
- **GPU:** Gigabyte RTX 4090 WINDFORCE V2 (`10de:2684`, `1458:4104`) via vfio-pci, behind a `pcie-root-port`.
  BAR1 is 4 GB, set on the host through sysfs. An HDMI dummy plug is the only output.
- **Driver:** `nullmoth-nvidia-2.3.3.tar.gz`, SHA256 checked against the release, installed with `install.sh`
  (log: `the install log`). OpenCore built with `make-config.py --cpu intel --stage gpu --debug`.

## Result: the driver runs, and the desktop is drawn by the RTX 4090

- All four kexts load: NVRM, NVAccel, NVRMFB, NVRMAGDC.
- `RmInitAdapter succeeded`, with GSP-RM running on the card. The driver reports VRAM 25757220864 bytes and the gpu
  profile "Ada AD102 … BAR1 4096 MB of 24564 MB VRAM".
- `bar1: placing BAR1 4096 MB @0x1e00000000, BAR3 32 MB @0x1f00000000, root port window …`, then `PLACED`.
- NVKMS 610.57.04 found the dummy plug on connector 0x1 (256 B EDID) and kept the firmware's 3840x2160 @ 60 Hz on
  head 0. It published the display audio ELD.
- `NVRM-fb: started on NVRMDisplay`, then `boot guard cleared: the framebuffer has its scanout memory and a display
  mode`.
- WindowServer flips onto it: `iop: ASYNC swap … 4096x2160 … scanned out of its own VRAM (flipResult 0)`.
- System Information: "NVIDIA GeForce RTX 4090, PCIe x8, VRAM 24 GB, Metal Support: Metal 3".
- Expected noise: `kapiInit failed` for fb1–fb3. `nvfbheads=4`, but only 1 display is connected ("nothing to drive").

## What it took (VM-specific; most of it would be new to nullmoth)

1. **Root port presence.** With q35's default ACPI PCI hot-plug on bridges, QEMU's root port reports presence-detect
   = 0 and link-active = 0, so macOS IOPCIFamily skips the slot. Fix:
   `-global ICH9-LPC.acpi-pci-hotplug-with-bridge-support=off`.
   - Without a root port (GPU on `pcie.0`), the driver still loads and Metal works for apps. But NVRMFB can't place
     BAR1 ("parent root port not found"), and the display never arms.
2. **BAR1 size.** QEMU exposes only the host's current ReBAR size, so the guest can't resize it. The host (Linux 6.1)
   resizes it through sysfs before start.
   - 32 GB: macOS drops the device.
   - 8 GB: macOS assigns a garbage address (0x8108700000000000). QEMU/KVM then abort.
   - 4 GB works, and it is the driver's minimum for the display (per 1401 `guide.py`).
3. **IONDRVSupport** can't be excluded via OpenCore on Tahoe ("not in Boot KC … Unsupported"). NVRMFB's claim blocks
   IONDRVFramebuffer by itself.

## Test results on the AMD CPU profile (2026-10-17)

`-cpu host` (Ryzen 9 3950X, 8C/16T) + AMD_Vanilla, 32 GB, display 4096x2160@50 HiDPI on the dummy plug, copy path
(`-nvrmnoflip`). Metal device: "NVIDIA GeForce RTX 4090 (NVMTL over NVIDIA GeForce RTX 4090 (NVK AD102-A))", plugin 0.1
(c3f6bce5).

**nullmoth `tools/metal_selftest.sh` (v2.3.3 tools, run over SSH): 16 ran, 0 failed, 1 skipped.**

| check | result | seconds | detail |
|---|---|---|---|
| surface-order (all 11 modes) | pass | 1-6 each | 100 frames, 0 bad |
| surface-wrappers rgba8 / nv12 | pass | 3 / 2 | 0 of 12 cases wrong |
| host-copy texture | skipped | | not in the v2.3.3 tools |
| pipeline-build | pass | 4 | OK |
| vt-transfer 4032x3024 -> 1024x768 | pass | 13 | first 6469 ms, later avg 224 ms, best 113 ms, 0 bad |
| vt-transfer 1920x1080 -> 960x540 | pass | 7 | first 307 ms, later avg 220 ms, best 138 ms, 0 bad |

**`guest/bench/metal-bench.m`** (ours; every test checks its result):

| test | result | RTX 4090 reference |
|---|---|---|
| FP32 FMA | 32.9 TFLOPS (result ok) | 82.6 TFLOPS spec. Rough: an FMA-chain kernel, and the translator/NVK compiler may fold the identity FMAs |
| VRAM copy, compute kernel | 947.7 GB/s | 1008 GB/s spec (94%) |
| VRAM copy, blit | 941.3 GB/s (data ok) | |
| upload shared -> private | 11.2 GB/s | PCIe 4.0 x8 (~13 GB/s practical) |
| download private -> shared | 13.1 GB/s | |
| tiny dispatch round trip | 120 us | |

recommendedMaxWorkingSet 19.2 GB, maxBufferLength 4.3 GB, hasUnifiedMemory 0.

**GPU monitoring:** NVAccel's `PerformanceStatistics` has only memory/texture counters (no "Device Utilization %",
VRAM in use, temperature or clocks), so Activity Monitor's GPU History and iStat Menus show nothing for it. There is
no nvidia-smi for macOS. The NVRM user client passes unknown escapes straight to `rm_ioctl` (`NVRMUserClient::esc`
default case), the Linux RM ioctl interface that NVML/nvidia-smi use, so a small RM-control tool is possible.

## Still to test

- Look at the desktop (macOS Screen Sharing via edi port 5931), plus Metal workloads and performance.
- Sleep/wake and multiple displays (nvfbheads).
- ~~The AMD CPU profile~~: done. `-cpu host,+topoext,…,-kvm-pv-eoi,-kvm-pv-unhalt` + AMD_Vanilla. The driver,
  display and Metal 3 are unchanged on the real Zen 2 CPU (2026-10-17). Without `-kvm-pv-eoi/-kvm-pv-unhalt`,
  macOS panics during IOKit matching (`_copypv` page fault); logs `serial-amd-panic{1,2-topoext,3-16c}.log` on edi.
- Report to nullmoth.
