#include "engine/Engine.h"

#include <algorithm>

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QThread>

#include "core/Builders.h"
#include "core/Resolver.h"  // hostEncodeFrameSocketDirFor: the bind source and the encoder
                            // must be named by one rule, or we create a dir nothing binds in.
#include "core/Gating.h"
#include "core/Parsers.h"
#include "core/Prereqs.h"  // archVariantRefusal: the deploy refusal and the flag table are one
                           // definition, so what preflight says and what `check` says cannot drift.

namespace remora {

static LineSink mkLog(LogSink &sink, const QString &step) {
    return [&sink, step](const QString &st, const QString &t) { sink.line(step, st, t); };
}

// THE TWO LEGACY-ADOPTION SHIMS THAT STOOD HERE ARE GONE (bd remora-28ix.4 step 4).
// adoptLegacyContainer renamed a pre-rename container to `remora-<x>` at provision, and
// adoptLegacyImageTag `docker tag`-aliased a pre-rename image onto a resolved
// `remora24:<x>` name. Both were transitional by construction: they existed so a deploy could
// straddle the rename between the host flip and the first Remora-lunched build, and that build
// happened long ago. Step 4 always said they go once no pre-flip container or image remains.
//
// WHAT THIS COSTS, stated so it is not discovered: a container or image still carrying a
// pre-rename name is no longer adopted. It is not silently ignored either — provision finds no
// container under the new name and creates one, and ensure-image finds no image and says so. An
// image old enough to carry the other spelling also predates the in-image agent bake, so it
// cannot be mirrored at all and had to be rebuilt regardless; adopting it would only have let it
// resolve capabilities it can no longer deliver.

// Android 17's libcutils does not look for /dev/ashmem. ashmem-dev.cpp derives the path as
// "/dev/ashmem" + the contents of /proc/sys/kernel/random/boot_id — upstream hardened the node with
// a per-boot suffix — so a container offering the plain node makes stat() return ENOENT on a device
// that is visibly present and world-readable. Every WebView sandboxed renderer then fails, is
// killed by ActivityManager, and its layer composites BLACK: Play sign-in and any in-app web view
// show nothing (bd remora-4ei.76). Measured on the A17 image: 276,415 "Unable to stat ashmem"
// errors and a blank screencap, versus 0 and a rendered page with the link in place.
//
// Runs post-boot and in-container because /dev is the container's own, so it must be redone on
// every recreate — which is exactly why doing it by hand does not hold.
//
// No Android-version gate is needed: A16's libcutils uses a fixed path and simply ignores the extra
// link, and the guard below makes the step a no-op wherever the suffixed node already exists (a
// future ashmem module that names it correctly, or a rerun).
static StepResult ensureAshmemNode(Spawner &sp, const RunContext &ctx, LogSink &sink) {
    auto log = mkLog(sink, "ashmem-node");
    const QString c = ctx.rc.containerName;
    // Derive boot_id INSIDE the container: it is the same kernel, so the value matches the host's,
    // and reading it there keeps the whole thing one shell round-trip with no host/guest skew.
    const QString script = QStringLiteral(
        "b=$(cat /proc/sys/kernel/random/boot_id 2>/dev/null); "
        "[ -n \"$b\" ] || { echo NO_BOOT_ID; exit 0; }; "
        "[ -e \"/dev/ashmem$b\" ] && { echo ALREADY; exit 0; }; "
        "[ -e /dev/ashmem ] || { echo NO_ASHMEM; exit 0; }; "
        "ln -sf /dev/ashmem \"/dev/ashmem$b\" && echo LINKED || echo FAILED");
    const QString out =
        sp.run({"docker", "exec", c, "sh", "-c", script}, {}, log).out.trimmed();
    // Never fail the deploy on this. A device without ashmem at all still boots and mirrors — it is
    // only WebView content that suffers — so a hard failure here would cost more than it saves.
    if (out.contains(QLatin1String("LINKED")))
        return StepResult::good(QStringLiteral("boot-id ashmem node linked"));
    if (out.contains(QLatin1String("ALREADY")))
        return StepResult::good(QStringLiteral("boot-id ashmem node already present"));
    if (out.contains(QLatin1String("NO_ASHMEM")))
        return StepResult::good(
            QStringLiteral("no /dev/ashmem in the container — nothing to link (WebViews may be "
                           "black on A17; see vendor/host-prereqs/README.md for the module)"));
    return StepResult::good(QStringLiteral("could not link the boot-id ashmem node (%1)").arg(out));
}

// Hand the passed-through camera nodes to Android's camera provider (bd remora-4ei.9).
//
// A `--device /dev/videoN` arrives inside the container owning the DOCKER HOST's video gid (44 on
// most distributions), which means nothing in Android: the external camera provider runs as user
// cameraserver, whose groups are audio/camera/input/drmrpc/usb — gid 1006, AID_CAMERA — so it
// cannot open the node at all and enumerates zero cameras. 0:1006 with mode 0660 is what lets it
// open: verified when this lived in the vm bringup script, where the HAL went from silence to
// "ExtCamDev: trimSupportedFormats: input format list is empty!", i.e. a complaint about the
// SOURCE's formats rather than an access failure.
//
// This is a RE-LANDING. The fix existed only in the retired guest bring-up script and was
// deleted with the vm backend, leaving bare and remote passing the node in with nothing to make it
// usable — so camera passthrough has been silently inert on every surviving backend since.
//
// Runs after boot, like the other device fix-ups: /dev is docker's tmpfs snapshot and Android's own
// init re-creates nothing here, so the ownership only has to be right before the provider opens it.
static StepResult ensureCameraNodes(Spawner &sp, const RunContext &ctx, LogSink &sink) {
    if (ctx.rc.cameraDevices.isEmpty()) {
        // "Asked for and absent" must not read like "nobody wanted one". Same shape as the ashmem
        // step: still not fatal — a device without a camera boots and mirrors — but the detail
        // names the cause and the consequence instead of a cheerful nothing-to-do.
        if (ctx.rc.cameraWantedButNoneFound)
            return StepResult::good(QStringLiteral(
                "camera_v4l2 is ON but the host has no /dev/video* — nothing was passed through, "
                "so Android will find NO cameras. Check the camera is plugged in and its node "
                "exists on the docker host."));
        return StepResult::good(QStringLiteral("no cameras passed through — nothing to do"));
    }
    auto log = mkLog(sink, "camera-nodes");
    // Only the nodes this profile actually passed, never a blanket /dev/video*: another container
    // or the host may own nodes we were not given, and chowning those would be reaching outside
    // what the operator handed us.
    QStringList quoted;
    for (const QString &cam : ctx.rc.cameraDevices)
        quoted << QStringLiteral("'%1'").arg(QString(cam).replace(QLatin1Char('\''), QString()));
    // …AND RESTART THE PROVIDER, which is what actually makes the camera appear (bd remora-4ei.9).
    // The ownership fix alone is too late to matter: a passed-through node arrives owned by the
    // HOST's video gid, which means nothing inside Android, and the external camera provider starts
    // at `class hal` — long before this step, which runs after boot-wait. It scans /dev once at
    // startup, fails to open a node it has no group for, and from then on only reacts to inotify
    // CREATE/DELETE. A chown fires neither, so the provider never looks again and dumpsys reports
    // "Number of camera devices: 0" forever, with the node sitting there correctly owned.
    //
    // Measured on the live instance: at boot the provider logged nothing about /dev/video0 and
    // enumerated 0 devices; `ctl.restart` after the chown made it claim the node and Android went
    // to 1. Restarting is cheap and idempotent — it is a stateless HAL with no session to lose,
    // and it happens before any app could have opened a camera.
    //
    // Deliberately restarting rather than moving the chown earlier: the node set is discovered per
    // deploy, so an init .rc would have to be generated per profile to name them, and the ordering
    // against `class hal` would still be a race. This makes the rescan explicit instead.
    const QString script =
        QStringLiteral("n=0; for d in %1; do [ -e \"$d\" ] || continue; "
                       "chown 0:1006 \"$d\" 2>/dev/null && chmod 0660 \"$d\" 2>/dev/null && "
                       "n=$((n+1)); done; "
                       "[ \"$n\" -gt 0 ] && setprop ctl.restart vendor.camera.provider-ext; "
                       "echo \"FIXED $n\"")
            .arg(quoted.join(QLatin1Char(' ')));
    const QString out =
        sp.run({"docker", "exec", ctx.rc.containerName, "sh", "-c", script}, {}, log).out.trimmed();
    // Advisory, like the ashmem node: a device with an unusable camera still boots and mirrors, so
    // failing the deploy here would cost far more than the feature is worth.
    if (out.startsWith(QLatin1String("FIXED 0")))
        return StepResult::good(QStringLiteral(
            "no camera node could be handed to cameraserver — the provider will find no cameras"));
    if (out.startsWith(QLatin1String("FIXED")))
        return StepResult::good(
            QStringLiteral("%1 camera node(s) handed to cameraserver (0:1006)")
                .arg(out.mid(6)));
    return StepResult::good(QStringLiteral("could not fix the camera nodes (%1)").arg(out));
}

// Hand /dev/snd to Android's audioserver so the USB audio HAL can open the capture card
// (bd remora-4ei.37). Exactly the camera step's problem one device class over, and it was found
// the same way — by the HAL failing rather than by reading code:
//     modules.usbaudio.audio_hal: adev_open_input_stream fail - cannot read profile
//     APM_AudioPolicyManager: Cannot open input stream for device
//         {AUDIO_DEVICE_IN_USB_DEVICE, @:card=0;device=0} on hw module usb
// The nodes arrive owning the DOCKER HOST's audio gid — 18 on most distributions — which means
// nothing in Android: audioserver is uid 1041 with gid 1005 (AID_AUDIO) and supplementary groups
// 1006/1013/1026/1031/3001/3002/3007/3010, none of them 18. So it cannot open the node, the
// attached device fails to enumerate, and AudioSource.MIC silently falls back to the primary
// STUB. 0:1005 with 0660 is what lets it open: verified live, "Available input devices" went
// from 2 to 3 with "USB Device In" at card=0;device=0 appearing.
//
// BLANKET /dev/snd/*, UNLIKE THE CAMERA STEP, and the difference is real rather than sloppy:
// camera nodes are handed over one at a time by an explicit --device grant, so chowning anything
// else would reach past what the operator gave us. /dev/snd is not granted per node — the whole
// directory is there because the container is privileged — so there is no smaller set to respect.
//
// AND IT DOES NOT ESCAPE TO THE HOST, which is the thing worth checking before chowning anything
// in a privileged container (the kernel.modprobe sysctl escapes exactly that way — see the
// keep_kernel_modprobe patch). MEASURED, not assumed: after chmod inside the container the host's
// /dev/snd/pcmC0D0c was still 0660 while the container's read 0666, so docker's /dev really is a
// separate tmpfs here.
//
// Restarts audioserver for the camera step's reason: the policy enumerates its attached devices
// once at startup, long before this step, and a chown fires no event that would make it look
// again. Cheap and idempotent this early — nothing has an audio session yet.
static StepResult ensureAudioNodes(Spawner &sp, const RunContext &ctx, LogSink &sink) {
    if (!ctx.rc.usbAudio)
        return StepResult::good(QStringLiteral("usb_audio is off — /dev/snd left as it arrived"));
    auto log = mkLog(sink, "audio-nodes");
    const QString script = QStringLiteral(
        "n=0; for d in /dev/snd/*; do [ -c \"$d\" ] || continue; "
        "chown 0:1005 \"$d\" 2>/dev/null && chmod 0660 \"$d\" 2>/dev/null && n=$((n+1)); done; "
        "[ \"$n\" -gt 0 ] && setprop ctl.restart audioserver; echo \"FIXED $n\"");
    const QString out =
        sp.run({"docker", "exec", ctx.rc.containerName, "sh", "-c", script}, {}, log).out.trimmed();
    // Advisory like the camera and ashmem steps: an image whose microphone does not work still
    // boots and mirrors, and failing the deploy over it would cost far more than the capability.
    if (out.startsWith(QLatin1String("FIXED 0")))
        return StepResult::good(QStringLiteral(
            "no /dev/snd node could be handed to audioserver — the USB audio HAL will find no "
            "card and capture falls back to the silent stub"));
    if (out.startsWith(QLatin1String("FIXED")))
        return StepResult::good(QStringLiteral("%1 /dev/snd node(s) handed to audioserver (0:1005)")
                                    .arg(out.mid(6)));
    return StepResult::good(QStringLiteral("could not fix the /dev/snd nodes (%1)").arg(out));
}

// Give Android a physical keyboard to believe in. With none attached the framework reports
// Configuration `nokeys`, which is exactly what puts the on-screen IME on every text field and
// takes the Physical keyboard section out of Settings — the operator asked to type on their own
// keyboard, not on one drawn over the mirror.
//
// It is a DECOY and nothing more: it delivers no keystrokes. Real keys keep arriving through
// the mirror's --keyboard=sdk injection. The device exists so the framework flips `nokeys` -> `qwerty`,
// and it carries the operator's own vendor/product because Gboard gates its physical-keyboard
// toolbar on a keyboard that looks real — which is the whole point of kbd_identity.
//
// This replaces the guest persist-kbd trick that went with the vm backend (bd remora-gf7). The mirror's
// own --keyboard=uhid cannot stand in for it: the UHID device is created on the HOST kernel, the
// container's /dev is docker's tmpfs snapshot, and the container's own netns means the kernel's
// uevent for the new device never arrives — so no /dev/input node ever appears and Android sees
// nothing. The helper sidesteps that by making its node itself; see vendor/native/uinput-kbd.c.
//
// The identity is read from the DOCKER HOST's /proc/bus/input/devices, so bare reads this machine
// and remote reads the machine the container is actually on. The container shares that kernel, so
// it is the same device list either way.
StepResult phantomKeyboard(Spawner &sp, const RunContext &ctx, LogSink &sink) {
    auto log = mkLog(sink, "phantom-kbd");
    const QString c = ctx.rc.containerName;
    const bool remote = ctx.backend == Backend::Remote;
    const auto sh = [&](const QString &cmd) {
        return remote ? sp.run(Spawner::sshArgv(ctx.guest, cmd), {}, log)
                      : sp.run({"sh", "-c", cmd}, {}, log);
    };
    // /remora/uinput-kbd on BOTH backends: remote's run argv now binds the staged
    // ~/.remora/uinput-kbd there (applyVendorPaths + host-prereqs, bd remora-4ei.88), so the old
    // remote-only push-then-`docker cp` is gone — the cp could not have worked anyway, since init
    // remounts the rootfs read-only once boot completes (bd remora-4ei.59).
    const QString binary = QStringLiteral("/remora/uinput-kbd");
    // Idempotent: a reconnect onto a container that already has one must not stack a second
    // keyboard (each would take another /dev/input node and Android would list them all).
    if (sh(QStringLiteral("docker exec %1 sh -c 'pidof uinput-kbd >/dev/null'").arg(c)).rc == 0)
        return StepResult::good(QStringLiteral("phantom keyboard already running"));

    QStringList env;
    QString who = QStringLiteral("generic");
    // Non-empty => an identity was WANTED and did not arrive. Falling back to generic is silent
    // otherwise, and it is not a cosmetic fallback: Gboard gates its physical-keyboard toolbar on
    // a keyboard that looks real, which is the entire reason this step impersonates one. Observed
    // a USB hub carrying the operator's keyboard was unplugged and two deploys
    // differed only in the word "generic" (bd remora-4ei.9's sibling finding).
    QString missing;
    // "none" is the picker's "Generic — no Gboard toolbar": still a keyboard, just not anyone's.
    // Deliberate, so it stays quiet.
    if (ctx.rc.kbdIdentity.value_or(QString()) != QLatin1String("none")) {
        const QString devs = sh(QStringLiteral("cat /proc/bus/input/devices")).out;
        const auto kbds = parseInputDevicesKeyboards(devs);
        if (const auto k = selectKbdIdentity(kbds, ctx.rc.kbdIdentity)) {
            for (const QString &kv : kbdIdentityEnv(*k)) env << QStringLiteral("-e") << kv;
            who = k->name;
        } else if (ctx.rc.kbdIdentity.has_value()) {
            // A NAMED keyboard that is not attached. Worth distinguishing from "none attached at
            // all": the answer is to plug that one in, or to re-pick on the Device page.
            missing = QStringLiteral("the configured keyboard '%1' is not attached to the %2 (%3 "
                                     "other keyboard(s) are)")
                          .arg(*ctx.rc.kbdIdentity,
                               remote ? QStringLiteral("docker host") : QStringLiteral("host"))
                          .arg(kbds.size());
        } else {
            missing = QStringLiteral("no real keyboard is attached to the %1")
                          .arg(remote ? QStringLiteral("docker host") : QStringLiteral("host"));
        }
    }
    QStringList argv{"docker", "exec", "-d"};
    argv << env << c << binary;
    // Locally the argv goes to QProcess as a list, so spaces need nothing. Remotely it becomes one
    // shell string, and REMORA_KBD_NAME is very often several words ("Keychron K8 Pro Keyboard") —
    // unquoted, the remote sh would read those as further arguments and docker would reject them.
    // Double-quoted, not single: sshArgv single-quotes the whole payload, and a nested ' would end
    // it. Nothing here can carry a " — these values come from the kernel's own device names.
    const auto r =
        remote ? sh([&] {
            QStringList q;
            for (const QString &a : argv)
                q << (a.contains(QLatin1Char(' ')) ? QStringLiteral("\"%1\"").arg(a) : a);
            return q.join(QLatin1Char(' '));
        }())
               : sp.run(argv, {}, log);
    // Never fail the deploy on this. A device without the decoy still boots, mirrors and types —
    // it just shows the on-screen keyboard — so a hard failure here would cost more than it saves.
    if (r.rc != 0)
        return StepResult::good(QStringLiteral("could not start the phantom keyboard — the "
                                               "on-screen keyboard will show"));
    if (!missing.isEmpty())
        return StepResult::good(
            QStringLiteral("phantom keyboard up (generic) — %1, so Gboard will NOT show its "
                           "physical-keyboard toolbar. Typing and the hidden on-screen IME are "
                           "unaffected.")
                .arg(missing));
    return StepResult::good(QStringLiteral("phantom keyboard up (%1)").arg(who));
}

// Share the operator's chosen input devices with the container, non-exclusively — the bare-mode
// replacement for the vm-era evdev-replay bridge (bd remora-4ei.36). The container shares the
// docker host's kernel, so the whole mechanism is remora-input-share.py mknod'ing the REAL
// /dev/input node into the container's /dev tmpfs: EventHub opens it on the inotify event,
// nothing grabs, and both sides receive every event (verified live — see the bead).
//
// The step runs one synchronous `expose` pass so the deploy can SAY what happened per device,
// then leaves a detached `watch` behind for replugs (a Bluetooth device that drops comes back
// under a new event number; Android handles the unplug itself via epoll hang-up). The previous
// watcher is always killed first — the device list may have changed, and a stale watcher would
// keep re-exposing devices the operator just unticked.
StepResult shareInputDevices(Spawner &sp, const RunContext &ctx, LogSink &sink) {
    auto log = mkLog(sink, "shared-inputs");
    const QString c = ctx.rc.containerName;
    const bool remote = ctx.backend == Backend::Remote;
    const auto sh = [&](const QString &cmd) {
        return remote ? sp.run(Spawner::sshArgv(ctx.guest, cmd), {}, log)
                      : sp.run({"sh", "-c", cmd}, {}, log);
    };
    // Double quotes, not single: sshArgv single-quotes the whole payload (see phantomKeyboard).
    sh(QStringLiteral("pkill -f \"remora-input-share.py watch %1\"").arg(c));
    if (ctx.rc.sharedInputs.isEmpty())
        return StepResult::good(QStringLiteral("no input devices shared"));

    // ~/.remora on the remote (staged by deployRemote, the uinput-kbd pattern); the checkout's
    // own copy locally. Names carry spaces, so locally they travel as argv elements and remotely
    // each is double-quoted into the one shell string ssh gets.
    const QString script = remote ? QStringLiteral("$HOME/.remora/remora-input-share.py")
                                  : vendorScript(QStringLiteral("host-scripts"),
                                                 QStringLiteral("remora-input-share.py"));
    QStringList argv{QStringLiteral("python3"), script, QStringLiteral("expose"), c};
    argv << ctx.rc.sharedInputs;
    const auto quoted = [&] {
        QStringList q;
        for (const QString &a : argv)
            q << (a.contains(QLatin1Char(' ')) ? QStringLiteral("\"%1\"").arg(a) : a);
        return q.join(QLatin1Char(' '));
    };
    const auto r = remote ? sh(quoted()) : sp.run(argv, {}, log);
    // Advisory from here on: a device that cannot be shared still leaves a bootable, mirrored
    // instance, so failing the deploy would cost far more than the feature is worth.
    if (r.rc != 0 && r.out.trimmed().isEmpty())
        return StepResult::good(
            QStringLiteral("could not run remora-input-share.py — no devices shared"));

    QStringList shared, absent;
    for (const QString &line : r.out.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        if (line.startsWith(QLatin1String("SHARED ")))
            shared << line.section(QLatin1Char(' '), 2);
        else if (line.startsWith(QLatin1String("GRABBED ")))
            absent << QStringLiteral("%1 (grabbed by another process — an active "
                                     "Sunshine/Moonlight session?)")
                          .arg(line.mid(8));
        else if (line.startsWith(QLatin1String("MISSING ")))
            absent << QStringLiteral("%1 (not attached)").arg(line.mid(8));
    }

    // The watcher owns replug handling; its own exit condition is the container stopping. The
    // spawn is detached AND scoped locally (spawnDetached lifts it out of the app unit's cgroup,
    // bd remora-4ei.29) so it survives Remora exiting, like the mirror; remotely nohup does the
    // same job under sshd.
    QStringList watch{QStringLiteral("python3"), script, QStringLiteral("watch"), c};
    watch << ctx.rc.sharedInputs;
    if (remote) {
        QStringList q;
        for (const QString &a : watch)
            q << (a.contains(QLatin1Char(' ')) ? QStringLiteral("\"%1\"").arg(a) : a);
        sh(QStringLiteral("nohup %1 >/dev/null 2>&1 &").arg(q.join(QLatin1Char(' '))));
    } else {
        sp.spawnDetached(watch, {});
    }

    if (shared.isEmpty())
        return StepResult::good(
            QStringLiteral("no shared input device is usable right now — %1")
                .arg(absent.join(QLatin1String("; "))));
    QString detail = QStringLiteral("sharing %1").arg(shared.join(QLatin1String(", ")));
    if (!absent.isEmpty())
        detail += QStringLiteral(" — not shared: %1").arg(absent.join(QLatin1String("; ")));
    return StepResult::good(detail);
}

// Play spoof (ReZygisk + PlayIntegrityFix) for the docker-argv backends. Ported from the vm
// bringup's block, which had it only there — so a bare profile with play_spoof=true silently got
// nothing, its device stayed uncertified, and Google sign-in failed with "Something went wrong"
// (bd remora-4ei.78).
//
// Everything runs through `docker exec`; nothing needs a rezmods dir mounted into the container.
// The command that seeds ReZygisk's temp dir, runs PIF's post-fs-data.sh and launches the injector.
// Shared by the pre-boot arming step and the post-boot fallback, because getting these three in the
// wrong order is what makes the spoof silently not apply.
static QString rezygiskArmCmd() {
    return QStringLiteral(
        "export TMP_PATH=/data/adb/rezygisk; rm -rf \"$TMP_PATH\"; mkdir -p \"$TMP_PATH\"; "
        "chmod 555 \"$TMP_PATH\"; chcon u:object_r:system_file:s0 \"$TMP_PATH\" 2>/dev/null || true; "
        "[ -f /data/adb/modules/playintegrityfix/post-fs-data.sh ] && "
        "sh /data/adb/modules/playintegrityfix/post-fs-data.sh >/dev/null 2>&1; "
        "cd /data/adb/modules/rezygisk && setsid ./bin/zygisk-ptrace64 monitor "
        ">/dev/null 2>&1 </dev/null &");
}

// Install the spoof modules into /data if the image carries newer ones. Returns false when this
// image has no spoof payload at all.
static bool spoofModulesReady(Spawner &sp, const RunContext &ctx, const LineSink &log) {
    const QString c = ctx.rc.containerName;
    // IMAGE gate: the modules are baked at /system/etc/remora-spoof by the play_spoof build
    // feature. Without them there is nothing to install, and saying so beats a silent no-op.
    if (sp.run({"docker", "exec", c, "test", "-d", "/system/etc/remora-spoof"}, {}, log).rc != 0)
        return false;
    // Re-install whenever the IMAGE carries a module /data lacks, or whose content drifted. /data
    // is a persistent volume, so "first boot" effectively never happens again: presence alone would
    // let a changed module keep its stale /data copy forever (bd remora-ckl).
    //
    // POST-FS-DATA.SH ALONE IS NOT A WITNESS FOR A VERSION BUMP, and comparing only that let one
    // through in silence (bd remora-4ei.86). The ReZygisk 515 -> 537 bump moved the BINARIES and
    // module.prop and nothing else — every support script, post-fs-data.sh included, is byte
    // identical across it. So the gate saw no drift, /data kept its 515 copy, and this host ran a
    // container off an image carrying 537 with 537 baked at /system/etc/remora-spoof and 515
    // installed in /data. Measured, not reasoned: the live module.prop read
    // "version=v1.0.0 (515-333d423-release)" while the image's read 537. Nothing reported anything,
    // and an acceptance test for the bump would have measured the old build believing it was new.
    //
    // Two more witnesses, each covering what the other cannot:
    //   versionCode — the field an upstream bump always moves, and the one field Magisk does NOT
    //     rewrite on the /data side (it rewrites description, which is why the whole file is still
    //     not compared).
    //   bin/ contents — because OUR OWN payload can move without the version moving: the injector
    //     is patched at assemble time, so re-deriving the patch produces different bytes under an
    //     unchanged versionCode. install.sh chmods bin/ but never rewrites it, so a content compare
    //     is stable and cannot fire spuriously.
    const QString driftCheck = QStringLiteral(
        "for d in /system/etc/remora-spoof/*/; do "
        "case \"$(basename \"$d\")\" in rez-mod) n=rezygisk ;; playintegrityfix) n=playintegrityfix ;; "
        "shamiko) n=zygisk_shamiko ;; devicespoof) n=remora_devicespoof ;; nb_sigfix) n=nb_sigfix ;; "
        "*) continue ;; esac; "
        "m=/data/adb/modules/$n; [ -d \"$m\" ] || exit 1; "
        "if [ -f \"${d}post-fs-data.sh\" ]; then "
        "cmp -s \"${d}post-fs-data.sh\" \"$m/post-fs-data.sh\" || exit 1; fi; "
        "if [ -f \"${d}module.prop\" ]; then "
        "[ \"$(sed -n 's/^versionCode=//p' \"${d}module.prop\")\" = "
        "\"$(sed -n 's/^versionCode=//p' \"$m/module.prop\" 2>/dev/null)\" ] || exit 1; fi; "
        "if [ -d \"${d}bin\" ]; then for b in \"${d}bin/\"*; do "
        "cmp -s \"$b\" \"$m/bin/$(basename \"$b\")\" || exit 1; done; fi; "
        // nb_sigfix's whole payload is zygisk/x86_64.so and it carries no bin/ or post-fs-data.sh,
        // so without comparing zygisk/ a rebuilt .so under an unchanged versionCode would keep its
        // stale /data copy — the same silent-drift trap bd remora-4ei.86 hit for ReZygisk's bins.
        "if [ -d \"${d}zygisk\" ]; then for b in \"${d}zygisk/\"*; do "
        "cmp -s \"$b\" \"$m/zygisk/$(basename \"$b\")\" || exit 1; done; fi; done; "
        "[ -f /system/etc/remora-spoof/pif.json ] && "
        "! cmp -s /system/etc/remora-spoof/pif.json /data/adb/modules/playintegrityfix/pif.json && exit 1; "
        "exit 0");
    if (sp.run({"docker", "exec", c, "sh", "-c", driftCheck}, {}, log).rc != 0)
        sp.run({"docker", "exec", c, "sh", "/system/etc/remora-spoof/install.sh"}, {}, log);

    // Clear stray `disable` markers, every deploy.
    //
    // Magisk disables EVERY module at once when its bootloop protection trips, and a run of failed
    // deploys is enough to trip it — observed here as four modules carrying a disable file with
    // one identical mtime, to the nanosecond. playSpoof's module loop skips any module with that
    // file, so the post-boot half silently stops running and the device is left unspoofed with
    // every step green.
    //
    // install.sh already knows this failure mode — "a stray 'disable' file is what kept ReZygisk
    // from ever starting" — but it only runs when module CONTENT has drifted, which it has not, so
    // nothing was healing it. Doing it here makes the recovery unconditional, which is what a
    // condition that can appear at any time requires. Reported when it fires, because a module
    // Magisk disabled is worth knowing about even after it is fixed.
    const QString enable = QStringLiteral(
        "n=0; for m in rezygisk playintegrityfix zygisk_shamiko remora_devicespoof nb_sigfix; do "
        "[ -f /data/adb/modules/$m/disable ] && { rm -f /data/adb/modules/$m/disable; n=$((n+1)); }; "
        "done; echo $n");
    const QString cleared =
        sp.run({"docker", "exec", c, "sh", "-c", enable}, {}, log).out.trimmed();
    if (!cleared.isEmpty() && cleared != QLatin1String("0"))
        log(QStringLiteral("stderr"),
            QStringLiteral("re-enabled %1 spoof module(s) that had been disabled — Magisk does this "
                           "to every module at once when its bootloop protection trips")
                .arg(cleared));
    return true;
}

// Install the post-fs-data hook that starts the injector on EVERY boot, before zygote.
//
// magiskd runs /data/adb/post-fs-data.d/* at the post-fs-data trigger, which is upstream of zygote
// by construction — so a script there is not racing anything. vendor/…/50-rezygisk-pif.sh was
// written for exactly this and NOTHING INSTALLED IT: it sat vendored and unreferenced while the
// deploy did the expensive thing instead (boot, stop, inject, start), paying a second full Android
// boot every time (bd remora-tdz).
//
// Read from the vendored file rather than embedded here, so there is one copy to keep right, and
// written through `sh -c` because `docker cp` cannot reach a running Android container at all —
// its rootfs is remounted read-only and the copy needs a scratch dir at the root (bd remora-4ei.59,
// reproduced again here: "mkdir …/merged/dev/venus: read-only file system").
//
// Installed POST-boot, deliberately. Doing it before boot-wait cannot work: this early the
// container has not reached the init-time overlay, so /data is not yet the filesystem the running
// system will see, and the write lands nowhere useful (measured: the step reported failure at
// ~0.15 s in). /data persists across container recreates, so post-boot is soon enough — this boot
// still pays the restart, and every cold deploy after it boots once.
static bool installSpoofBootHook(Spawner &sp, const RunContext &ctx, const LineSink &log) {
    QFile f(vendorScript(QStringLiteral("source-build/features/zygisk_pif"),
                         QStringLiteral("50-rezygisk-pif.sh")));
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QString body = QString::fromUtf8(f.readAll());
    // The heredoc terminator must not occur in the payload, or the write truncates silently.
    if (body.isEmpty() || body.contains(QLatin1String("REMORA_EOF"))) return false;
    const QString dst = QStringLiteral("/data/adb/post-fs-data.d/50-rezygisk-pif.sh");
    const QString cmd = QStringLiteral("mkdir -p /data/adb/post-fs-data.d && "
                                       "cat > %1 <<'REMORA_EOF'\n%2\nREMORA_EOF\n"
                                       "chmod 755 %1")
                            .arg(dst, body);
    return sp.run({"docker", "exec", ctx.rc.containerName, "sh", "-c", cmd}, {}, log).rc == 0;
}

// Take `su` off the app PATH (bd remora-4ei.101). Same install shape as the hook above — vendored
// file, written through `sh -c` because docker cp cannot reach a running Android container — but
// into service.d, NOT post-fs-data.d: module post-fs-data scripts resolve `magisk`/`su` by bare
// name (Shamiko's does, and mis-branches if magisk is missing), so the links have to outlive that
// pass. The script itself carries the full rationale and the measurements behind it.
static bool installAppletHideHook(Spawner &sp, const RunContext &ctx, const LineSink &log) {
    QFile f(vendorScript(QStringLiteral("source-build/features/zygisk_pif"),
                         QStringLiteral("60-hide-magisk-applets.sh")));
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QString body = QString::fromUtf8(f.readAll());
    if (body.isEmpty() || body.contains(QLatin1String("REMORA_EOF"))) return false;
    const QString dst = QStringLiteral("/data/adb/service.d/60-hide-magisk-applets.sh");
    const QString cmd = QStringLiteral("mkdir -p /data/adb/service.d && "
                                       "cat > %1 <<'REMORA_EOF'\n%2\nREMORA_EOF\n"
                                       "chmod 755 %1")
                            .arg(dst, body);
    return sp.run({"docker", "exec", ctx.rc.containerName, "sh", "-c", cmd}, {}, log).rc == 0;
}

// Arm the injector before Android's first zygote, by WAITING for the moment rather than guessing it.
//
// The post-fs-data hook (bd remora-v8h) works, but only sometimes: /data is an init-time overlay
// here, the hook lives in the merged view, and whether magiskd's post-fs-data pass sees it is a race
// nobody outside init controls. Five identical cold deploys went hook, hook, hook, fallback,
// fallback — and losing costs the full ~14s framework restart (bd remora-d6r).
//
// So do it from this side too, where the timing IS controllable. Measured on this host, from
// container start: /system/etc/remora-spoof visible at 0.15s, the rezygisk binary executable at
// 0.68s, zygote at 1.75s. That is a full second of room — but only if we POLL for it. The earlier
// attempt fired once, immediately, found /system not yet mounted and gave up ("image has no
// play_spoof payload"), which is why it never once won.
//
// Budget deliberately shorter than zygote's arrival plus a margin: past that there is nothing left
// to race, and holding the chain to keep trying would cost more than the restart it is avoiding.
// The hook stays in place regardless — when it wins, this step finds the tracer already up and the
// idempotence check inside rezygiskArmCmd makes the second start a no-op.
static StepResult playSpoofArm(Spawner &sp, const RunContext &ctx, LogSink &sink) {
    auto log = mkLog(sink, "spoof-arm");
    const QString c = ctx.rc.containerName;
    if (!ctx.rc.playSpoof) return StepResult::good("play_spoof off");
    constexpr int kPollMs = 100;
    constexpr int kBudgetMs = 3000;
    const QString ready =
        QStringLiteral("test -d /system/etc/remora-spoof && "
                       "test -x /data/adb/modules/rezygisk/bin/zygisk-ptrace64");
    int waited = 0;
    bool armed = false;
    for (; waited < kBudgetMs; waited += kPollMs) {
        if (sp.run({"docker", "exec", c, "sh", "-c", ready}, {}, log).rc == 0) {
            armed = true;
            break;
        }
        sp.waitMs(kPollMs);
    }
    if (!armed)
        return StepResult::good(
            "the spoof modules were not ready before zygote — the boot hook or the framework "
            "restart will cover it");
    sp.run({"docker", "exec", c, "sh", "-c", rezygiskArmCmd()}, {}, log);
    return StepResult::good(
        QStringLiteral("injector armed %1 ms into the container's life").arg(waited));
}

static StepResult playSpoof(Spawner &sp, const RunContext &ctx, LogSink &sink) {
    auto log = mkLog(sink, "play-spoof");
    const QString c = ctx.rc.containerName;
    if (!ctx.rc.playSpoof) return StepResult::good("play_spoof off");
    if (!spoofModulesReady(sp, ctx, log))
        return StepResult::good(
            "play_spoof is on but this image was not built with the play_spoof feature — rebuild "
            "with it, or turn the option off");

    // Root is a PRECONDITION of everything below, not a side effect of it — and nothing else in
    // the chain proves it came up. magisk.rc's setup runs at init, ReZygisk is injected by Remora
    // itself rather than through magiskd, so the whole root stack can fail wholesale while every
    // step stays green: the first attempt at swapping in upstream Magisk did exactly that (its CLI
    // lacks Delta's setup flags, so MAGISKTMP was never built — no magiskd, no su, no denylist),
    // and the only symptoms were a longer boot animation and a session of misdiagnosis (bd
    // remora-82c.13). That swap has since been done properly — the shipped payload IS upstream
    // v30.7 and magisk-setup.sh builds the environment — but this gate is what makes the class of
    // failure visible at all, so it stays regardless of which build is current.
    // Ask the three questions in dependency order and fail LOUDLY, naming the first gap.
    // The retries are margin for a busy host, not a second boot: magiskd starts at post-fs-data,
    // long before the boot_completed this step already waited for, so a healthy stack answers on
    // the first ask and the wait is only ever paid on the way to a failure.
    // BOTH MAGISKTMP paths are on the PATH, and that is not belt-and-braces. magisk.rc moved
    // MAGISKTMP from /sbin to /debug_ramdisk (bd remora-4ei.94 — a /sbin directory is a root tell
    // that no DenyList can hide, because it is a directory and not a mount), but images built
    // before that change still put the CLI at /sbin, and this binary has to drive both.
    // /debug_ramdisk goes first so a current image resolves without touching a stale /sbin.
    const QString rootProbe = QStringLiteral(
        "export PATH=/debug_ramdisk:/sbin:$PATH; "
        "command -v magisk >/dev/null 2>&1 || { echo NO_MAGISKTMP; exit 1; }; "
        "pgrep -x magiskd >/dev/null 2>&1 || { echo NO_DAEMON; exit 1; }; "
        "su -c true >/dev/null 2>&1 || { echo NO_SU; exit 1; }; echo OK");
    QString rootGap;
    for (int i = 0; i < 3; ++i) {
        const ProcResult r = sp.run({"docker", "exec", c, "sh", "-c", rootProbe}, {}, log);
        rootGap = r.out.trimmed();
        if (r.rc == 0) break;
        sp.waitMs(1000);
    }
    if (rootGap != QLatin1String("OK")) {
        const QString why =
            rootGap == QLatin1String("NO_MAGISKTMP")
                ? QStringLiteral("MAGISKTMP (/debug_ramdisk, or /sbin on a pre-4ei.94 image) was "
                                 "never set up, so the magisk CLI is not on any PATH")
            : rootGap == QLatin1String("NO_DAEMON")
                ? QStringLiteral("MAGISKTMP exists but magiskd is not running")
            : rootGap == QLatin1String("NO_SU")
                ? QStringLiteral("magiskd is up but `su` does not grant root")
                : QStringLiteral("the in-container probe did not answer");
        return StepResult::fail(QStringLiteral(
            "root did not come up: %1. A green ReZygisk injection does NOT imply root — the "
            "spoof would look installed and silently not apply. Magisk needs its tmpfs "
            "environment built before any of it works, and in a container nothing builds that "
            "except the image's own magisk-setup.sh: an image whose Magisk payload and magisk.rc "
            "disagree fails exactly this way, silently "
            "(vendor/source-build/features/magisk/magisk/PROVENANCE.md, bd remora-82c.13). "
            "Rebuild with a matched pair, or redeploy the last known-good image. Or set "
            "play_spoof=false to bring the device up without root.")
                                    .arg(why));
    }

    // Make the NEXT boot free. Costs one file write; saves a whole Android boot every cold deploy
    // from here on.
    installSpoofBootHook(sp, ctx, log);

    // Take `su` off the app PATH from the next boot on (bd remora-4ei.101). Same one-file-write
    // cost, and installed here for the same reason: /data persists across recreates, so writing it
    // post-boot is soon enough and every later boot picks it up on its own.
    installAppletHideHook(sp, ctx, log);


    // Has the post-boot module work already run for THIS framework? Keyed on system_server's pid
    // AND on a marker in /dev, which is a tmpfs and therefore dies with the container.
    //
    // The pid alone is NOT enough, and believing it was cost a silently unspoofed device: each
    // container gets a fresh PID namespace where pids restart low and the boot is deterministic, so
    // a recreated container hands system_server the SAME pid remarkably often. Caught live —
    // stored 540, current 540, different containers — and the consequence is exactly the silent
    // one this comment used to warn about: the step reports "already injected", the module work is
    // skipped, and ro.product.model reads the product's own name instead of Pixel 8 Pro.
    //
    // /dev makes the three cases come out right: a container recreate wipes it (fresh), a framework
    // stop/start keeps it but changes the pid (fresh), and a warm reconnect matches both (skip).
    //
    // The question matters because the two cases need opposite things, and it used to be answered
    // by "is the spoof injected?", which stopped being the same question the moment the boot hook
    // could do the injecting: a warm reconnect must re-run nothing, while a boot that came up with
    // the injector ALREADY in place still owes the modules their post-boot half — the service.sh
    // work that nothing else triggers, because there was no framework restart. Getting this wrong
    // is silent: the spoof looks installed and simply does not apply.
    const QString genCheck = QStringLiteral(
        "cur=$(pgrep -f 'system_serve[r]' | head -1); [ -n \"$cur\" ] || exit 0; "
        "[ \"$cur\" = \"$(cat /dev/.remora-spoof-gen 2>/dev/null)\" ] && exit 1; exit 0");
    const bool freshGeneration =
        sp.run({"docker", "exec", c, "sh", "-c", genCheck}, {}, log).rc == 0;

    // Any process carrying libzygisk means a live ReZygisk.
    const QString injected = QStringLiteral(
        "for p in /proc/[0-9]*; do grep -ql libzygisk \"$p/maps\" 2>/dev/null && exit 0; done; exit 1");
    const bool isInjected =
        sp.run({"docker", "exec", c, "sh", "-c", injected}, {}, log).rc == 0;
    if (isInjected && !freshGeneration)
        return StepResult::good("play spoof already injected");
    if (!isInjected) {

    // The arming step ahead of boot-wait did not take — so fall back to what this always did:
    // bounce the framework and catch the zygote that `start` spawns. Costs a second full boot,
    // which is exactly why it is the fallback now and not the default (bd remora-tdz).
    log(QStringLiteral("stderr"),
        QStringLiteral("the pre-boot injector did not take — restarting the framework to catch a "
                       "fresh zygote (this costs a second boot)"));
    sp.run({"docker", "exec", c, "sh", "-c", "stop; sleep 2"}, {}, log);
    sp.run({"docker", "exec", c, "sh", "-c", rezygiskArmCmd()}, {}, log);
    sp.run({"docker", "exec", c, "sh", "-c", "start"}, {}, log);
    // Wait for the framework that `start` spawns — identified by a NEW system_server, not by
    // sys.boot_completed. `stop` does not clear that property, so the obvious poll matched the
    // OUTGOING boot's value and returned instantly: the step reported success with the framework
    // still seconds from existing, and the wait leaked into connect's gates where it was harder to
    // account for. Same 60 s budget, and it now measures the thing it names.
    const QString ssPid = QStringLiteral("pgrep -f 'system_serve[r]' | head -1");
    const QString before =
        sp.run({"docker", "exec", c, "sh", "-c", ssPid}, {}, log).out.trimmed();
    for (int i = 0; i < 60; ++i) {
        const QString now =
            sp.run({"docker", "exec", c, "sh", "-c", ssPid}, {}, log).out.trimmed();
        if (!now.isEmpty() && now != before
            && sp.run({"docker", "exec", c, "getprop", "sys.boot_completed"}, {}, log)
                       .out.trimmed() == QLatin1String("1"))
            break;
        sp.waitMs(1000);
    }
    }
    // The remaining modules' post-fs-data.sh, AFTER `start` — never before. devicespoof rewrites
    // ro.hardware / ro.hardware.egl / ro.board.platform, and those SELECT THE HAL IMPLEMENTATIONS:
    // applied pre-start, SurfaceFlinger hunts a Pixel gralloc/EGL this image does not have and
    // system_server never comes up. Post-start the HALs are already resolved, so the spoof reaches
    // apps (which read ro.* at process start) without touching what init used. On the Venus stack
    // that ordering is what keeps ro.hardware.egl=angle in force for SurfaceFlinger — verified:
    // GLES stayed "ANGLE (NVIDIA … Venus)" with the spoof active.
    // PATH must carry MAGISKTMP: Shamiko's script branches on `magisk -V` being empty and
    // DISABLES ITSELF when the lookup fails (bd remora-ckl). Both candidates are listed because
    // magisk.rc moved MAGISKTMP to /debug_ramdisk (bd remora-4ei.94) while images built before
    // that still use /sbin, and this binary drives both.
    sp.run({"docker", "exec", c, "sh", "-c",
            "export PATH=/debug_ramdisk:/sbin:$PATH; for m in /data/adb/modules/*/; do "
            "case \"$m\" in */rezygisk/|*/playintegrityfix/) continue ;; esac; "
            "[ -f \"${m}disable\" ] && continue; "
            "[ -f \"${m}post-fs-data.sh\" ] && sh \"${m}post-fs-data.sh\" >/dev/null 2>&1; "
            // service.sh too, which nothing ran until now (bd remora-82c.7). Magisk normally runs
            // both; this container has no module lifecycle at all, so whatever is not invoked here
            // simply never happens. That is not a cosmetic gap: service.sh is where a module does
            // its LATE work, after boot_completed. PIF's does the whole sensitive-property pass
            // (lockstate, vbmeta.device_state, verifiedbootstate, veritymode, oem_unlock_allowed,
            // and `resetprop --compact` to hide that properties were touched at all), and Tricky
            // Store starts its keystore daemon from it — which is why that daemon "would not stay
            // up" when it was tried, a symptom then attributed to a container ptrace limit.
            //
            // Backgrounded: these scripts deliberately block on `until getprop sys.boot_completed`,
            // so running them in the foreground would hang the deploy until the timeout.
            //
            // A module that must NOT run one ships it as service.sh.disabled and is skipped by the
            // -f test — Shamiko's is exactly that, because upstream's sets ro.debuggable=0 and
            // ro.adb.secure=1 and takes adb down with it (bd remora-4ei.50).
            "[ -f \"${m}service.sh\" ] && setsid sh \"${m}service.sh\" >/dev/null 2>&1 </dev/null &"
            " done"},
           {}, log);
    // PIF explicitly, because the loop above SKIPS it — its post-fs-data.sh has to run pre-start
    // (it is what seeds the injector), so the whole module is excluded there. Its service.sh is the
    // opposite: it blocks on boot_completed and then does the sensitive-property pass that is the
    // entire point of this exercise. Skipping the module wholesale meant that half never ran.
    sp.run({"docker", "exec", c, "sh", "-c",
            "export PATH=/debug_ramdisk:/sbin:$PATH; "
            "[ -f /data/adb/modules/playintegrityfix/service.sh ] && "
            "[ ! -f /data/adb/modules/playintegrityfix/disable ] && "
            "setsid sh /data/adb/modules/playintegrityfix/service.sh >/dev/null 2>&1 </dev/null &"},
           {}, log);
    // Re-run netfix AFTER the framework restart, then VERIFY adb end-to-end before handing off.
    // `stop`/`start` restarts netd (flushing the legacy route tables netfix populated) and adbd,
    // and the exact moment everything is genuinely reachable again is not observable from any one
    // probe — the container's own `ss` misreports listeners, pgrep counts relay children as a live
    // shim, and boot_completed says nothing about adbd. What IS observable is the thing the next
    // step needs: an adb handshake through the forwarder. So insist on that here, reapplying the
    // netfix periodically while waiting, instead of letting connect burn its retries against a
    // stack that is still settling. Both fresh-container deploys failed their first
    // connect in exactly that race and succeeded on a manual retry ~25s later; this folds that
    // retry into the chain (bd remora-e5x.16).
    sp.run({"docker", "exec", c, "sh", "-c", "sh /remora/netfix.sh"}, {}, log);
    // Stamp the generation the module work just ran for, so a reconnect against this same framework
    // does not repeat it.
    sp.run({"docker", "exec", c, "sh", "-c",
            "pgrep -f 'system_serve[r]' | head -1 > /dev/.remora-spoof-gen"}, {}, log);
    // Deliberately not naming WHICH of the two got there first — the pre-zygote arm step and the
    // post-fs-data hook both aim at the same instant and either is a good outcome. What matters
    // here is only that no framework restart was needed.
    return StepResult::good(isInjected
                                ? "play spoof already active — no framework restart needed"
                                : "play spoof injected after a framework restart");
}

static QStringList groups(Spawner &sp, const LineSink &log) {
    return sp.run({"id", "-nG"}, {}, log)
        .out.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
}

// Ensure-image for the ssh backends: the tag existing on the target is NOT enough — after a
// local rebuild the target's tag still points at the OLD image and every step stays green over
// a stale deploy (bd remora-t06). Compare content via the layer DiffID list: .Id cannot be
// compared across hosts (the legacy graph driver hashes the config, the containerd image store
// reports the manifest digest — the SAME image gets different .Id on the two stores). No local
// copy of the tag → nothing to compare, presence on the target stays sufficient.
static const QLatin1String kLayersFmt("{{range .RootFS.Layers}}{{.}} {{end}}");

// The resolver hands through selectImage's "(no pre-built image for A17)" sentinel when no tag
// exists for the profile's Android version — catch it before it reaches a shell (the parens are
// a bash syntax error) and say what to do instead of a cryptic inspect failure.
static std::optional<StepResult> placeholderTagFailure(const QString &tag) {
    if (!isPlaceholderImageTag(tag)) return std::nullopt;
    return StepResult::fail(
        QStringLiteral("%1 — no image exists for this profile's Android version; build one from "
                       "source (Image panel → Build from source) or switch versions").arg(tag));
}

static StepResult ensureImageOn(Spawner &sp, const QString &host, const QString &where,
                                const QString &tag, LogSink &sink) {
    auto log = mkLog(sink, "ensure-image");
    if (auto bad = placeholderTagFailure(tag)) return *bad;
    const auto target = sp.run(
        Spawner::sshArgv(host, QStringLiteral("docker image inspect --format '%1' '%2'")
                                   .arg(kLayersFmt, tag)),
        {}, log);
    if (target.rc != 0)
        return StepResult::fail(QStringLiteral("image not found on %1: %2").arg(where, tag),
                                target.stderrTail);
    const auto local = sp.run({"docker", "image", "inspect", "--format", kLayersFmt, tag}, {}, log);
    if (local.rc == 0 && local.out.trimmed() != target.out.trimmed())
        return StepResult::fail(
            QStringLiteral("%1's %2 is a different image than the local build — rebuilt but not "
                           "pushed? docker save %2 | ssh %3 'docker load', then rerun Up. Or "
                           "promoted on %1 but not locally? docker tag %2-src %2 (Promote the "
                           "-src build), then rerun Up")
                .arg(where, tag, host));
    return StepResult::good(
        local.rc == 0
            ? QStringLiteral("image %1 on %2 matches the local build").arg(tag, where)
            : QStringLiteral("image %1 present on %2 (no local copy to compare)").arg(tag, where));
}

// Every backend's connect launches the local adb + mirror clients — catch missing tools in
// preflight, not as a confusing late failure inside the connect step.
static std::optional<StepResult> missingHostTools(Spawner &sp, const LineSink &log) {
    if (sp.run({"sh", "-c", "command -v adb >/dev/null 2>&1"}, {}, log).rc != 0)
        return StepResult::fail("adb not found on PATH — install android-tools");
    // No external-client check since the cutover: the mirror client is the remora binary itself.
    return std::nullopt;
}

// backend=vm is retired (bd remora-d5v). The profile is NOT quietly re-read as remote: vm resolved
// gpu_mode=host, no overlayfs, macvlan and container adb 5555, and remote defaults to the opposite
// of all four, so "close enough" would change where and how the container runs without saying so.
// Refuse in preflight — before anything is created — and say exactly what to edit.
static std::optional<StepResult> retiredVmBackend(const RunContext &ctx) {
    if (!ctx.retiredVmBackend) return std::nullopt;
    return StepResult::fail(retiredVmBackendMessage());
}

// Same rule for gpu_mode: a value that is neither host nor guest is refused, not defaulted.
// gpu_mode=auto used to parse into a mode nothing handled, and deployed as SwiftShader.
static std::optional<StepResult> unrecognisedGpuMode(const RunContext &ctx) {
    if (!ctx.unrecognisedGpuMode) return std::nullopt;
    return StepResult::fail(unrecognisedGpuModeMessage(*ctx.unrecognisedGpuMode));
}

// The data dir on disk must already have the shape this deploy is about to mount it with. Neither
// mismatch is destructive and neither is visible: the deploy succeeds, Android boots, and /data is
// simply blank while the real one sits on disk one level away from where anything is looking. That
// is how the A17 instance lost itself — new app uids, a fresh GSF checkin identity and
// a silently de-certified device — and it took two sessions to find, because success was reported at
// every layer. Refuse here, before the container exists, and name both ways out (bd remora-4ei.89).
//
// Shared by both backends on purpose: the shapes and the remedies are identical whether docker runs
// here or over ssh, and only the transport differs.
static std::optional<StepResult> dataShapeMismatch(Spawner &sp, const RunContext &ctx) {
    const DataDirShape shape =
        probeDataDirShape(sp, ctx.backend, ctx.rc.dataDir, ctx.guest);
    const QString refusal =
        dataShapeRefusal(ctx.rc.useOverlayfs, shape, ctx.rc.dataDir, ctx.rc.dataBaseDir);
    if (refusal.isEmpty()) return std::nullopt;
    return StepResult::fail(refusal);
}

// The image's CPU tuning against the CPU that has to run it (bd remora-bm7.13). Shared by both
// backends because the hazard is: a tuned image is DELIVERED with `docker save | ssh docker load`,
// and neither end of that has a step where anything would notice the ISA changed underneath it.
//
// In preflight rather than in ensure-image, so nothing has been created when it refuses — and so a
// missing image is still ensure-image's story to tell: the probe answers "unknown" for one, which
// passes here, and the very next step fails it by name.
static std::optional<StepResult> archVariantMismatch(Spawner &sp, const RunContext &ctx) {
    const ArchVariantProbe av =
        probeArchVariant(sp, ctx.backend, ctx.rc.imageTag, ctx.guest);
    const QString refusal = archVariantRefusal(
        av.variant, av.flagsLine, ctx.rc.imageTag,
        ctx.backend == Backend::Bare ? QStringLiteral("this host") : ctx.guest);
    if (refusal.isEmpty()) return std::nullopt;
    return StepResult::fail(refusal);
}

// ─────────────────────────────── BARE (local docker) ───────────────────────────────
class BareDeployer : public Deployer {
public:
    BareDeployer(Spawner &sp, int ba, int bi) : sp_(sp), bootAttempts_(ba), bootIntervalMs_(bi) {}
    QVector<Step> steps(const RunContext &ctx) override {
        return {
            {"preflight", "Preflight (docker group, ashmem)",
             [this, ctx](LogSink &s) { return preflight(ctx, s); }},
            {"ensure-image", "Ensure image present",
             [this, ctx](LogSink &s) { return ensureImage(ctx, s); }},
            {"host-prereqs", "Host prerequisites",
             [this, ctx](LogSink &s) { return hostPrereqs(ctx, s); }},
            {"provision", "Start / create container",
             [this, ctx](LogSink &s) { return provision(ctx, s); }},
            // Between provision and boot-wait, which is the only place it can be: the injector has
            // to exist before zygote does, and zygote arrives ~1.75s after the container starts.
            {"spoof-arm", "Arm the Play spoof (pre-zygote)",
             [this, ctx](LogSink &s) { return playSpoofArm(sp_, ctx, s); }},
            {"boot-wait", "Wait for Android boot",
             [this, ctx](LogSink &s) { return bootWait(ctx, s); }},
            // AFTER boot-wait. Moving these ahead of it looked free — neither wants a booted
            // FRAMEWORK — but both are shell scripts, and for the first second of a container's
            // life there is no shell to run them with: /system is not mounted yet, so netfix died
            // on "exec /system/bin/sh: no such file or directory" and took the whole deploy with
            // it. It only ever appeared to work because spoof-arm's polling happened to delay it
            // past the mount, which is not a dependency anybody declared (bd remora-tdz).
            {"netfix", "Wire adb (netfix + forwarder)",
             [this, ctx](LogSink &s) { return netfix(ctx, s); }},
            {"ashmem-node", "Boot-id ashmem node (A17 WebView)",
             [this, ctx](LogSink &s) { return ensureAshmemNode(sp_, ctx, s); }},
            {"camera-nodes", "Hand the camera nodes to cameraserver",
             [this, ctx](LogSink &s) { return ensureCameraNodes(sp_, ctx, s); }},
            {"audio-nodes", "Hand /dev/snd to audioserver",
             [this, ctx](LogSink &s) { return ensureAudioNodes(sp_, ctx, s); }},
            // After boot: EventHub watches /dev/input with inotify, so the decoy is picked up
            // whenever it appears — it does not have to beat Android's first scan.
            {"phantom-kbd", "Phantom keyboard (hide the on-screen IME)",
             [this, ctx](LogSink &s) { return phantomKeyboard(sp_, ctx, s); }},
            // Same inotify guarantee as the decoy: shared devices land whenever their node
            // appears, so this can run any time after the container exists.
            {"shared-inputs", "Share host input devices",
             [this, ctx](LogSink &s) { return shareInputDevices(sp_, ctx, s); }},
            {"play-spoof", "Play spoof (ReZygisk + PIF)",
             [this, ctx](LogSink &s) { return playSpoof(sp_, ctx, s); }},
            {"connect", "Connect + launch mirror",
             [this, ctx](LogSink &s) {
                 // A full deploy boots the device from scratch — the pre-chain splash showed the
                 // boot (provision appends REMORA_BOOTING when it starts one) and this ADOPTS it
                 // as the mirror. waitLit holds the launch until the screen actually composites,
                 // so the hand-off is animation → drawn home screen, never a black window.
                 // Reconnect (device already running) uses the plain doConnect: no splash there.
                 return doConnect(sp_, ctx, s, /*retries=*/15, /*retryDelayMs=*/2000,
                                  /*livenessDelayMs=*/1000, /*mirrorRetries=*/5,
                                  /*bootWaitMs=*/240000, /*waitLit=*/true,
                                  /*bootStableMs=*/4000, /*mirrorHoldMs=*/3000);
             }},
        };
    }

private:
    StepResult preflight(const RunContext &ctx, LogSink &sink) {
        auto log = mkLog(sink, "preflight");
        if (auto retired = retiredVmBackend(ctx)) return *retired;
        if (auto bad = unrecognisedGpuMode(ctx)) return *bad;
        if (auto miss = missingHostTools(sp_, log)) return *miss;
        if (!groups(sp_, log).contains("docker"))
            return StepResult::fail(
                "not in the 'docker' group — sudo usermod -aG docker $USER, then re-login");
        if (sp_.run({"sh", "-c", "lsmod | grep -q ashmem"}, {}, log).rc != 0)
            return StepResult::fail(
                "ashmem_linux.ko not loaded — bare metal needs the IBT-fixed ashmem module");
        if (auto shape = dataShapeMismatch(sp_, ctx)) return *shape;
        if (auto arch = archVariantMismatch(sp_, ctx)) return *arch;
        return StepResult::good("adb present, docker group ok, ashmem loaded, /data shape "
                                "matches, image runs on this CPU");
    }
    StepResult ensureImage(const RunContext &ctx, LogSink &sink) {
        auto log = mkLog(sink, "ensure-image");
        if (auto bad = placeholderTagFailure(ctx.rc.imageTag)) return *bad;
        auto r = sp_.run({"docker", "image", "inspect", ctx.rc.imageTag}, {}, log);
        if (r.rc != 0)
            return StepResult::fail(QStringLiteral("image not found: %1").arg(ctx.rc.imageTag),
                                    r.stderrTail);
        return StepResult::good(QStringLiteral("image %1 present").arg(ctx.rc.imageTag));
    }
    // Android's netd uses LEGACY iptables; a modern host uses the nft backend and therefore never
    // loads the legacy table modules. The container inherits the host's netfilter, so every table
    // netd tries to set up fails, and it RETRIES — measured at 4.24s of a ~13s boot, 552
    // IptablesRestoreController lines, 63 "unable to initialize table" errors across
    // filter/mangle/nat/raw on both families (bd remora-u9m):
    //
    //   host:      iptables v1.8.13 (nf_tables)     <- legacy ip[6]table_* never loaded
    //   container: iptables v1.8.11 (legacy)        <- needs exactly those
    //
    // Probed under /sys/module, which is world-readable and answers for built-in support too.
    // NOT /proc/net/ip[6]_tables_names: that looked like the kernel's own registry and reads
    // "Permission denied" for an ordinary user — swallowed by 2>/dev/null, it made every host look
    // like it was missing everything, including one that had just loaded them.
    QString netfilterNote(const LineSink &log) {
        static const char *const kMods =
            "ip_tables iptable_filter iptable_mangle iptable_nat iptable_raw "
            "ip6_tables ip6table_filter ip6table_mangle ip6table_nat ip6table_raw";
        const QString missing =
            sp_.run({"sh", "-c",
                     QStringLiteral("for m in %1; do [ -e /sys/module/$m ] || printf '%%s ' \"$m\"; "
                                    "done").arg(QLatin1String(kMods))},
                    {}, log)
                .out.trimmed();
        if (missing.isEmpty()) return QString();
        return QStringLiteral(
                   "legacy netfilter tables are missing, so Android's netd spends seconds of every "
                   "boot retrying iptables rules that cannot succeed — this host's iptables is the "
                   "nft backend and never loads them, but the container's is legacy. Measured: the "
                   "netd window was 4.24s with none loaded and 2.11s with the IPv4 half. Load the "
                   "rest once: sudo modprobe %1 — and to persist, put them one per line in "
                   "/etc/modules-load.d/remora-netfilter.conf")
            .arg(missing);
    }

