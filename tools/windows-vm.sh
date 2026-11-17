#!/usr/bin/env bash
# The Windows 11 test VM (dockur/windows in docker, KVM) that signs off the Windows client
# (bd remora-7oih). Shared with Kartend, hence the container name and account.
#
#   tools/windows-vm.sh up             # create on first use (unattended install, ~20 min), else start
#   tools/windows-vm.sh ssh [cmd…]     # cmd.exe on the guest; no cmd = interactive
#   tools/windows-vm.sh put <src…> <guest-dir>     # scp -r into the guest, e.g. C:/remora
#   tools/windows-vm.sh down           # graceful shutdown; frees the RAM
#
# Windows 11 Enterprise Evaluation, downloaded from Microsoft by dockur on first boot: a 90-day
# evaluation licence, which is what Microsoft offers for testing. Web console on
# http://127.0.0.1:8006, RDP on 127.0.0.1:3389, ssh on 127.0.0.1:2222. All bound to loopback.
#
# The install ends by running oem/install.bat (written below): it installs the pinned Win32-OpenSSH
# MSI and authorises ~/.ssh/kartend_win.pub for the admin account. Its log is C:\OEM\install.log.
# The account password is in $VM_DIR/password, for the web console and RDP.
#
# 8 GB of RAM is held for as long as the VM runs (no ballooning), so stop it between test sessions.
#
# No nested virtualization, so this VM cannot run the Windows Hypervisor Platform, and the WHPX
# half of Remora's own guest (bd remora-7oih.5) cannot be tested here: only its TCG fallback and
# everything around it can. Tried on kvm_intel nested=Y: dockur's VMX=Y with its
# Hyper-V enlightenments crash-looped Windows at boot once HypervisorPlatform was enabled, and
# VMX=Y HV=N hung at the boot logo. WHPX needs a real Windows PC.

set -euo pipefail

VM_DIR="${REMORA_WINDOWS_VM_DIR:-/mnt/Build/windows-vm}"
NAME=kartend-windows
IMAGE=docker.io/dockurr/windows:6.06
KEY="$HOME/.ssh/kartend_win"
OPENSSH_URL=https://github.com/PowerShell/Win32-OpenSSH/releases/download/10.4.0.0p1-Preview/OpenSSH-Win64-v10.4.0.0.msi
OPENSSH_SHA256=788d8643464a01eb63640a2fd746f9ed23d106b5a901a35f88dc8cd40e446ac9
SSH_OPTS=(-i "$KEY" -P 2222 -o IdentitiesOnly=yes -o BatchMode=yes -o ConnectTimeout=10
          -o StrictHostKeyChecking=accept-new -o "UserKnownHostsFile=$VM_DIR/known_hosts"
          -o LogLevel=ERROR)

write_oem() {
    mkdir -p "$VM_DIR/oem" "$VM_DIR/storage" /tmp/winshare
    local msi="$VM_DIR/oem/OpenSSH-Win64.msi"
    if ! echo "$OPENSSH_SHA256  $msi" | sha256sum -c --quiet - 2>/dev/null; then
        curl -fL -o "$msi" "$OPENSSH_URL"
        echo "$OPENSSH_SHA256  $msi" | sha256sum -c --quiet -
    fi
    cp "$KEY.pub" "$VM_DIR/oem/authorized_key.pub"
    (umask 077; [[ -f "$VM_DIR/password" ]] || head -c 18 /dev/urandom | base64 | tr -d '/+=' >"$VM_DIR/password")
    # CRLF: cmd.exe misreads a batch file with bare LF line ends.
    printf '%s\r\n' \
        '@echo off' \
        'rem Runs once, as the last step of dockur'"'"'s unattended setup (this folder is copied to C:\OEM).' \
        'rem It starts sshd and authorises the host key for the admin account (tools/windows-vm.sh in Remora).' \
        'set LOG=C:\OEM\install.log' \
        'msiexec /i "C:\OEM\OpenSSH-Win64.msi" /qn /norestart /l*v C:\OEM\openssh-msi.log >> %LOG% 2>&1' \
        'if not exist "C:\ProgramData\ssh" mkdir "C:\ProgramData\ssh" >> %LOG% 2>&1' \
        'copy /y "C:\OEM\authorized_key.pub" "C:\ProgramData\ssh\administrators_authorized_keys" >> %LOG% 2>&1' \
        'rem SIDs, not names: Administrators and SYSTEM, whatever the display language.' \
        'icacls "C:\ProgramData\ssh\administrators_authorized_keys" /inheritance:r /grant *S-1-5-32-544:F /grant *S-1-5-18:F >> %LOG% 2>&1' \
        'netsh advfirewall firewall add rule name="OpenSSH Server (sshd)" dir=in action=allow protocol=TCP localport=22 >> %LOG% 2>&1' \
        'sc config sshd start= auto >> %LOG% 2>&1' \
        'net start sshd >> %LOG% 2>&1' \
        'echo done >> %LOG%' >"$VM_DIR/oem/install.bat"
}

case "${1:-}" in
up)
    if docker container inspect "$NAME" >/dev/null 2>&1; then
        docker start "$NAME" >/dev/null
    else
        write_oem
        docker run -d --name "$NAME" \
            -e VERSION=11e -e RAM_SIZE=8G -e CPU_CORES=8 -e DISK_SIZE=96G \
            -e USERNAME=kartend -e PASSWORD="$(cat "$VM_DIR/password")" \
            -p 127.0.0.1:8006:8006 -p 127.0.0.1:3389:3389/tcp -p 127.0.0.1:2222:22 \
            --device=/dev/kvm --device=/dev/net/tun --cap-add NET_ADMIN \
            -v "$VM_DIR/storage:/storage" -v "$VM_DIR/oem:/oem" -v /tmp/winshare:/shared \
            --stop-timeout 120 "$IMAGE" >/dev/null
    fi
    echo "windows-vm: starting; ssh answers once Windows is up (first install: ~20 min)"
    ;;
down)
    docker stop "$NAME" >/dev/null && echo "windows-vm: stopped"
    ;;
ssh)
    shift
    ssh_opts=("${SSH_OPTS[@]}")
    ssh_opts[2]=-p  # ssh takes -p for the port where scp takes -P
    exec ssh "${ssh_opts[@]}" kartend@127.0.0.1 "$@"
    ;;
put)
    shift
    (($# >= 2)) || { echo "usage: windows-vm.sh put <src…> <guest-dir>" >&2; exit 2; }
    dest="${*: -1}"
    exec scp -r "${SSH_OPTS[@]}" "${@:1:$#-1}" "kartend@127.0.0.1:$dest"
    ;;
*)
    sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
    ;;
esac
