#!/bin/sh
# Runs INSIDE the pinned Alpine container that guest/build.sh starts. /guest is this directory,
# read-only; /out receives the result. See guest/build.sh for what the guest is and why.
set -eu

ALPINE=v3.24
MIRROR=https://dl-cdn.alpinelinux.org/alpine
R=/rootfs
# Remora's kernel: kernel.org's release, configured by guest/kernel/config.
KERNEL=6.18.63
KERNEL_SHA256=81764a545072d160748beff4d9de2e7d95aa9f16e8261110288f067a2cc61d71

# What the guest is for, package by package (the kernel is not one: it is built below):
#   docker, docker-openrc
#   openssh-server, openssh-sftp-server
#                        the remote backend's transport. sftp-server because Windows' scp speaks
#                        SFTP by default.
#   bash coreutils procps-ng iproute2 findutils grep sed gawk util-linux tar gzip xz
#                        the GNU userland Remora's remote snippets are written against (`ps -C`,
#                        `stat -c`, `date -d`, `xargs -r`…), which busybox only partly provides
#   python3 openssl      the image catalog and Play installer helpers run on the docker host;
#                        the catalog checks the image signature with `openssl pkeyutl`
#   sqlite               the Reddit sign-in transplant edits account databases on the host side
#   curl ca-certificates image and payload downloads made on the host
#   e2fsprogs            formats the data disk on first boot
PKGS="alpine-base kmod e2fsprogs \
      docker docker-openrc openssh-server openssh-sftp-server \
      bash coreutils procps-ng iproute2 findutils grep sed gawk util-linux tar gzip xz \
      python3 openssl sqlite curl ca-certificates"

apk add --no-cache e2fsprogs e2fsprogs-extra qemu-img curl >/dev/null

# The kernel, built once per version and configuration and cached across builds (~5 min cold).
# Binder and binderfs, DMA-BUF heaps, and every driver from power-on to / built in; the rest as
# Alpine configures it. See guest/kernel/remora.fragment for what differs and why.
KKEY="$KERNEL-$(sha256sum /guest/kernel/config | cut -c1-16)"
KOUT="/cache/kernel-$KKEY"
if [ ! -f "$KOUT/vmlinuz" ]; then
    apk add --no-cache build-base bc bison flex perl openssl openssl-dev elfutils-dev \
        linux-headers diffutils findutils xz ncurses-dev gmp-dev mpc1-dev mpfr-dev gawk >/dev/null
    TAR="/cache/linux-$KERNEL.tar.xz"
    if ! echo "$KERNEL_SHA256  $TAR" | sha256sum -c -s 2>/dev/null; then
        curl -fL -o "$TAR" "https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-$KERNEL.tar.xz"
        echo "$KERNEL_SHA256  $TAR" | sha256sum -c -s || { echo "kernel source checksum" >&2; exit 1; }
    fi
    # The tree is kept in the cache too, so a failure late in a build costs a rebuild of what
    # changed rather than the whole kernel. One tree per version.
    TREE="/cache/tree-$KERNEL"
    [ -d "$TREE" ] || { mkdir -p "$TREE.new" && tar -xf "$TAR" -C "$TREE.new" --strip-components=1 &&
                        mv "$TREE.new" "$TREE"; }
    rm -rf "$KOUT.new"
    mkdir -p "$KOUT.new"
    cd "$TREE"
    cp /guest/kernel/config .config
    make -s olddefconfig
    cmp -s .config /guest/kernel/config || {
        echo "guest/kernel/config is not complete for $KERNEL: rerun guest/kernel/derive-config.sh" >&2
        exit 1
    }
    make -s -j"$(nproc)" bzImage modules
    make -s INSTALL_MOD_PATH="$KOUT.new" INSTALL_MOD_STRIP=1 modules_install
    cp arch/x86/boot/bzImage "$KOUT.new/vmlinuz"
    cd /
    mv "$KOUT.new" "$KOUT"
fi

mkdir -p "$R/etc/apk"
printf '%s/%s/main\n%s/%s/community\n' "$MIRROR" "$ALPINE" "$MIRROR" "$ALPINE" >"$R/etc/apk/repositories"
cp -r /etc/apk/keys "$R/etc/apk/"
# Package scripts run chrooted into $R (adding the sshd and docker users and groups).
apk add --root "$R" --initdb --no-cache $PKGS >/dev/null 2>"/tmp/apk.err" || {
    cat /tmp/apk.err >&2
    exit 1
}

