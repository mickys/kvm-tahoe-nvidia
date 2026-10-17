#!/bin/bash
# Build QEMU 11 (all system emulators) on a workstation with docker and install it on the KVM host as QEMU_DIR
# (config.env). Written for a Debian bullseye host (glibc 2.31) without docker or rsync: the build runs in a
# bullseye container and the result goes over with tar through ssh. All non-glibc libraries ship in $PREFIX/lib (see
# build-in-container.sh for why), so the result doesn't depend on the host's library versions.
#   host/qemu-build/build.sh            CONFIGURE_ONLY=1 stops after QEMU's configure
set -euo pipefail
cd "$(dirname "$0")"
. ../../scripts/config.sh
QEMU_TAG=v11.1.2
PREFIX=$QEMU_DIR

# Dependency sources. spice: hashes match the .sha256 files on spice-space.org. nettle and gnutls: GPG signatures
# checked at first download (nettle: Niels Möller via the GNU keyring; gnutls: Zoltan Fridrich and Alexander
# Sosedkin). lzfse and libcbor: GitHub tag archives, hashes recorded at first download (2026-10-17).
mkdir -p deps
fetch() { [ -f "deps/$1" ] || curl -fsSL -o "deps/$1" "$2"; echo "$3  deps/$1" | sha256sum -c --quiet; }
fetch spice-0.16.0.tar.bz2 https://www.spice-space.org/download/releases/spice-server/spice-0.16.0.tar.bz2 \
  0a6ec9528f05371261bbb2d46ff35e7b5c45ff89bb975a99af95a5f20ff4717d
fetch spice-protocol-0.14.5.tar.xz https://www.spice-space.org/download/releases/spice-protocol-0.14.5.tar.xz \
  baf58449f6e89d19f475899ad5fb9196fdc46c03cc53233f4e39cf2978f9cff7
fetch lzfse-1.0.tar.gz https://github.com/lzfse/lzfse/archive/refs/tags/lzfse-1.0.tar.gz \
  cf85f373f09e9177c0b21dbfbb427efaedc02d035d2aade65eb58a3cbf9ad267
fetch libcbor-0.14.0.tar.gz https://github.com/PJK/libcbor/archive/refs/tags/v0.14.0.tar.gz \
  a8c1516e741562cf95aa4479c64916c3d4d2623e24fdc35e414e2320e7300aae
fetch nettle-3.10.2.tar.gz https://ftp.gnu.org/gnu/nettle/nettle-3.10.2.tar.gz \
  fe9ff51cb1f2abb5e65a6b8c10a92da0ab5ab6eaf26e7fc2b675c45f1fb519b5
fetch gnutls-3.8.13.tar.xz https://www.gnupg.org/ftp/gcrypt/gnutls/v3.8/gnutls-3.8.13.tar.xz \
  ffed8ec1bf09c2426d4f14aae377de4753b53e537d685e604e99a8b16ca9c97e

[ -d qemu ] || git clone -q --depth 1 --branch "$QEMU_TAG" https://gitlab.com/qemu-project/qemu.git qemu
# local compat patches for edi's libvirt 7.0 domains (see patches/): applied once, skipped if already in the tree
for p in patches/*.patch; do
  git -C qemu apply --reverse --check "../$p" 2>/dev/null && continue
  git -C qemu apply "../$p" && echo "applied $p"
done
docker build -q -t qemu-edi-builder -f Dockerfile.edi . >/dev/null
cp build-in-container.sh qemu/
mkdir -p logs
docker run --rm -e PREFIX="$PREFIX" -e CONFIGURE_ONLY="${CONFIGURE_ONLY:-}" -v "$PWD/qemu:/src" -v "$PWD/deps:/deps:ro" qemu-edi-builder /src/build-in-container.sh \
  > logs/build.log 2>&1 || { tail -30 logs/build.log; exit 1; }
tail -1 logs/build.log

# every binary and bundled library must resolve against the host's glibc 2.31
D="qemu/install-edi$PREFIX"
need=$(find "$D/bin" "$D/libexec" "$D/lib" -type f -exec sh -c 'file -L "$1" | grep -q ELF && objdump -T "$1"' _ {} \; \
  | grep -oE 'GLIBC_[0-9.]+' | sort -V -u | tail -1)
echo "highest glibc symbol: $need"
[ "$(printf '%s\nGLIBC_2.31\n' "$need" | sort -V | tail -1)" = GLIBC_2.31 ] || { echo "too new for the host"; exit 1; }

# a libvirt domain or run.sh may be using the installed copy: never replace it under a running QEMU
ssh "$KVM_HOST" "! pgrep -f '^$PREFIX/bin/' >/dev/null" || { echo "a QEMU from $PREFIX is running on $KVM_HOST, not replacing it"; exit 1; }
tar -C "qemu/install-edi$(dirname $PREFIX)" -cf - "$(basename $PREFIX)" \
  | ssh "$KVM_HOST" "mkdir -p $(dirname $PREFIX) && cd $(dirname $PREFIX) && rm -rf $(basename $PREFIX) && tar -xf -"

./verify-on-host.sh
