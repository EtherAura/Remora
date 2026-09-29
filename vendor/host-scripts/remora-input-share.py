#!/usr/bin/env python3
"""Share host input devices with a Remora container, non-exclusively (bd remora-4ei.36).

Bare-mode successor to the vm-era evdev-replay bridge. The container shares this machine's
kernel, so sharing a device is ONE mknod: expose the real /dev/input/eventN inside the
container's /dev/input (its /dev is docker's tmpfs snapshot — the ueventd input subsystem is
patched out of the image precisely so exposure is selective) and Android's EventHub opens the
REAL device the instant the node appears. Verified live: no side EVIOCGRABs, so the
desktop and Android both receive every event, and Android sees the true bus/vendor/product —
a Bluetooth controller arrives as Bluetooth with no identity-copying machinery at all.

Two modes, one code path:

  remora-input-share.py expose <container> <name>...
      One pass. For each device NAME (matched exactly, then case-insensitively, against
      /proc/bus/input/devices) print exactly one status line:
          SHARED <eventN> <name>     node exposed (or already correct) in the container
          GRABBED <name>            another host process holds an EVIOCGRAB — sharing it would
                                    deliver silence, so it is reported instead (the usual culprit
                                    is an active Sunshine/Moonlight session)
          MISSING <name>            no such device is attached right now
      Exit 0 if every name was SHARED, 1 otherwise (advisory: the deploy reports, never aborts).

  remora-input-share.py watch <container> <name>...
      The expose pass in a loop, silently, every POLL seconds — the supervision the vm-era
      bridge never had. Unplug needs no help (EventHub drops the device on epoll hang-up, seen
      live); what the watcher adds is RE-plug: the device returns under a new event number, so
      it unlinks the stale node and mknods the new one, and Android picks it right back up.
      Exits when the container stops answering (docker exec fails twice in a row).

Deliberately stdlib-only, like its predecessor: identity comes from /proc/bus/input/devices,
the node number from the H: Handlers line, major:minor from sysfs, and the grab probe is one
ioctl pair. Runs as-is on a remote docker host (staged to ~/.remora, bd remora-4ei.88 pattern).
"""
import fcntl, os, re, subprocess, sys, time

EVIOCGRAB = 0x40044590  # _IOW('E', 0x90, int)
POLL = 2.0


def docker(container, *sh_cmd):
    """Run a shell command inside the container; returns (rc, stdout)."""
    r = subprocess.run(["docker", "exec", container, "sh", "-c", " ".join(sh_cmd)],
                       capture_output=True, text=True)
    return r.returncode, r.stdout.strip()


def host_devices():
    """name -> eventN for every device the host reports, preserving report order."""
    devs = {}
    try:
        with open("/proc/bus/input/devices") as f:
            blocks = f.read().split("\n\n")
    except OSError:
        return devs
    for block in blocks:
        name = ev = None
        for line in block.splitlines():
            if line.startswith("N: Name="):
                name = line.split("=", 1)[1].strip().strip('"')
            elif line.startswith("H: Handlers="):
                m = re.search(r"\bevent\d+\b", line)
                if m:
                    ev = m.group(0)
        if name and ev and name not in devs:
            devs[name] = ev
    return devs


def resolve(want, devs):
    """Match a configured name exactly, then case-insensitively (the picker's own rule)."""
    if want in devs:
        return devs[want]
    lowered = {n.lower(): e for n, e in reversed(devs.items())}
    return lowered.get(want.lower())


def grabbed(event):
    """True if another process holds an exclusive grab. Probe = acquire + release, the only
    reliable question the kernel answers; any errno other than EBUSY reports 'free' so an
    undecidable probe cannot block a share that would have worked (c056fa0's rule)."""
    try:
        fd = os.open(f"/dev/input/{event}", os.O_RDONLY | os.O_NONBLOCK)
    except OSError:
        return False
    try:
        fcntl.ioctl(fd, EVIOCGRAB, 1)
        fcntl.ioctl(fd, EVIOCGRAB, 0)
        return False
    except OSError as e:
        return e.errno == 16  # EBUSY
    finally:
        os.close(fd)


def ensure(container, event):
    """Make /dev/input/<event> inside the container point at the host device, idempotently.
    Mode must be right AT CREATION — EventHub opens on the inotify event and never retries a
    Permission denied (see vendor/native/uinput-kbd.c) — hence mknod -m, never mknod+chmod."""
    try:
        with open(f"/sys/class/input/{event}/dev") as f:
            maj, mn = f.read().strip().split(":")
    except OSError:
        return False
    node = f"/dev/input/{event}"
    rc, out = docker(container, f"stat -c %t:%T {node} 2>/dev/null || true")
    if rc != 0:
        return None  # container gone — the watcher's exit signal
    if out == f"{int(maj):x}:{int(mn):x}":
        return True  # already exposed and pointing at the right device
    rc, _ = docker(container, f"rm -f {node} && mknod -m 666 {node} c {maj} {mn}")
    return rc == 0


def one_pass(container, names, exposed, report):
    """Reconcile once. `exposed` maps name -> eventN from earlier passes so a replug under a
    new number (or a vanished device) gets its stale node removed."""
    devs = host_devices()
    container_alive = True
    for want in names:
        ev = resolve(want, devs)
        stale = exposed.get(want)
        if ev is None:
            if stale:
                docker(container, f"rm -f /dev/input/{stale}")
                exposed.pop(want, None)
            if report:
                print(f"MISSING {want}", flush=True)
            continue
        if stale and stale != ev:
            docker(container, f"rm -f /dev/input/{stale}")
            exposed.pop(want, None)
        if grabbed(ev):
            if report:
                print(f"GRABBED {want}", flush=True)
            continue
        ok = ensure(container, ev)
        if ok is None:
            container_alive = False
            break
        if ok:
            exposed[want] = ev
            if report:
                print(f"SHARED {ev} {want}", flush=True)
        elif report:
            print(f"MISSING {want}", flush=True)
    return container_alive


def main():
    if len(sys.argv) < 4 or sys.argv[1] not in ("expose", "watch"):
        print(__doc__.strip().splitlines()[0], file=sys.stderr)
        print("usage: remora-input-share.py expose|watch <container> <device name>...",
              file=sys.stderr)
        return 2
    mode, container, names = sys.argv[1], sys.argv[2], sys.argv[3:]

    exposed = {}
    if mode == "expose":
        one_pass(container, names, exposed, report=True)
        return 0 if len(exposed) == len(names) else 1

    dead_polls = 0
    while dead_polls < 2:
        if one_pass(container, names, exposed, report=False):
            dead_polls = 0
        else:
            dead_polls += 1
        time.sleep(POLL)
    return 0


if __name__ == "__main__":
    sys.exit(main())
