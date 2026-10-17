# edi host kernel upgrade: 5.10.13 → 6.1.180 (2026-10-17)

The user asked for this so that edi's 4090 BAR1 can be shrunk at runtime through sysfs (`resourceN_resize`, needs Linux
≥ 5.13) if macOS hangs at "PCI configuration" with the 32 GB BAR. QEMU 11.1.2's vfio-pci shows the guest only the
host's current ReBAR size (`hw/vfio/pci.c` `vfio_setup_rebar_ecap`), so the guest can't resize it itself. A
QEMU upgrade for the libvirt domains comes later, as a separate step.

## What was done (before the reboot)

- Kernel: `linux-image-6.1.0-0.deb11.52-amd64` 6.1.180-1~deb11u1, Debian's bullseye LTS build (bullseye userspace).
  - Fetched from `archive.debian.org/debian-security` (security.debian.org's pool no longer has it after bullseye
    LTS ended).
  - Verified: InRelease signature (bullseye-security key), Packages.xz hash, .deb SHA256
    `5e1511f12a28b6dcccd15f7d36c4ace343ed098e2cab5fa57f6be9695460e826`.
  - Installed with `dpkg -i`. The .deb is kept in a folder on the host.
  - No apt sources were changed and nothing else was upgraded.
- No headers were installed, on purpose. DKMS can't build `nvidia-current 460.39` (unused: the 4090 is on vfio-pci)
  or `it87` (sensors) for 6.1, and it prints a header error. That is expected.
- The new initramfs carries the same modprobe.d files as 5.10's (`vfio.conf` with `ids=10de:2684,10de:22ba`,
  amdgpu/nvidia softdeps, nouveau and snd_hda_intel blacklists).
- GRUB (backups of `/etc/default/grub` and `grub.cfg` are in that same `host-kernel/` folder):
  - `GRUB_DEFAULT=saved`. The saved entry is **5.10.0-3**, so 5.10 stays the default.
  - `grub-reboot` points at **6.1 for the next boot only**. Any later reset falls back to 5.10.
  - `panic=10` was added to `GRUB_CMDLINE_LINUX`: if 6.1 panics, edi reboots by itself into 5.10. A hang without a
    panic still needs a physical power cycle.

## Post-reboot checks (run from the dev VM)

```
ssh edi 'uname -r; cat /proc/cmdline; grub-editenv list'
ssh edi 'ip -br addr show br0 br1 br2; virsh list --all | head -12'
ssh edi 'for d in 0e:00.0 0e:00.1; do basename $(readlink /sys/bus/pci/devices/0000:$d/driver); done; ls /dev/vfio/; fuser -v /dev/vfio/* 2>&1'
ssh edi 'ls /sys/bus/pci/devices/0000:0e:00.0/ | grep resize; cat /sys/bus/pci/devices/0000:0e:00.0/resource1_resize'
ssh edi 'dmesg --level=err,warn | head -50; systemctl --failed'
```

Expected:
- `uname -r` = `6.1.0-0.deb11.52-amd64`.
- br0/br1/br2 have their addresses, and the 7 autostart VMs are running and `99-Service-Debian13-4090` is shut off.
- The 4090 is on vfio-pci.
- `resource1_resize` exists.
- Known pre-existing failures: `unbind_gpu.service` (its script `/usr/local/bin/unbind_gpus.sh` is missing) and
  `nvidia-persistenced`.

## Making 6.1 the default (only after the checks pass)

```
ssh edi 'U=4f0bef8f-2973-455b-b01c-c92653989937; grub-set-default "gnulinux-advanced-$U>gnulinux-6.1.0-0.deb11.52-amd64-advanced-$U"; grub-editenv list'
```

## Rollback

- Back on 5.10 (after a reset or a panic): nothing to do, it is still the default.
- To remove 6.1 completely: `dpkg -r linux-image-6.1.0-0.deb11.52-amd64`, restore `/etc/default/grub` from the
  backup, and run `update-grub`.

## Status

- [x] Installed, GRUB set to boot 6.1 once (2026-10-17)
- [x] `99-Service-Debian13-4090` autostart disabled (2026-10-17). The user shut the other VMs down by hand
- [x] Rebooted into 6.1 (2026-10-17 08:22 EEST)
- [x] Post-reboot checks passed (see below)
- [x] 6.1 made the saved GRUB default (`grub-set-default`). 5.10.0-3 is still installed, as a fallback in GRUB's
  "Advanced options" menu

## Post-reboot results (2026-10-17)

- Kernel `6.1.0-0.deb11.52-amd64`, the cmdline includes `panic=10`, and `next_entry` was consumed.
- br0 <host-ip> and br2 <host-ip> are up with the VMs' vnet ports. br1 is down (no cable on `upc0`), as
  before.
- VMs: the host's other libvirt domains autostarted as before. The Linux VM that also uses the 4090
  (`99-Service-Debian13-4090`) is shut off, with autostart off.
- 4090: both functions are on vfio-pci, `/dev/vfio/33` is free, and BAR1 is at `0x7000000000` (32 GB).
- **`resource0/1/3_resize` exist.** `resource1_resize` = `0xffc0`, meaning BAR1 can be set to any power of two from
  64 MB to 32 GB. To resize (with the device unused): `echo <log2 MB> > resource1_resize`, e.g. `13` = 8 GB.
  The vfio-pci driver may need to be unbound first.
- KVM: `npt`, `avic`, `nested` and `ignore_msrs` are all on. AMD-Vi interrupt remapping is enabled.
- Failed units:
  - `nvidia-persistenced` and `unbind_gpu.service` were failing before too.
  - `systemd-modules-load` is new and harmless: `/etc/modules-load.d/nvidia.conf` (`nvidia-drm`) and
    `/etc/modules` (`it87`) name DKMS modules that have no 6.1 build. Removing those entries would clear it (a
    host config change, so ask first).
- dmesg errors: only missing firmware for the iwlwifi card and `regulatory.db`. Both are harmless.
- libvirt can't autostart several storage pools whose directories don't exist (`/mnt/ssd2tb1`, `/mnt/hdd4tb1`,
  ...). Those disks weren't mounted before the reboot either.

