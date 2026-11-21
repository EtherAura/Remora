#!/bin/sh
# Build the prototype's native Mesa VA driver inside the pinned Alpine builder.
set -eu

VERSION=26.1.6
SHA512=6746b73f5b87e8ff3723eee586b4b5f033dbe6d13cd0309d743b7fa484c350268859ec8ff21aadcbca7a04070cdaa7828146dd28dde4b1935bb879891d687f0b
OUT="$1"
TAR="/cache/mesa-$VERSION.tar.xz"
TREE="/cache/mesa-$VERSION"
STAGE="$OUT/stage"

apk add --no-cache build-base bison flex meson ninja python3 py3-mako py3-packaging \
    py3-yaml libdrm-dev libva-dev expat-dev elfutils-dev linux-headers zlib-dev zstd-dev \
    xz curl >/dev/null
if ! echo "$SHA512  $TAR" | sha512sum -c -s 2>/dev/null; then
    curl -fL -o "$TAR" "https://archive.mesa3d.org/mesa-$VERSION.tar.xz"
    echo "$SHA512  $TAR" | sha512sum -c -s
fi
if [ ! -d "$TREE" ]; then
    tar -xf "$TAR" -C /cache
fi

meson setup "$OUT/compile" "$TREE" --prefix=/usr --libdir=lib --buildtype=release \
    -Dplatforms= -Dvulkan-drivers= -Dvulkan-layers= -Dgallium-drivers=virgl \
    -Dgallium-va=enabled -Dvideo-codecs=h264enc,h265enc -Dglx=disabled -Degl=disabled \
    -Dgbm=disabled -Dgles1=disabled -Dgles2=disabled -Dopengl=false -Dllvm=disabled
DESTDIR="$STAGE" ninja -C "$OUT/compile" -j "$(nproc)" install
install -m 644 "$STAGE/usr/lib/dri/virtio_gpu_drv_video.so" "$OUT/virtio_gpu_drv_video.so"
install -m 644 "$TREE/docs/license.rst" "$OUT/license.rst"
