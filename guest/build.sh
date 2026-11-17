#!/usr/bin/env bash
# Build the Linux guest that Remora for Windows runs Android in (bd remora-7oih.5):
#
#   guest/build.sh [<out-dir>]          # default build-guest/; needs docker, nothing else
#
# Windows has no Linux kernel, and Android needs one (binder, memfd, cgroups). So Remora for
# Windows starts this small Alpine guest under QEMU on the Windows hypervisor, out of sight, and
# deploys to it the way it deploys to any docker host over ssh. The guest is a product of this
# repository, built here on Linux and shipped; a Windows user never builds it.
#
# Output, all of which ships:
#   vmlinuz              Remora's own kernel (guest/kernel/: Alpine's lts configuration plus the
#                        DMA-BUF heaps Android's codecs need), booted directly by QEMU -kernel:
#                        no bootloader, no partitions, no initramfs
#   system.qcow2         the root filesystem. Read-only to the app: each install boots a qcow2
#                        overlay on it, which an update throws away. User state lives on a
#                        second disk the guest formats on first boot (guest/rootfs/etc/init.d/
#                        remora-guest), so an update never touches it.
#   cmdline              the kernel command line those three need
#   manifest.txt         Alpine release, kernel, every installed package with its version, and
#                        the SHA-256 of each file above
#
# Built rootless: everything happens in a pinned Alpine container, and the ext4 image is made
# with `mkfs.ext4 -d`, which fills a filesystem from a directory without mounting anything.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT="${1:-$ROOT/build-guest}"
IMAGE=docker.io/library/alpine:3.24.4@sha256:f59abe656b692d901e184be3445e084786602108599d2a51d229c0f5a48c71bf

command -v docker >/dev/null || { echo "guest/build.sh: needs docker" >&2; exit 1; }
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
# The kernel source and each kernel built from it are cached here, keyed by version and config.
CACHE="${REMORA_GUEST_CACHE:-$HOME/.cache/remora-guest-build}"
mkdir -p "$CACHE"
docker run --rm -e OWNER="$(id -u):$(id -g)" -v "$ROOT/guest:/guest:ro" -v "$OUT:/out" \
    -v "$CACHE:/cache" "$IMAGE" sh /guest/assemble.sh
echo "guest: $(cat "$OUT/manifest.txt" | sed -n 1p) -> $OUT"