# -r, not -a: the files here belong to whoever checked out the repo, often uid 1000, which is
# the guest's remora account. In the guest they must be root's.
cp -r /guest/rootfs/. "$R/"
chmod 755 "$R/etc/init.d/remora-guest" "$R/usr/sbin/remora-run"

# Alpine's own sysctl hardening, shadowed by a same-named copy in /etc with two values changed:
#   kernel.unprivileged_bpf_disabled 1 -> 0: Android's netbpfload writes 0 at boot, a 1 is
#     permanent until reboot so the write fails, and netbpfload is critical, so Android reboots
# (and its shutdown sends sysrq to the whole guest; see /etc/inittab). Seen.
#   kernel.panic 120 -> 10: reboot soon after a panic, as cmdline's panic=10 asks.
mkdir -p "$R/etc/sysctl.d"
{
    echo "# Remora: Alpine's /usr/lib/sysctl.d/00-alpine.conf with unprivileged bpf allowed (Android"
    echo "# needs it) and a 10 s panic reboot. Same name, so it replaces that file. See guest/assemble.sh."
    sed -e 's/^kernel.unprivileged_bpf_disabled = 1$/kernel.unprivileged_bpf_disabled = 0/' \
        -e 's/^kernel.panic = 120$/kernel.panic = 10/' "$R/usr/lib/sysctl.d/00-alpine.conf"
} >"$R/etc/sysctl.d/00-alpine.conf"
grep -qx 'kernel.unprivileged_bpf_disabled = 0' "$R/etc/sysctl.d/00-alpine.conf" &&
    grep -qx 'kernel.panic = 10' "$R/etc/sysctl.d/00-alpine.conf" || {
    echo "assemble: Alpine's sysctl defaults changed shape; revisit the override" >&2
    exit 1
}

# One account, in the docker group: the remote backend needs no root on its docker host.
# '*' rather than adduser's '!': OpenSSH without PAM refuses key logins to a '!' (locked) account.
chroot "$R" adduser -D -u 1000 -s /bin/bash remora
chroot "$R" addgroup remora docker
sed -i 's/^remora:!/remora:*/' "$R/etc/shadow"

for s in devfs dmesg mdev hwdrivers; do chroot "$R" rc-update add "$s" sysinit >/dev/null; done
for s in modules hwclock hostname bootmisc sysctl localmount cgroups remora-guest; do
    chroot "$R" rc-update add "$s" boot >/dev/null
done
# sshd, dockerd and acpid are NOT OpenRC services here: init runs them (see /etc/inittab).
chroot "$R" rc-update add networking default >/dev/null
for s in mount-ro killprocs savecache; do chroot "$R" rc-update add "$s" shutdown >/dev/null; done

mkdir -p "$R/lib/modules"
cp -a "$KOUT/lib/modules/." "$R/lib/modules/"
KVER=$(ls "$R/lib/modules")
cp "$KOUT/vmlinuz" /out/vmlinuz
rm -f /out/initramfs
rm -rf "$R/var/cache/apk/"*

# ext4 with a third free for logs and the odd apk add while debugging. User data never lands
# here: it goes to the data disk.
USED_MB=$(du -sm "$R" | cut -f1)
SIZE_MB=$(( USED_MB * 4 / 3 + 256 ))
rm -f /tmp/system.img
mkfs.ext4 -q -L remora-system -d "$R" -E root_owner=0:0 /tmp/system.img "${SIZE_MB}M"
# Converted beside the old disk and renamed over it: a guest still booted on the previous build
# holds it as a backing file, and qemu-img will not write an image another QEMU has locked.
qemu-img convert -c -O qcow2 /tmp/system.img /out/system.qcow2.new
mv /out/system.qcow2.new /out/system.qcow2

# panic=10: a sysrq 'c' from a container (see /etc/inittab) panics the kernel; reboot, not hang.
# psi=1: the configuration builds pressure stall information in but off, and Android's lmkd exits
# without it, every 5 s, for as long as Android runs.
echo "root=/dev/vda rootfstype=ext4 rw console=ttyS0 quiet panic=10 psi=1" \
    >/out/cmdline
{
    echo "Alpine $(cat "$R/etc/alpine-release"), kernel $KVER"
    echo
    echo "sha256:"
    (cd /out && sha256sum vmlinuz system.qcow2 cmdline)
    echo
    echo "packages:"
    apk info --root "$R" -v 2>/dev/null | sort
} >/out/manifest.txt
# The container runs as root; the files belong to whoever ran guest/build.sh.
chmod 644 /out/*
chown "${OWNER:-0:0}" /out/*
