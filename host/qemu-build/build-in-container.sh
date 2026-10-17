#!/bin/bash
# Runs inside the qemu-edi-builder container (see Dockerfile.edi). /src is the QEMU source tree, /deps holds the
# spice tarballs.
# 1. Libraries that bullseye lacks or has too old, built into /opt/deps:
#    - spice-protocol 0.14.5 + spice-server 0.16.0 (QEMU 11 needs spice-server >= 0.15; bullseye has 0.14.3)
#    - nettle 3.10.2 + gnutls 3.8.13 (QEMU 11 needs gnutls >= 3.7.5, bullseye has 3.7.1; gnutls 3.8 needs
#      nettle >= 3.10). nettle 4.0 exists but changed its API, so 3.10.x.
#    - libcbor 0.14.0, static (QEMU needs >= 0.7, bullseye has 0.5; enables the nitro-enclave machine, virtio-nsm)
#    - lzfse 1.0, static (LZFSE-compressed DMG images; not packaged in bullseye)
# 2. QEMU: all system emulators, every feature that makes sense on a headless KVM host.
# 3. Bundle every non-glibc library into $PREFIX/lib. edi's libraries are an early-2021 sid snapshot, older than
#    bullseye's (slirp 4.1, pixman 0.36, no liburing), so the binaries must not use them. They find the bundled
#    copies through DT_RPATH $ORIGIN/../lib, which also covers the libraries' own dependencies.
set -euo pipefail
PREFIX=${PREFIX:-/opt/qemu-11.1.2}   # set by build.sh from config.env (QEMU_DIR)
DEPS=/opt/deps
export PKG_CONFIG_PATH=$DEPS/lib/x86_64-linux-gnu/pkgconfig:$DEPS/lib/pkgconfig:$DEPS/share/pkgconfig
export CC=clang-19 CXX=clang++-19

mkdir -p /tmp/spice && cd /tmp/spice
tar -xzf /deps/lzfse-1.0.tar.gz
make -s -C lzfse-lzfse-1.0 install CC=clang-19 INSTALL_PREFIX=/usr/local \
  CFLAGS='-O2 -fPIC -Wall -Wno-unknown-pragmas -Wno-unused-variable -DNDEBUG -D_POSIX_C_SOURCE -std=c99'
tar -xzf /deps/libcbor-0.14.0.tar.gz
# each build runs in a subshell: set -e does not stop on a failing command inside an && chain, a failing subshell does
(cmake -S libcbor-0.14.0 -B libcbor-build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$DEPS \
   -DCMAKE_INSTALL_LIBDIR=lib -DBUILD_SHARED_LIBS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DWITH_EXAMPLES=OFF \
   -DSANITIZE=OFF -DCMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE=OFF && cmake --build libcbor-build -j"$(nproc)" && cmake --install libcbor-build) >/dev/null
tar -xzf /deps/nettle-3.10.2.tar.gz
(cd nettle-3.10.2 && ./configure -q --prefix=$DEPS --libdir=$DEPS/lib --disable-documentation --disable-openssl \
   && make -s -j"$(nproc)" && make -s install) >/dev/null
tar -xJf /deps/gnutls-3.8.13.tar.xz
(cd gnutls-3.8.13 && ./configure -q --prefix=$DEPS --libdir=$DEPS/lib --disable-doc --disable-manpages \
   --disable-tests --disable-tools --disable-guile --without-tpm --without-tpm2 --with-included-unistring \
   --with-included-libtasn1 --without-brotli && make -s -j"$(nproc)" && make -s install) >/dev/null
tar -xJf /deps/spice-protocol-0.14.5.tar.xz && tar -xjf /deps/spice-0.16.0.tar.bz2
(cd spice-protocol-0.14.5 && meson setup build --prefix=$DEPS && ninja -C build install >/dev/null)
(cd spice-0.16.0 && meson setup build --prefix=$DEPS --buildtype=release -Dgstreamer=no -Dmanual=false \
   -Dtests=false -Dlz4=true -Dsasl=true -Dopus=enabled -Dsmartcard=enabled && ninja -C build install >/dev/null)
