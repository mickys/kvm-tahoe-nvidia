# RTX 4090 passthrough baseline on edi's kernel 6.1 (2026-10-17)

`99-Service-Debian13-4090` was started unchanged, at the user's request. It ran with the system QEMU 4.2 and
libvirt 7.0, machine `pc-q35-4.2`, SeaBIOS, `host-passthrough` CPU, 16 vCPU, 64 GB, and both 4090 functions as managed
hostdevs. The point was to confirm passthrough still works after the host kernel upgrade, before the QEMU 11 compat
patches.

## Result: works

**Host (edi, kernel 6.1.0-0.deb11.52):**
- vfio-pci logged only the usual `vfio_ecap_init: hiding ecap` lines (0x1e, 0x19, 0x26, 0x27, 0x25) and
  `enabling device` for the audio function.
- No reset errors, AER events or IOMMU faults. Otherwise only `kvm: ignored rdmsr` lines (`report_ignored_msrs=Y`).
- Memory with the guest's 64 GB pinned and 6 other VMs running: 96 GB used, 10 GB free, 27 GB available. Tight
  but fine for tests; leave headroom in mind before running our Tahoe guest alongside everything.

**Guest (Debian 13, kernel 6.12.57, NVIDIA driver 580.105.08, CUDA 13.0):**
- The guest agent answered about 15 s after start. IP <host-ip> on br2, which edi can reach and the dev VM
  can't.
- `07:00.0 AD102 [GeForce RTX 4090] [10de:2684]`, subsystem `1458:4104`, driver `nvidia`. Audio `07:00.1` on
  `snd_hda_intel`.
- `nvidia-smi`: P0, 40 °C, 53 W idle (450 W cap), 24564 MiB VRAM, no processes.
- **BAR1 = 32 GB in the guest** (`Region 1: Memory at 1800000000 (64-bit, prefetchable) [size=32G]`,
  `BAR1 Memory Usage Total 32768 MiB`). This domain boots **SeaBIOS**, not OVMF (no `<loader>`, and the guest has
  no `/sys/firmware/efi`). SeaBIOS with QEMU 4.2 places the full ReBAR window above 4 GB without any
  `pci-hole64-size` settings. Our Tahoe guest uses OVMF, so it still needs the `X-PciMmio64Mb` and
  `pci-hole64-size` settings from docs/macos-vm.md.
- PCIe: `nvidia-smi` reports Gen 4 current, x8 of x16 (the slot's physical link). `lspci` shows
  `LnkSta 2.5GT/s (downgraded)` at idle, which is link power-saving.
- Guest dmesg: no Xid, NVRM errors or AER events. Only `module verification failed … tainting kernel`, which is
  normal for an out-of-tree module.
- There is no CUDA toolkit, torch or gpu-burn in the guest, so no load test was run.

## How the guest was inspected

Through qemu-guest-agent `guest-exec`, without guest credentials, using
`vm/gexec.py` (copied to the project folder on the host):
`python3 gexec.py 99-Service-Debian13-4090 "nvidia-smi"`.

## Same VM on QEMU 11.1.2 (`/opt/qemu-11.1.2`, local patches 0001-0003), 2026-10-17

The XML was unchanged except `<emulator>` (machine `pc-q35-4.2`, `-no-hpet`, SeaBIOS).
- **Handover from QEMU 4.2:** a clean `virsh shutdown --mode agent` took about 6 s. Host dmesg was silent, both
  functions went back to vfio-pci, and `/dev/vfio/33` was free.
- **First start failed:** `-device hda-duplex: no default audio driver available` (libvirt 7.0 + `-nodefaults` +
  `QEMU_AUDIO_DRV`). Patch 0003 fixed it.
- **Start on the patched build:** runs. Host dmesg shows the same `hiding ecap` lines only. QEMU logs deprecation
  warnings for libvirt 7.0's old syntax (`-mon`, short-form booleans, `pc-q35-4.2`), all harmless, plus
  `VFIO dma-buf not supported in kernel, using mmap fallback, P2P DMA will not work` (P2P isn't used here).
- **Guest:** the agent was up about 41 s after start. Driver 580.105.08, P0, 43 °C, 57 W, 24564 MiB, PCIe Gen 4 x8
  (`LnkSta 16GT/s` under the driver). BAR1 is 32 GB, now placed at `0xe0000000000` (QEMU 11's 64-bit window is
  higher than 4.2's `0x1800000000`). No Xid or NVRM errors. Both sound cards are present (emulated ICH9 HDA and the
  4090 HDMI audio).
- **Guest reboot (`virsh reboot --mode agent`):** back after about 49 s, GPU healthy, host dmesg silent. The 4090
  resets cleanly under QEMU 11.
- **Host memory with it running:** 98 GB used, 6 GB free, 25 GB available.
