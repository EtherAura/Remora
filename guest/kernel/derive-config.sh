#!/bin/sh
# Regenerate guest/kernel/config: Alpine's linux-lts configuration with remora.fragment merged in.
# Run by hand when the kernel version moves (bd remora-7oih.5), inside the pinned Alpine image,
# with the kernel tree unpacked at $1 and Alpine's config for that version at $2:
#
#   docker run --rm -v "$PWD/guest:/guest" -v <tree>:/src -v <config>:/alpine.config:ro \
#       <guest/build.sh's image> sh /guest/kernel/derive-config.sh /src /alpine.config
#
# The result is committed: the guest build never derives, it only builds what is checked in.
set -eu
SRC="$1"
apk add --no-cache build-base bison flex perl gawk >/dev/null
cd "$SRC"
cp "$2" .config
scripts/kconfig/merge_config.sh -m .config /guest/kernel/remora.fragment >/dev/null
make olddefconfig >/dev/null
# merge_config only warns when an option does not take; a required one that did not is fatal.
for opt in DMABUF_HEAPS_SYSTEM=y ANDROID_BINDERFS=y ANDROID_BINDER_IPC=y VIRTIO_BLK=y VIRTIO_PCI=y \
           VIRTIO_NET=y EXT4_FS=y FW_CFG_SYSFS=y OVERLAY_FS=y MEMFD_CREATE=y PSI=y \
           BLK_DEV_LOOP=y BLK_DEV_DM=y FUSE_FS=y TUN=y INPUT_UINPUT=y; do
    grep -qx "CONFIG_$opt" .config || { echo "derive-config: CONFIG_$opt did not take" >&2; exit 1; }
done
for opt in BRIDGE VETH NF_TABLES IP_NF_IPTABLES; do
    grep -qE "^CONFIG_$opt=(y|m)$" .config || { echo "derive-config: CONFIG_$opt lost" >&2; exit 1; }
done
cp .config /guest/kernel/config
echo "derive-config: $(grep -c '=m$' .config) modules, $(grep -c '=y$' .config) built-in"
