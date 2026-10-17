# Running the VM under libvirt

`tahoe.xml.in` is the domain, `hooks/qemu.in` the libvirt hook it needs. `render.sh` fills both from config.env and
smbios.json into `libvirt/out/` (gitignored). The domain runs the same VM_DIR files as `vm/run.sh` (OpenCore image,
qcow2, OVMF_VARS.fd), so use one or the other, never both at once.

Written for libvirt 7.0 with QEMU 11.1 (docs/qemu-11-build.md); the machine type is `pc-q35-11.1`.

## Install

```sh
# workstation
libvirt/render.sh                          # REPO_ON_HOST=/opt/kvm-tahoe-nvidia by default for the hook's paths
scripts/deploy-to-host.sh                  # copies the repo (with config.env, smbios.json, libvirt/out) to the host
# host, as root
/opt/kvm-tahoe-nvidia/libvirt/install.sh   # installs /etc/libvirt/hooks/qemu (backs up a different one) + virsh define
systemctl restart libvirtd                 # only if the hook file is new: libvirtd looks for hooks at startup
virsh start tahoe-4090
```

Check before the first start: the GPU's functions are on vfio-pci, no other domain using the GPU is running
(`virsh list`), and the qcow2 isn't open by a `vm/run.sh` QEMU (`fuser $VM_DIR/tahoe.qcow2`).

## What is where

| Need | In the XML |
|---|---|
| q35, OVMF code + the existing `OVMF_VARS.fd` | `<os>` loader/nvram |
| OpenCore image (boot 1) and macOS disk on AHCI | `<disk bus='sata'>` |
| 64 GB PCI hole | `<pcihole64>` on `pcie-root` |
| GPU behind its own root port, hotplug off, both functions | `pcie-root-port` index 1 + two `<hostdev managed='yes'>` |
| USB keyboard + tablet, serial log, no emulated display, no balloon | `<input>`, `<serial type='file'>`, `<video model='none'>` |
| `-cpu host,host-phys-bits=off,phys-bits=40,+topoext,…,-kvm-pv-eoi,-kvm-pv-unhalt` | `<qemu:commandline>`: libvirt 7.0 has no XML for phys-bits or vmware-cpuid-freq. A second `-cpu` replaces libvirt's (checked: QMP shows `phys-bits` 40) |
| `ICH9-LPC.acpi-pci-hotplug-with-bridge-support=off`, root ports without power controller | `<qemu:commandline>` `-global` |
| OVMF 64-bit MMIO window | `<qemu:commandline>` `-fw_cfg name=opt/ovmf/X-PciMmio64Mb`: libvirt reserves `opt/ovmf/` names in `<sysinfo type='fwcfg'>` ("That firmware name is reserved") |
| Apple SMC, SMBIOS type 2 | `<qemu:commandline>` |
| Network: user-mode NAT with SSH and Screen Sharing forwarded to host 127.0.0.1 | `<qemu:commandline>` `-netdev user,hostfwd=…`: libvirt 7.0 has no port forwarding for `type='user'` |
| BAR1 = 4 GB before start | the hook (`prepare begin`); libvirt can't resize BARs |
| Screen Sharing on the LAN for VNC viewers | the hook starts `vm/tcp-forward.py --vnc-password-only` (`started`), stops it at `release` |

`<qemu:commandline>` marks the domain "tainted" in libvirt's log; that's expected.

The rendered XML was checked on the reference host with `virt-xml-validate` and `virsh domxml-to-native qemu-argv`
(which builds the full QEMU command line without defining anything).

## Bridged network instead of NAT

The default keeps the guest's networking exactly as under `vm/run.sh`: its NIC and MAC, the host's routes to other
networks (Parsec reaches clients on any network the host is on), and `ssh` through the host. To give the VM its own
address on a bridge instead, remove the `-netdev`/`-device virtio-net-pci` pair from `<qemu:commandline>` and add:

```xml
<interface type='bridge'>
  <source bridge='br0'/>
  <mac address='@MAC@'/>
  <model type='virtio'/>
</interface>
```

Then reach SSH and Screen Sharing at the guest's own address (a DHCP reservation for the MAC helps), and point
`vm/tcp-forward.py` at `guest:5900` if RealVNC is used.

## Day to day

- `virsh shutdown tahoe-4090` sends the ACPI power button; `sudo shutdown -h now` in the guest also works.
  If macOS hangs in the SMC halt after unmounting (docs/macos-vm.md, known issues), `virsh destroy` is safe once
  serial.log shows `vfs_unmountall(system) end`.
- Typing into the guest: `virsh send-key tahoe-4090 KEY_…` (libvirt owns the QMP socket, so `vm/qmp-type.py`
  doesn't apply).
- Hook log: `/var/log/libvirt/qemu/<VM_NAME>-hook.log` (BAR1 changes), forwarder log next to it.
