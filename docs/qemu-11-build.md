# QEMU 11 for an old Debian host with libvirt 7.0

The reference host runs Debian bullseye/sid (glibc 2.31) with libvirt 7.0, and its system QEMU 4.2 is too old for
macOS Tahoe. `host/qemu-build/build.sh` builds QEMU 11.1.2 on a workstation with docker and installs it on the host
as `QEMU_DIR` (config.env, default `/opt/qemu-11.1.2`), side by side with the system QEMU. libvirt domains then
switch one at a time with `<emulator>$QEMU_DIR/bin/qemu-system-x86_64</emulator>`.

```sh
host/qemu-build/build.sh                  # CONFIGURE_ONLY=1 stops after configure (review the feature summary)
host/qemu-build/verify-on-host.sh         # also run by build.sh
```

## What build.sh does

1. Clones QEMU v11.1.2 and applies `host/qemu-build/patches/` (below).
2. Builds **all 29 system emulators** in a Debian bullseye container (`Dockerfile.edi`). Libraries bullseye lacks or
   has too old are built from source first; build.sh downloads them and checks their SHA256:

   | Library | Why |
   |---|---|
   | spice-server 0.16.0 + spice-protocol 0.14.5 | QEMU 11 needs spice-server ≥ 0.15; bullseye has 0.14.3 |
   | gnutls 3.8.13 + nettle 3.10.2 | QEMU 11 needs gnutls ≥ 3.7.5 (bullseye 3.7.1); gnutls 3.8 needs nettle ≥ 3.10 (nettle 4.0 changed its API) |
   | libcbor 0.14.0 (static) | for the nitro-enclave machine; bullseye has 0.5 |
   | lzfse 1.0 (static) | LZFSE-compressed DMG images; not packaged in bullseye |

3. Bundles every non-glibc library into `$PREFIX/lib`, checks that nothing needs a glibc newer than 2.31, and copies
   the result to the host with tar over ssh (no rsync needed). It refuses to replace an install that a running
   QEMU uses.
4. `verify-on-host.sh`: no missing or system libraries other than glibc, and every binary starts under
   `LD_BIND_NOW=1`.

The full feature list, and what is left out and why, is at the top of `build-in-container.sh`.

## libvirt 7.0 with QEMU 11: three local patches

Changing only `<emulator>` of an existing domain doesn't work out of the box. `build.sh` applies these (the version
string then reads `v11.1.2-dirty`):

1. **`0001-keep-4.2-machine-types.patch`.** QEMU 11 drops versioned machine types older than 6 years
   (`MACHINE_VER_DELETION_MAJOR` in `include/hw/core/boards.h`), but existing domains use `pc-q35-4.2`. The patch
   sets it to 7, which brings back the 4.2, 5.0 and 5.1 types; 4.1 and older stay dropped.
2. **`0002-restore-no-hpet-alias.patch`.** libvirt 7.0 emits `-no-hpet` for `<timer name='hpet' present='no'/>`, and
   QEMU removed that option in 9.0. The patch adds it back as an alias for `-machine hpet=off` (checked through QMP:
   `qom-get /machine hpet` is false with it, true without).
3. **`0003-honour-QEMU_AUDIO_DRV.patch`.** libvirt 7.0 passes `-nodefaults` (which in QEMU 11 also disables default
   audiodevs) and picks the audio backend only through `QEMU_AUDIO_DRV=spice`, which QEMU stopped reading in 8.2.
   Without the patch every domain with `<sound>` fails with `hda-duplex: no default audio driver available`.

Checks that passed on the reference host: the binary runs as `libvirt-qemu`; `virsh domcapabilities --emulatorbin`
works; `virsh domxml-to-native` accepts every option and device of existing domains with only `<emulator>` changed
(a hostdev needs `<driver name='vfio'/>` for that dry run). A Linux VM with the RTX 4090 passed through runs on it
(docs/4090-linux-baseline.md).

## Why it looks the way it does

Each point was learned from a failed build or a check:

- **All non-glibc libraries ship in `$PREFIX/lib`**, found through DT_RPATH `$ORIGIN/../lib`. The host's libraries
  are an early-2021 sid snapshot, older than bullseye's final ones (slirp 4.1, pixman 0.36, libfdt 1.5.1, libseccomp
  2.4.2, no liburing). Linking against the container's libraries but loading the host's fails with undefined
  symbols, possibly only when a function is first called.
- **Debian bullseye container**, to match the host's glibc 2.31. bullseye is EOL, so apt uses `archive.debian.org`
  with `Acquire::Check-Valid-Until=false`.
- QEMU 11 needs GCC ≥ 10.4 (bullseye has 10.2), and Clang ≤ 13 fails on some board code, so it uses **clang-19 from
  apt.llvm.org** (`llvm-toolchain-bullseye-19`).
- bullseye's Python 3.9 needs **`pip3 install tomli`** for QEMU's configure.
- **`--disable-plugins`**: the C++ contrib plugin needs `-fcoroutines` with bullseye's libstdc++.
- **meson and cmake come from pip.** spice needs a newer meson than bullseye has, and libcbor 0.14 needs C23, which
  bullseye's cmake 3.18 can't enable. spice-common needs `python3-pyparsing`.
- **Bundled libraries get `RUNPATH=$ORIGIN` (patchelf).** libtool leaves `/opt/deps/lib` as gnutls's and nettle's
  RUNPATH, and a library's own RUNPATH overrides the binary's RPATH for its dependencies, so on the host they would
  load the host's old nettle.
- **Each dependency build runs in a `( … )` subshell.** `set -e` doesn't stop on a failing command inside an `&&`
  chain, which once let a failed libcbor build go unnoticed.
