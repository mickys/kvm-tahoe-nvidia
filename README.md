# kvm-tahoe-nvidia

macOS 26 **Tahoe** (x86_64) in a KVM virtual machine on an **AMD Ryzen** host, with an **NVIDIA RTX 4090 passed
through** and driven by [nullmoth's NVIDIA Metal driver](https://github.com/nullmoth/nvidia-macos-driver).

| | Status (driver 2.4.3, macOS 26.7.1) |
|---|---|
| Boot, install | macOS installed with an emulated Intel CPU, then runs on the host's real Ryzen 9 3950X (AMD_Vanilla) |
| GPU | RTX 4090 (AD102): Metal 3, 24 GB, 4K desktop drawn by the GPU on an HDMI dummy plug |
| Driver self-test | nullmoth's `metal_selftest.sh`: 16 ran, 0 failed |
| Throughput | VRAM 948 GB/s (94 % of spec), PCIe 4.0 x8 11–13 GB/s |
| Remote desktop | macOS Screen Sharing / VNC viewers, Parsec |
| Hardware video encode / decode | The stock driver has no encoder for RTX 40 (it ships them for RTX 20, 30 and 50). Our open-source codecs ([mickys/nvenc](https://github.com/mickys/nvenc) 0.9.3 (GTX 1650 / TU117 too): H.264 / HEVC / AV1 encode, H.264 / HEVC / VP9 / AV1 / MPEG-1/2/4 decode) load through the stock plugin's `NVMTL_NVENCVT` hook (no patch needed since 2.3.7): Parsec 4.7–4.9 ms per 4K frame; packaged as the [mickys overlay](https://github.com/mickys/nvidia-macos-driver/releases) |
| Desktop | Patch 0001 in `guest/nvenc/` (48 lines; applies to 2.3.7 and 2.4.3, measured on 2.3.7) uploads CPU-drawn windows from their own pages through the GPU: a full-screen 4K Safari window 21 → 34 compositor updates/s, 19.7 → 7.7 ms per upload; smaller windows unchanged |
| Screen capture, streaming | Overlay [v2.4.3-mickys.8](https://github.com/mickys/nvidia-macos-driver/releases) `--desktop`: captured frames current and in order, no tearing, no ~117 ms freezes (Parsec on a GTX 1650 at 1080p: 46 → 60-66 fps). Boot **without** `-nvrmnoflip`: the copy-path scan-out paged the 8 MB composite off VRAM for every display frame (`docs/macos-vm.md`) |
| Monitoring | `nvidia-smi` for macOS (`guest/nvidia-smi/`): NVIDIA's table, `-q`, `-L`, `--query-gpu=… --format=csv`, `dmon`, `-l`; temperature, P-state, clocks, PCIe link, VRAM / BAR1, GPU / memory / encoder / decoder utilization, encoder sessions, processes power draw / limits and fan RPM from the RM's user shared data page (class `0x00de`; fan % N/A) |

## Repository layout

| Path | What |
|---|---|
| `config.example.env` | All machine-specific settings (host paths, GPU PCI addresses, ports, VM size). Copy to `config.env` |
| `smbios.example.json` | MacPro7,1 identity template. Copy to `smbios.json` and generate your own values |
| `host/qemu-build/` | Build QEMU 11.1.2 for an old Debian host + patches so libvirt 7.0 works with it (docs/qemu-11-build.md) |
| `opencore/` | Fetch OpenCore, kexts and macOS recovery; generate config.plist; build the OpenCore disk image |
| `vm/` | `run.sh` (plain QEMU), `tcp-forward.py` (Screen Sharing for VNC viewers), QMP/VNC helpers |
| `libvirt/` | Domain XML template, libvirt hook, render/install scripts (libvirt/README.md) |
| `guest/` | Tools for the macOS guest: nvidia-smi, display modes, Metal benchmark, NVENC patches and tests |
| `scripts/` | `config.sh` (loader), `fetch-vendor.sh` (third-party sources), `deploy-to-host.sh` |
| `docs/` | How it works and what was measured (below) |

Not in the repo, on purpose: `config.env`, `smbios.json`, passwords (`.env`), disk images, Apple's recovery image,
OpenCore/kext/driver downloads (fetched and SHA256-checked by the scripts), the GPU's VBIOS.

## Quick start

Two machines: a **workstation** with docker (builds, scripts) and the **KVM host** with the GPU (root ssh from the
workstation; the GPU bound to vfio-pci; Linux ≥ 5.13 for BAR resizing).

1. `cp config.example.env config.env` and edit it; `cp smbios.example.json smbios.json` and fill it in.
2. QEMU 11 on the host: `host/qemu-build/build.sh`.
3. Sources and downloads: `scripts/fetch-vendor.sh`, `opencore/fetch-downloads.sh`, `opencore/fetch-recovery.sh`.
4. OpenCore images: `opencore/build-esp.sh …` (docs/macos-vm.md §1), copy them to `VM_DIR` on the host.
5. `scripts/deploy-to-host.sh`, then install macOS with `vm/run.sh start` on the host (`CPU=intel`,
   `GPU_PASSTHROUGH=0`, VNC). docs/macos-vm.md §2.
6. Pass the GPU through, install the driver, switch to the AMD profile: docs/macos-vm.md §3–5.
7. Optional: run it under libvirt (libvirt/README.md), remote access and Parsec (§6), NVENC (docs/nvenc-enable-plan.md).

## Docs

- [docs/macos-vm.md](docs/macos-vm.md): the whole VM: OpenCore, install, GPU passthrough requirements, driver, AMD
  CPU, remote access, display modes, guest tools, known issues
- [libvirt/README.md](libvirt/README.md): running it as a libvirt domain
- [docs/qemu-11-build.md](docs/qemu-11-build.md): QEMU 11 on Debian bullseye with libvirt 7.0
- [docs/host-kernel-upgrade.md](docs/host-kernel-upgrade.md): host kernel 5.10 → 6.1 (needed for BAR1 resizing)
- [docs/4090-linux-baseline.md](docs/4090-linux-baseline.md): the same 4090 in a Linux VM on QEMU 11 (baseline)
- [docs/driver-test-2.3.3.md](docs/driver-test-2.3.3.md): driver bring-up, self-test and benchmark results
- Our own NVENC encoder from public docs (replacing the driver's closed library) now lives in its own repository, `nvenc`
- [docs/nvenc-research.md](docs/nvenc-research.md), [docs/nvenc-enable-plan.md](docs/nvenc-enable-plan.md):
  hardware encoding before Blackwell: what blocks it, how the input layout was measured and fixed, Parsec results

## Credits

- [nullmoth/nvidia-macos-driver](https://github.com/nullmoth/nvidia-macos-driver): the driver this is all about
- [kholia/OSX-KVM](https://github.com/kholia/OSX-KVM): Tahoe quirks, SSDT-EC, MCEReporterDisabler
- [acidanthera](https://github.com/acidanthera): OpenCore, Lilu, VirtualSMC, macrecovery; AMFIPass via
  [OpenCore Legacy Patcher](https://github.com/dortania/OpenCore-Legacy-Patcher)
- [AMD-OSX/AMD_Vanilla](https://github.com/AMD-OSX/AMD_Vanilla): kernel patches for AMD CPUs
- [LongQT-sea/OpenCore-ISO](https://github.com/LongQT-sea/OpenCore-ISO): the `-kvm-pv-eoi,-kvm-pv-unhalt` hint
- [NVIDIA open-gpu-kernel-modules](https://github.com/NVIDIA/open-gpu-kernel-modules): RM headers for nvidia-smi
- [BlackHole](https://existential.audio/blackhole/): virtual audio device for Parsec

macOS's license allows it to run only on Apple hardware. This repo documents an experiment; it ships no Apple
software.