    StepResult hostPrereqs(const RunContext &ctx, LogSink &sink) {
        auto log = mkLog(sink, "host-prereqs");
        sp_.run({"mkdir", "-p", ctx.rc.dataDir}, {}, log);
        // pre-create the overlayfs base too — docker would auto-create a missing bind source
        // root-owned, which blocks seeding it from an unprivileged shell later
        if (ctx.rc.useOverlayfs)
            sp_.run({"mkdir", "-p", ctx.rc.dataBaseDir}, {}, log);
        QStringList notes{QStringLiteral("data dir %1").arg(ctx.rc.dataDir)};
        const QString nf = netfilterNote(log);
        if (!nf.isEmpty()) {
            notes << QStringLiteral("SLOW BOOT: ") + nf;
            log(QStringLiteral("stderr"), nf);
        }
        // macvlan: the network must exist before the docker run names it. Created here from the
        // host's own LAN rather than demanded of the operator (bd remora-400).
        const NetworkEnsure net = ensureMacvlanNetwork(sp_, ctx.rc, QString(), log);
        if (!net.ok) return StepResult::fail(net.detail);
        if (!net.detail.isEmpty()) notes << net.detail;
        // Before the container: the docker argv already names this directory as a bind source
        // (bd remora-6279). Non-fatal, so a host it cannot be built on still deploys.
        {
            const StepResult cf = ensureCpufreqTopology(sp_, ctx.rc, QString(), log);
            if (!cf.ok)
                log(QStringLiteral("stderr"), cf.detail);
            else if (!cf.detail.isEmpty())
                notes << cf.detail;
        }
        // THE COMPOSER'S FRAME-SOCKET DIRECTORY IS A BIND SOURCE, so it must exist HERE — before
        // provision — and be owned by us (bd remora-emoe phase 2). Creating it in the encoder step
        // instead was wrong and cost a failed deploy: provision runs first, docker creates a missing
        // bind source as a ROOT-OWNED directory, and the encoder then died with
        // "remora-frame-encoder: bind: Permission denied" while every step above it reported
        // healthy. Same reason and same placement as the venus socket dir below.
        if (ctx.rc.hostEncode && !ctx.rc.hostEncodeFrameSocket.isEmpty()) {
            const QString fdir = hostEncodeFrameSocketDirFor(ctx.rc.containerName);
            // A previous deploy may have left docker's root-owned artefact; an EMPTY one is
            // unambiguously that and is removed, a non-empty one is reported rather than guessed at.
            const QFileInfo fi(fdir);
            if (fi.exists() && fi.isDir() && !fi.isWritable())
                if (const QString err = clearDockerCreatedDir(fdir); !err.isEmpty())
                    return StepResult::fail(err);
            sp_.run({"mkdir", "-p", fdir}, {}, log);
            notes << QStringLiteral("frame socket dir %1").arg(fdir);
        }
        if (ctx.rc.venus) {
            const StepResult v = venusPrereqs(ctx, sink);
            if (!v.ok) return v;
            notes << v.detail;
        } else if (ctx.rc.gpuMode == GpuMode::Guest && ctx.rc.buildPropOverlay) {
            // Guest mode needs only the /vendor/build.prop overlay from the venus prereq set —
            // no server, no guest libs — generated by the shared script (local carriage; remote
            // runs the same script over ssh). Fatal on failure: without it the baked Intel ICD
            // wins over gpu_setup_guest's setprop and SurfaceFlinger crash-loops before boot
            // ever completes (bd remora-e5x.33).
            auto log = mkLog(sink, "guest-gpu");
            const auto r = sp_.run(
                {"sh", "-c", guestBuildPropScript(*ctx.rc.buildPropOverlay, ctx.rc.imageTag)}, {},
                log);
            if (r.rc != 0)
                return StepResult::fail(
                    QStringLiteral("guest-gpu: could not generate %1 — the container cannot boot "
                                   "without it (baked Intel ICD, bd remora-e5x.33)")
                        .arg(*ctx.rc.buildPropOverlay),
                    r.stderrTail);
            notes << QStringLiteral("guest build.prop overlay (SwiftShader ICD)");
        }
        return StepResult::good(notes.join(QStringLiteral("; ")));
    }
    // NVIDIA Venus stack, brought up BEFORE the container so the bind mounts the docker argv
    // already names all resolve (bd remora-4ei.56): the socket directory, the render server
    // listening inside it, and the generated /vendor/build.prop overlay.
    StepResult venusPrereqs(const RunContext &ctx, LogSink &sink) {
        auto log = mkLog(sink, "venus");
        const ResolvedConfig &rc = ctx.rc;
        if (!rc.venusServerPath || !rc.venusLibDir || !rc.venusSocketDir || !rc.buildPropOverlay)
            return StepResult::fail(
                "venus is on but a prerequisite is missing — build the render server and install "
                "the guest libs under ~/.local/share/remora/venus (see "
                "vendor/host-prereqs/venus-nvidia/README.md), or set venus=false");
        sp_.run({"mkdir", "-p", *rc.venusSocketDir}, {}, log);
        if (const auto bad = writeVenusBuildProp(ctx, log)) return *bad;
        // The server itself is brought up by the shared helper, because the deploy chain is not the
        // only path that needs it — see ensureVenusServer().
        return ensureVenusServer(sp_, rc, log);
    }
    // The overlay = the IMAGE'S OWN /vendor/build.prop plus the Venus settings (the pure
    // transform is venusBuildPropLines, tested on its own; gpu_mode=guest has its own generator,
    // guestBuildPropScript). Generated per deploy rather than vendored: every other prop in that
    // file is image-specific, so a checked-in copy would stale-bake all of them the first time
    // the image changed.
    std::optional<StepResult> writeVenusBuildProp(const RunContext &ctx, const LineSink &log) {
        const QString dst = *ctx.rc.buildPropOverlay;
        // A NEW image tag means this file does not exist yet, and any docker run that beats this
        // write turns the missing bind-mount source into a root-owned directory the open below
        // then fails on, sticky across every retry (bd remora-gcu).
        if (const QString dirErr = clearDockerCreatedDir(dst); !dirErr.isEmpty())
            return StepResult::fail(QStringLiteral("venus: ") + dirErr);
        sp_.run({"mkdir", "-p", QFileInfo(dst).absolutePath()}, {}, log);
        const auto cid = sp_.run({"docker", "create", ctx.rc.imageTag}, {}, log);
        if (cid.rc != 0)
            return StepResult::fail("venus: docker create failed", cid.stderrTail);
        const QString id = cid.out.trimmed();
        const QString tmp = dst + QStringLiteral(".tmp");
        const auto cp =
            sp_.run({"docker", "cp", id + QStringLiteral(":/vendor/build.prop"), tmp}, {}, log);
        // The GUEST half of this stack is built by our source patches and therefore lives in the
        // IMAGE, not in venusLibDir: vulkan.virtio.so (the Venus ICD) and gralloc.minigbm_gbm_mesa
        // .so. A stock image ships the ICD but NOT the gralloc, and androidboot.remora_gralloc
        // would then name a HAL that does not exist. Verified rather than inferred from the tag,
        // which promises nothing about how the image was built. Checked inside this step because it
        // already has a container to copy out of.
        const QString probe = dst + QStringLiteral(".gralloc-probe");
        const bool hasGralloc =
            sp_.run({"docker", "cp",
                     id + QStringLiteral(":/vendor/lib64/hw/gralloc.minigbm_gbm_mesa.so"), probe},
                    {}, log)
                .rc == 0;
        QFile::remove(probe);
        sp_.run({"docker", "rm", id}, {}, log);
        if (cp.rc != 0)
            return StepResult::fail("venus: could not read /vendor/build.prop from the image",
                                    cp.stderrTail);
        if (!hasGralloc)
            return StepResult::fail(
                QStringLiteral("venus: %1 has no gralloc.minigbm_gbm_mesa.so — rebuild the image "
                               "with the minigbm_gbm_mesa_vtest source patch enabled, or set "
                               "venus=false to keep the SwiftShader/xe path")
                    .arg(ctx.rc.imageTag));
        QFile in(tmp);
        if (!in.open(QIODevice::ReadOnly))
            return StepResult::fail(QStringLiteral("venus: cannot read %1").arg(tmp));
        const QStringList lines =
            venusBuildPropLines(QString::fromUtf8(in.readAll()).split(QLatin1Char('\n')));
        in.close();
        QFile::remove(tmp);
        QFile out(dst);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
            // The errorString matters here: "cannot write" alone once cost a diagnosis session,
            // because the actual state (a directory at the path) looked like a permission error.
            return StepResult::fail(QStringLiteral("venus: cannot write %1 — %2")
                                        .arg(dst, out.errorString()));
        out.write(lines.join(QLatin1Char('\n')).toUtf8());
        out.close();
        return std::nullopt;
    }
    StepResult provision(const RunContext &ctx, LogSink &sink) {
        auto log = mkLog(sink, "provision");
        // `docker start` on an ALREADY-running container is a silent no-op — judge whether a
        // fresh Android boot starts by the pre-start state, and tell the splash (REMORA_BOOTING)
        // only then, so no animation plays over a container that never rebooted.
        const bool wasRunning =
            sp_.run({"docker", "inspect", "-f", "{{.State.Running}}", ctx.rc.containerName}, {},
                    log)
                .out.trimmed() == QLatin1String("true");
        // Starting an existing container is only correct while its IMAGE is still the one we
        // resolve to. Rebuilding a tag leaves the container pinned to the OLD image id, and a bare
        // `docker start` then silently runs it — after a multi-hour source build that reads as "the
        // rebuild did nothing", with every check reporting the previous behaviour (bd
        // remora-4ei.67). The smart-run gate upstream only inspects a RUNNING container, so a
        // merely stopped one reached here unchecked.
        const QString curImg =
            sp_.run({"docker", "inspect", "-f", "{{.Image}}", ctx.rc.containerName}, {}, log)
                .out.trimmed();
        const QString wantImg =
            sp_.run({"docker", "image", "inspect", "-f", "{{.Id}}", ctx.rc.imageTag}, {}, log)
                .out.trimmed();
        // Both must be known before calling it stale: a missing container or an unbuilt tag give
        // empty strings, and treating those as a mismatch would recreate on every deploy.
        const bool staleImage = !curImg.isEmpty() && !wantImg.isEmpty() && curImg != wantImg;
        // Networking is fixed at CREATE time, so the same rule applies to it: a container made on
        // the bridge keeps the bridge however the profile is now configured (bd remora-400).
        const bool staleNet = containerNetworkStale(
            sp_.run({"docker", "inspect", "-f", QString::fromUtf8(kNetworkInspectFormat),
                     ctx.rc.containerName},
                    {}, log)
                .out,
            ctx.rc);
        // Devices are fixed at CREATE time too, and the camera is the one that DRIFTS: a
        // container created while the camera was absent carries only its GPU nodes, so a plain
        // `docker start` runs it with the resolver's /dev/video* never passed through and every
        // check green (bd remora-usn0). Gated on curImg like the image rule: a missing container
        // inspects to an empty device list, and that must not read as stale — it is about to be
        // created with the right devices anyway.
        const bool staleDevs =
            !curImg.isEmpty() &&
            containerDevicesStale(
                sp_.run({"docker", "inspect", "-f",
                         "{{range .HostConfig.Devices}}{{println .PathOnHost}}{{end}}",
                         ctx.rc.containerName},
                        {}, log)
                    .out.simplified()
                    .split(QLatin1Char(' '), Qt::SkipEmptyParts),
                ctx.rc);
        if (staleImage || staleNet || staleDevs) {
            log(QStringLiteral("stdout"),
                staleImage
                    ? QStringLiteral("%1 was created from a superseded %2 — recreating")
                          .arg(ctx.rc.containerName, ctx.rc.imageTag)
                : staleNet
                    ? QStringLiteral("%1 was created with different networking than %2 mode now "
                                     "asks for — recreating")
                          .arg(ctx.rc.containerName, ctx.rc.networkMode)
                    : QStringLiteral("%1 was created with different device passthrough than the "
                                     "profile now resolves (camera replugged or removed?) — "
                                     "recreating")
                          .arg(ctx.rc.containerName));
            sp_.run({"docker", "rm", "-f", ctx.rc.containerName}, {}, log);
        } else if (sp_.run({"docker", "start", ctx.rc.containerName}, {}, log).rc == 0) {
            if (!wasRunning) appendStatusLine(ctx.statusPath, QStringLiteral("REMORA_BOOTING"));
            return StepResult::good(QStringLiteral("started existing %1").arg(ctx.rc.containerName));
        }
        auto r = sp_.run(ctx.dockerRunArgv, {}, log);
        if (r.rc != 0) return StepResult::fail("docker run failed", r.stderrTail);
        appendStatusLine(ctx.statusPath, QStringLiteral("REMORA_BOOTING"));
        return StepResult::good(QStringLiteral("created %1").arg(ctx.rc.containerName));
    }
    StepResult bootWait(const RunContext &ctx, LogSink &sink) {
        auto log = mkLog(sink, "boot-wait");
        for (int i = 0; i < bootAttempts_; ++i) {
            auto r = sp_.run({"docker", "exec", ctx.rc.containerName, "getprop", "sys.boot_completed"},
                             {}, log);
            if (parseBootCompleted(r.out) == std::optional<bool>(true))
                return StepResult::good(QStringLiteral("booted (after %1 polls)").arg(i + 1));
            sp_.waitMs(bootIntervalMs_);
        }
        return StepResult::fail(QStringLiteral("boot did not complete after %1 polls").arg(bootAttempts_));
    }
    StepResult netfix(const RunContext &ctx, LogSink &sink) {
        auto log = mkLog(sink, "netfix");
        const QString c = ctx.rc.containerName;
        // The pair arrives as read-only bind mounts from buildDockerRunArgv — NOT copied here.
        // `docker cp` into a booted container cannot work: init remounts the rootfs read-only and
        // cp must create a .pivot_root dir at the rootfs root, so it fails for every destination
        // path (bd remora-4ei.59). No chmod either: `fwd` is 755 on the host and the script is run
        // as `sh <path>`, so the read-only mounts are already sufficient.
        //
        // /remora, not /data/local/tmp: under use_overlayfs the init-time overlay on /data shadows
        // every sub-bind beneath it, which used to make adb unwireable on bare (bd remora-4u4.3).
        // Mounting outside /data behaves the same either way, so there is no overlayfs branch here.
        auto r = sp_.run({"docker", "exec", c, "sh", "-c", "sh /remora/netfix.sh"}, {}, log);
        if (r.rc != 0) return StepResult::fail("netfix failed", r.stderrTail);
        return StepResult::good("rp_filter + routes set, adb forwarder up");
    }
    Spawner &sp_;
    int bootAttempts_, bootIntervalMs_;
};

// ─────────────────────────────── REMOTE (generic ssh docker) ───────────────────────────────
class RemoteDeployer : public Deployer {
public:
    RemoteDeployer(Spawner &sp, int ba, int bi) : sp_(sp), bootAttempts_(ba), bootIntervalMs_(bi) {}
    QVector<Step> steps(const RunContext &ctx) override {
        return {
            {"preflight", "Preflight (remote ssh + docker)",
             [this, ctx](LogSink &s) { return preflight(ctx, s); }},
            {"ensure-image", "Ensure image present (remote)",
             [this, ctx](LogSink &s) { return ensureImage(ctx, s); }},
            {"host-prereqs", "Push helpers to remote",
             [this, ctx](LogSink &s) { return hostPrereqs(ctx, s); }},
            {"provision", "Start / create container (remote)",
             [this, ctx](LogSink &s) { return provision(ctx, s); }},
            // Between provision and boot-wait, which is the only place it can be: the injector has
            // to exist before zygote does, and zygote arrives ~1.75s after the container starts.
            {"spoof-arm", "Arm the Play spoof (pre-zygote)",
             [this, ctx](LogSink &s) { return playSpoofArm(sp_, ctx, s); }},
            {"boot-wait", "Wait for Android boot",
             [this, ctx](LogSink &s) { return bootWait(ctx, s); }},
            {"netfix", "Wire adb (netfix + forwarder)",
             [this, ctx](LogSink &s) { return netfix(ctx, s); }},
            {"ashmem-node", "Boot-id ashmem node (A17 WebView)",
             [this, ctx](LogSink &s) { return ensureAshmemNode(sp_, ctx, s); }},
            {"camera-nodes", "Hand the camera nodes to cameraserver",
             [this, ctx](LogSink &s) { return ensureCameraNodes(sp_, ctx, s); }},
            {"audio-nodes", "Hand /dev/snd to audioserver",
             [this, ctx](LogSink &s) { return ensureAudioNodes(sp_, ctx, s); }},
            // After boot: EventHub watches /dev/input with inotify, so the decoy is picked up
            // whenever it appears — it does not have to beat Android's first scan.
            {"phantom-kbd", "Phantom keyboard (hide the on-screen IME)",
             [this, ctx](LogSink &s) { return phantomKeyboard(sp_, ctx, s); }},
            // Same inotify guarantee as the decoy: shared devices land whenever their node
            // appears, so this can run any time after the container exists.
            {"shared-inputs", "Share host input devices",
             [this, ctx](LogSink &s) { return shareInputDevices(sp_, ctx, s); }},
            {"play-spoof", "Play spoof (ReZygisk + PIF)",
             [this, ctx](LogSink &s) { return playSpoof(sp_, ctx, s); }},
            {"connect", "Connect + launch mirror",
             [this, ctx](LogSink &s) {
                 // A full deploy boots the device from scratch — the pre-chain splash showed the
                 // boot (provision appends REMORA_BOOTING when it starts one) and this ADOPTS it
                 // as the mirror. waitLit holds the launch until the screen actually composites,
                 // so the hand-off is animation → drawn home screen, never a black window.
                 // Reconnect (device already running) uses the plain doConnect: no splash there.
                 return doConnect(sp_, ctx, s, /*retries=*/15, /*retryDelayMs=*/2000,
                                  /*livenessDelayMs=*/1000, /*mirrorRetries=*/5,
                                  /*bootWaitMs=*/240000, /*waitLit=*/true,
                                  /*bootStableMs=*/4000, /*mirrorHoldMs=*/3000);
             }},
        };
    }

private:
    StepResult preflight(const RunContext &ctx, LogSink &sink) {
        auto log = mkLog(sink, "preflight");
        if (auto retired = retiredVmBackend(ctx)) return *retired;
        if (auto bad = unrecognisedGpuMode(ctx)) return *bad;
        // No baked ssh host (bd remora-4ei.12) — name the missing field instead of dialling an
        // empty target and reporting it as unreachable.
        if (ctx.guest.isEmpty())
            return StepResult::fail("no ssh host set for this instance — set it on the Device "
                                    "page (Deploy target > SSH host), or ssh_host=user@host in "
                                    "remorarc");
        // Config shape, so it needs no network and fails before anything is dialled: a "local:"
        // companion app has no staging path to a remote docker host (bd remora-pzq).
        if (const QString apps = companionAppsRemoteRefusal(ctx.rc.companionApps); !apps.isEmpty())
            return StepResult::fail(apps);
        if (auto miss = missingHostTools(sp_, log)) return *miss;
        if (sp_.run(Spawner::sshArgv(ctx.guest, "echo ok"), {}, log).out.trimmed() !=
            QLatin1String("ok"))
            return StepResult::fail(QStringLiteral("remote %1 not reachable over ssh").arg(ctx.guest));
        if (sp_.run(Spawner::sshArgv(ctx.guest, "docker info >/dev/null 2>&1 && echo ok"), {}, log)
                .out.trimmed() != QLatin1String("ok"))
            return StepResult::fail("docker not usable on the remote (group/daemon?)");
        // After the reachability checks, never before: the shape probe reads the REMOTE filesystem,
        // and an unreachable host answering nothing must not be mistaken for a shape.
        if (auto shape = dataShapeMismatch(sp_, ctx)) return *shape;
        // Also after reachability, and for the same reason as the shape probe: both the image and
        // the cpuinfo live on the far side, so an unreachable host answers nothing to each — which
        // reads as "unknown" and passes, exactly as it should, but only once the ssh failure has
        // already been reported as an ssh failure.
        if (auto arch = archVariantMismatch(sp_, ctx)) return *arch;
        return StepResult::good(
            QStringLiteral("%1 reachable, docker ok, /data shape matches, image runs on its CPU")
                .arg(ctx.guest));
    }
    StepResult ensureImage(const RunContext &ctx, LogSink &sink) {
        return ensureImageOn(sp_, ctx.guest, QStringLiteral("remote"), ctx.rc.imageTag, sink);
    }
    StepResult hostPrereqs(const RunContext &ctx, LogSink &sink) {
        auto log = mkLog(sink, "host-prereqs");
        sp_.run(Spawner::sshArgv(ctx.guest, "mkdir -p ~/.remora"), {}, log);
        // CHECKED pushes, because these become the /remora bind SOURCES at docker run
        // (bd remora-4ei.88) — docker answers a missing source by creating an empty directory
        // there, so a failed scp must stop the deploy HERE, where the message names the push,
        // not later as an inexplicably empty /remora inside the container. scp carries the mode,
        // so fwd stays executable on the other side.
        const auto push = [&](const QString &local, const QString &name) {
            return sp_.run({"scp", local, ctx.guest + QStringLiteral(":.remora/") + name}, {}, log)
                       .rc == 0;
        };
        if (!push(vendorNativeBinary("fwd"), QStringLiteral("fwd")))
            return StepResult::fail("could not push fwd to the remote's ~/.remora");
        if (!push(vendorScript("container-scripts", "remora-netfix.sh"),
                  QStringLiteral("remora-netfix.sh")))
            return StepResult::fail("could not push remora-netfix.sh to the remote's ~/.remora");
        // The forwarder's init service travels with the pair it supervises. Unconditional like
        // them, not gated like uinput-kbd: it is a committed source file, not a built artefact,
        // so it is always there to push (bd remora-yn4).
        if (!push(vendorScript("container-scripts", "remora-fwd.rc"),
                  QStringLiteral("remora-fwd.rc")))
            return StepResult::fail("could not push remora-fwd.rc to the remote's ~/.remora");
        // The androidboot.remora_* namespace adapter, unconditional for the same committed-file
        // reason — and load-bearing at boot: without it a remote container gets the remora_* argv
        // and an empty directory mounted at its rc path (bd remora-28ix.4).
        if (!push(vendorScript("container-scripts", "remora.props.rc"),
                  QStringLiteral("remora.props.rc")))
            return StepResult::fail("could not push remora.props.rc to the remote's ~/.remora");
        // Staged only when built locally — the same existence gate applyVendorPaths uses for the
        // bind, so the two can never disagree about whether /remora/uinput-kbd exists.
        if (QFileInfo::exists(vendorNativeBinary("uinput-kbd"))
            && !push(vendorNativeBinary("uinput-kbd"), QStringLiteral("uinput-kbd")))
            return StepResult::fail("could not push uinput-kbd to the remote's ~/.remora");
        // Unconditional like remora-netfix.sh: a committed source file, not a built artefact.
        // shareInputDevices runs it from ~/.remora on the remote (the docker host owns the
        // devices being shared, so the script must run there, not here).
        if (!push(vendorScript("host-scripts", "remora-input-share.py"),
                  QStringLiteral("remora-input-share.py")))
            return StepResult::fail("could not push remora-input-share.py to the remote's ~/.remora");
        // The mirror agent, on the same built-locally gate. Its rc rides with the dex rather than
        // unconditionally: an rc without a dex is a service init retries forever (bd
        // remora-28ix.3).
        if (QFileInfo::exists(agentDex())) {
            if (!push(agentDex(), QStringLiteral("agent.dex")))
                return StepResult::fail("could not push agent.dex to the remote's ~/.remora");
            if (!push(vendorScript("container-scripts", "remora-agent.rc"),
                      QStringLiteral("remora-agent.rc")))
                return StepResult::fail("could not push remora-agent.rc to the remote's ~/.remora");
        }
        // Same macvlan rule as bare, against the remote docker host's own LAN — the subnet that
        // matters is the one the CONTAINER will be on, never this machine's (bd remora-400).
        const NetworkEnsure net = ensureMacvlanNetwork(sp_, ctx.rc, ctx.guest, log);
        if (!net.ok) return StepResult::fail(net.detail);
        // On the REMOTE docker host, where the bind source has to exist (bd remora-6279). Same
        // non-fatal contract as bare.
        QStringList notes;
        if (!net.detail.isEmpty()) notes << net.detail;
        {
            const StepResult cf = ensureCpufreqTopology(sp_, ctx.rc, ctx.guest, log);
            if (!cf.ok)
                log(QStringLiteral("stderr"), cf.detail);
            else if (!cf.detail.isEmpty())
                notes << cf.detail;
        }
        // gpu_mode=guest — the REMOTE DEFAULT — needs the /vendor/build.prop overlay on the
        // machine running docker, generated there by one script (docker cp the image's own copy
        // out, strip the Remora-owned GPU keys, append the SwiftShader ICD — the same transform
        // buildPropOverlayLines applies for bare). FATAL on failure, unlike cpufreq: without it
        // the baked Intel ICD wins over gpu_setup_guest's setprop and SurfaceFlinger crash-loops
        // before boot ever completes (bd remora-e5x.33). The rmdir first heals docker's
        // missing-bind-source directory artefact, remote twin of clearDockerCreatedDir.
        if (ctx.rc.gpuMode == GpuMode::Guest && ctx.rc.buildPropOverlay) {
            const auto r = sp_.run(
                Spawner::sshArgv(ctx.guest, guestBuildPropScript(*ctx.rc.buildPropOverlay,
                                                                 ctx.rc.imageTag)),
                {}, log);
            if (r.rc != 0)
                return StepResult::fail(
                    QStringLiteral("guest-gpu: could not generate %1 on %2 — the container "
                                   "cannot boot without it (baked Intel ICD, bd remora-e5x.33)")
                        .arg(*ctx.rc.buildPropOverlay, ctx.guest),
                    r.stderrTail);
            notes << QStringLiteral("guest build.prop overlay (SwiftShader ICD)");
        }
        return StepResult::good(notes.isEmpty()
                                    ? QStringLiteral("helpers pushed to remote ~/.remora")
                                    : QStringLiteral("helpers pushed to remote ~/.remora; %1")
                                          .arg(notes.join(QStringLiteral("; "))));
    }
    StepResult provision(const RunContext &ctx, LogSink &sink) {
        auto log = mkLog(sink, "provision");
        const QString c = ctx.rc.containerName;
        // Same fresh-boot judgement as bare: only a not-running container boots on `docker start`.
        const bool wasRunning =
            sp_.run(Spawner::sshArgv(ctx.guest,
                                     "docker inspect -f '{{.State.Running}}' " + c),
                    {}, log)
                .out.trimmed() == QLatin1String("true");
        // Networking is fixed at create time here too — see the bare provision (bd remora-400).
        if (containerNetworkStale(
                sp_.run(Spawner::sshArgv(ctx.guest,
                                         QStringLiteral("docker inspect -f '%1' %2")
                                             .arg(QString::fromUtf8(kNetworkInspectFormat), c)),
                        {}, log)
                    .out,
                ctx.rc)) {
            log(QStringLiteral("stdout"),
                QStringLiteral("%1 was created with different networking than %2 mode now asks "
                               "for — recreating")
                    .arg(c, ctx.rc.networkMode));
            sp_.run(Spawner::sshArgv(ctx.guest, "docker rm -f " + c), {}, log);
        } else if (sp_.run(Spawner::sshArgv(ctx.guest, "docker start " + c), {}, log).rc == 0) {
            if (!wasRunning) appendStatusLine(ctx.statusPath, QStringLiteral("REMORA_BOOTING"));
            return StepResult::good(QStringLiteral("started existing %1").arg(c));
        }
        auto r = sp_.run(Spawner::sshArgv(ctx.guest, ctx.dockerRunArgv.join(QLatin1Char(' '))), {},
                         log);
        if (r.rc != 0) return StepResult::fail("remote docker run failed", r.stderrTail);
        appendStatusLine(ctx.statusPath, QStringLiteral("REMORA_BOOTING"));
        return StepResult::good(QStringLiteral("created %1 on remote").arg(c));
    }
    StepResult bootWait(const RunContext &ctx, LogSink &sink) {
        auto log = mkLog(sink, "boot-wait");
        const QString c = ctx.rc.containerName;
        for (int i = 0; i < bootAttempts_; ++i) {
            auto r = sp_.run(Spawner::sshArgv(ctx.guest,
                                              "docker exec " + c + " getprop sys.boot_completed"),
                             {}, log);
            if (parseBootCompleted(r.out) == std::optional<bool>(true))
                return StepResult::good(QStringLiteral("booted (after %1 polls)").arg(i + 1));
            sp_.waitMs(bootIntervalMs_);
        }
        return StepResult::fail(QStringLiteral("boot did not complete after %1 polls").arg(bootAttempts_));
    }
    StepResult netfix(const RunContext &ctx, LogSink &sink) {
        auto log = mkLog(sink, "netfix");
        const QString c = ctx.rc.containerName;
        // Same mechanism as bare, over ssh: the pair arrives as read-only /remora bind mounts of
        // the remote's staged ~/.remora copies (host-prereqs pushed them, applyVendorPaths named
        // them in the run argv). The old post-boot `docker cp` into /data/local/tmp cannot work —
        // init remounts the rootfs read-only and cp fails recreating the mount points
        // (bd remora-4ei.59, reproduced; bd remora-4ei.88) — and under use_overlayfs the /data
        // overlay would shadow the copies anyway (bd remora-4u4.3).
        auto r = sp_.run(Spawner::sshArgv(ctx.guest, "docker exec " + c +
                                                         " sh -c 'sh /remora/netfix.sh'"),
                         {}, log);
        if (r.rc != 0)
            return StepResult::fail(
                "remote netfix failed — if /remora/netfix.sh is missing or an empty directory, "
                "the container predates the /remora binds: remove it (docker rm -f " + c +
                    " on the remote) and redeploy",
                r.stderrTail);
        return StepResult::good("rp_filter + routes set, adb forwarder up");
    }
    Spawner &sp_;
    int bootAttempts_, bootIntervalMs_;
};

std::unique_ptr<Deployer> makeDeployer(Backend backend, Spawner &sp, int bootAttempts,
                                       int bootIntervalMs) {
    switch (backend) {
        case Backend::Bare: return std::make_unique<BareDeployer>(sp, bootAttempts, bootIntervalMs);
        case Backend::Remote:
            return std::make_unique<RemoteDeployer>(sp, bootAttempts, bootIntervalMs);
    }
    return std::make_unique<BareDeployer>(sp, bootAttempts, bootIntervalMs);
}

}  // namespace remora