# the libraries QEMU links statically must be plain objects: libcbor turns on LTO by itself, and clang's LTO bitcode
# can't be read by GNU ld
for a in $DEPS/lib/libcbor.a /usr/local/lib/liblzfse.a; do
  ar t "$a" | head -1 | xargs -I{} sh -c "cd /tmp && ar x '$a' {} && file {}" | grep -q ELF || { echo "$a is not ELF"; exit 1; }
done

# Left out on purpose: opengl/virglrenderer/gtk/sdl (edi is headless; the 4090 is passed through, not rendered on),
# xen (the Xen *emulation* under KVM is in), rbd/glusterfs/iscsi/nfs (unused), hv-balloon (needs glib >= 2.68,
# bullseye has 2.66), bpf (needs libbpf >= 1.0), igvm (needs the Rust igvm library), u2f (no Debian package), rust (rustc >= 1.83 needed; it only adds experimental copies of pl011
# and hpet), plugins (TCG only; the C++ contrib plugin needs -fcoroutines with bullseye's libstdc++), docs.
# QEMU's configure; an existing build tree is reused when the arguments are unchanged (retries only rebuild what changed)
CONFIGURE=(../configure --cc=clang-19 --cxx=clang++-19 --prefix=$PREFIX --disable-user \
  --extra-ldflags='-Wl,--disable-new-dtags -Wl,-rpath,$ORIGIN/../lib' \
  --enable-kvm --enable-vhost-net --enable-vhost-user --enable-vhost-kernel --enable-vhost-vdpa \
  --enable-slirp --enable-vnc --enable-vnc-jpeg --enable-png --enable-vnc-sasl \
  --enable-spice --enable-spice-protocol --enable-usb-redir --enable-libusb --enable-smartcard \
  --enable-seccomp --enable-linux-aio --enable-linux-io-uring --enable-numa --enable-fdt=system \
  --enable-cap-ng --enable-attr --enable-virtfs --enable-gnutls --enable-libcbor --enable-libudev \
  --enable-curl --enable-libssh --enable-zstd --enable-bzip2 --enable-lzo --enable-snappy --enable-lzfse \
  --enable-fuse --enable-capstone --enable-libkeyutils --enable-selinux --enable-tools --enable-guest-agent \
  --enable-tpm --enable-libpmem --enable-libdaxctl --enable-vde --enable-rdma --audio-drv-list=pa,alsa,oss \
  --disable-opengl --disable-virglrenderer --disable-gtk --disable-sdl --disable-xen \
  --disable-docs --disable-werror --disable-rust --disable-plugins)
cd /src && mkdir -p build-edi && cd build-edi
if [ "$(cat .configure-args 2>/dev/null)" != "${CONFIGURE[*]}" ]; then
  cd /src && rm -rf build-edi && mkdir build-edi && cd build-edi
  "${CONFIGURE[@]}"
  echo "${CONFIGURE[*]}" > .configure-args
fi
[ -z "${CONFIGURE_ONLY:-}" ] || exit 0   # CONFIGURE_ONLY=1: stop here to review the feature summary
make -j"$(nproc)"
rm -rf /src/install-edi && make install DESTDIR=/src/install-edi

D=/src/install-edi$PREFIX
mkdir -p $D/lib
for f in $D/bin/* $D/libexec/*; do file -L "$f" | grep -q ELF && LD_LIBRARY_PATH=$DEPS/lib/x86_64-linux-gnu:$DEPS/lib ldd "$f"; done \
  | awk '/=> \//{print $3}' | sort -u \
  | grep -vE '/(libc|libm|libpthread|libdl|librt|libresolv|libutil|libanl|ld-linux-x86-64)\.so' \
  | while read -r l; do cp -L "$l" $D/lib/; done
# libtool leaves DT_RUNPATH=/opt/deps/lib on gnutls/nettle; a library's own RUNPATH overrides the binary's RPATH for
# its dependencies, so point every bundled library at its own directory
for l in $D/lib/*; do patchelf --set-rpath '$ORIGIN' "$l"; done
echo "bundled $(ls $D/lib | wc -l) libraries"
