#include "engine/Engine.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTextStream>

#include "core/Builders.h"

namespace remora {

namespace {

// Mirrors chain progress into the splash status file so the boot-splash window — which opens the
// moment the deploy starts — always shows what is happening: each step's human title while it
// runs, and the failure text if one fails. Forwards everything to the real sink untouched.
class StatusSink : public LogSink {
public:
    StatusSink(LogSink &inner, QString statusPath, QHash<QString, QString> titles)
        : inner_(inner), statusPath_(std::move(statusPath)), titles_(std::move(titles)) {}
    void line(const QString &step, const QString &stream, const QString &text) override {
        inner_.line(step, stream, text);
    }
    void state(const QString &step, StepState st, const QString &detail,
               const QString &stderrTail) override {
        if (st == StepState::Running)
            appendStatusLine(statusPath_, titles_.value(step, step) + QStringLiteral("..."));
        else if (st == StepState::Failed)
            appendStatusLine(statusPath_,
                             QStringLiteral("failed: %1").arg(detail.isEmpty() ? step : detail));
        inner_.state(step, st, detail, stderrTail);
    }

private:
    LogSink &inner_;
    QString statusPath_;
    QHash<QString, QString> titles_;
};

// Wayland clients cannot position their own windows — the compositor places them by its own
// policy (KWin's put this splash at 484,275, not centered). And when the user configured a
// window size, they mean the WHOLE visible window: under server-side decorations the client
// cannot even measure the frame, so only the compositor can size it exactly. Best-effort: a
// detached one-shot helper loads a transient KWin script that sets the splash window's FRAME to
// the configured size (when set) and centers it within its screen's placement area (retrying a
// few seconds so it lands after the mirror's own client-size enforcement settles — the adopting
// screen then keeps this geometry). Silently a no-op on non-KDE hosts (no qdbus6/KWin).
void centerSplashWindow(Spawner &sp, const RunContext &ctx) {
    if (ctx.windowTitle.isEmpty()) return;
    const QString jsPath = ctx.statusPath + QStringLiteral(".center.js");
    QFile js(jsPath);
    if (!js.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
    QString title = ctx.windowTitle;
    title.replace(QLatin1Char('\\'), QLatin1String("\\\\"))
        .replace(QLatin1Char('"'), QLatin1String("\\\""));
    const bool haveSize = ctx.rc.windowWidth && ctx.rc.windowHeight;
    // Act only while the geometry is WRONG: every frameGeometry write triggers a compositor
    // configure (visible jitter when repeated), and once the size is right the helper must be a
    // true no-op — both to keep the window rock-still and to leave a user who moves it alone.
    // With a configured size, size correctness is the trigger (a later manual move is respected);
    // without one there is nothing to size, so an off-center position is the trigger instead.
    js.write(QStringLiteral(
                 "var t=\"%1\";var l=workspace.windowList();"
                 "for(var i=0;i<l.length;i++){var w=l[i];"
                 "if(w.caption.indexOf(t)===0){"
                 "var g=w.frameGeometry;"
                 "var W=%2>0?%2:g.width;"
                 "var H=%3>0?%3:g.height;"
                 "var a=workspace.clientArea(KWin.PlacementArea,w);"
                 "var cx=a.x+Math.round((a.width-W)/2);"
                 "var cy=a.y+Math.round((a.height-H)/2);"
                 "var sizeOff=g.width!==W||g.height!==H;"
                 "var posOff=Math.abs(g.x-cx)>1||Math.abs(g.y-cy)>1;"
                 "if(sizeOff||(%2===0&&posOff)){"
                 "w.frameGeometry={x:cx,y:cy,width:W,height:H};}}}")
                 .arg(title)
                 .arg(haveSize ? *ctx.rc.windowWidth : 0)
                 .arg(haveSize ? *ctx.rc.windowHeight : 0)
                 .toUtf8());
    js.close();
    // Tight cadence: the splash window maps within ~0.5-1s of the spawn, and every tick before
    // the geometry lands is a visible black window at the compositor's placement — apply
    // immediately and re-apply every 250ms so the correction hits within a frame or two of the
    // map (it is idempotent once the geometry matches), with a short slow tail for a late map.
    const QString loop = QStringLiteral(
        "command -v qdbus6 >/dev/null 2>&1 || exit 0; "
        "for i in $(seq 1 22); do "
        "N=$(qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.loadScript '%1' "
        "remora-center 2>/dev/null) || exit 0; "
        "qdbus6 org.kde.KWin /Scripting/Script$N org.kde.kwin.Script.run >/dev/null 2>&1; "
        "qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.unloadScript remora-center "
        ">/dev/null 2>&1; "
        "if [ $i -lt 16 ]; then sleep 0.25; else sleep 1; fi; "
        "done; rm -f '%1'")
                             .arg(jsPath);
    sp.spawnDetached({QStringLiteral("sh"), QStringLiteral("-c"), loop}, {});
}

// Launch the one-window boot splash: the mirror itself, opened immediately with the
// --boot-animation flags, so the user sees a window (status → boot animation → live mirror) from
// the second they click instead of wondering what is happening. Deploy-only, and only for pinned
// targets — an auto-IP target isn't known until connect, and the splash needs the serial from t0.
// Stale mirrors AND stale guest-side servers are reaped BEFORE the splash exists — this is the
// last moment when everything matching is by definition stale. doConnect skips both reaps once a
// splash exists, because by then the only thing they can match is that splash (bd remora-eoy.10).
void startSplash(Spawner &sp, RunContext &ctx) {
    if (ctx.fullscreenDisplay || ctx.rc.target.isEmpty() || ctx.splashArgv.isEmpty()) return;
    sp.run({"pkill", "-f", mirrorTitlePattern(ctx.rc.target, ctx.windowTitle)}, {}, {});
    // The guest-side reap that stood here went with killStaleServer (bd remora-28ix.4
    // step 4): the agent is one long-running init service rather than a server spawned per
    // session, so there is no stale guest process left to reap and no VF encoder context to leak.
    // The host-side pkill above stays — that one is about OUR window, not the device's.
    QDir().mkpath(QFileInfo(ctx.statusPath).dir().path());
    QFile f(ctx.statusPath);  // fresh run, fresh status — stale markers must not replay
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) f.write("starting...\n");
    QStringList argv = ctx.splashArgv;
    // When the KWin helper will manage the geometry (frame-exact sizing incl. decorations, which
    // the client cannot compute), the splash must not run its own client-size enforcement: two
    // authorities racing over the window (client target = frame + decorations vs helper target =
    // frame) made the final size NONDETERMINISTIC — observed settling on either, run to run.
    const bool helperSizes =
        ctx.rc.windowWidth && ctx.rc.windowHeight
        && !QStandardPaths::findExecutable(QStringLiteral("qdbus6")).isEmpty();
    if (helperSizes) argv << QStringLiteral("--boot-animation-external-geometry");
    ctx.splash = std::make_shared<Detached>(
        sp.spawnDetached(argv, mirrorSpawnEnv(ctx, ctx.rc.target, ctx.mirrorArgv)));
    centerSplashWindow(sp, ctx);
}

// Step titles keyed by step name, for StatusSink's Running lines.
QHash<QString, QString> stepTitles(const QVector<Step> &steps) {
    QHash<QString, QString> titles;
    for (const Step &s : steps) titles.insert(s.name, s.title);
    return titles;
}

}  // namespace

QString mirrorTitlePattern(const QString &target, const QString &windowTitle) {
    QString inst = windowTitle;
    if (inst.startsWith(QLatin1String("Remora:"))) inst = inst.mid(7);
    // No usable title → match only the legacy prefix: a bare "--window-title" pattern would
    // reap app/desktop windows too.
    if (inst.isEmpty())
        return QStringLiteral("remora mirror.*-s %1.*--window-title Remora:").arg(target);
    return QStringLiteral("remora mirror.*-s %1.*--window-title (Remora:)?%2")
        .arg(target, QRegularExpression::escape(inst));
}

bool connectRun(Backend backend, const RemoraConfig &cfg, LogSink &sink, Spawner &sp,
                const std::optional<HostCapabilities> &caps, const QString &windowTitle,
                const ConflictFn &onConflict, bool fullscreenDisplay) {
    RunContext ctx = makeContext(cfg, backend, caps, windowTitle, fullscreenDisplay);
    ExclusionService excl(sp, ctx.guest);
    const bool includeGuest = (backend == Backend::Remote);
    const ClusterState state = excl.probe(includeGuest);
    if (const auto conflict = conflictFor(state, backend, ctx.rc)) {
        const bool consent = onConflict ? onConflict(*conflict) : false;
        if (!consent) return false;
        // Stop the actual HOLDER, not a hardcoded name — with per-profile containers the two are
        // only the same for the historical default profile (bd remora-4u4.1).
        excl.stop(backend, conflict->holder);
    }
    auto dep = makeDeployer(backend, sp);
    startSplash(sp, ctx);  // after conflict consent — a declined takeover must not flash a window
    const QVector<Step> steps = dep->steps(ctx);
    StatusSink ssink(sink, ctx.splash ? ctx.statusPath : QString(), stepTitles(steps));
    const bool ok = runChain(steps, ssink);
    // A failed chain leaves the splash showing "failed: …" (status phase) or animating (a failure
    // after the boot marker) forever — close it. The marker covers the status phase; the kill
    // covers the animation phase, where the file is no longer read. On success the splash has
    // already become the mirror and neither can touch it (doConnect adopted its pid).
    if (!ok && ctx.splash) {
        appendStatusLine(ctx.statusPath, QStringLiteral("REMORA_ABORT"));
        if (sp.detachedAlive(*ctx.splash))
            sp.run({"kill", QString::number(ctx.splash->pid)}, {}, {});
    }
    return ok;
}

bool reconnectRun(Backend backend, const RemoraConfig &cfg, LogSink &sink, Spawner &sp,
                  const std::optional<HostCapabilities> &caps, const QString &windowTitle,
                  bool fullscreenDisplay) {
    RunContext ctx = makeContext(cfg, backend, caps, windowTitle, fullscreenDisplay);
    // Before the mirror, not after: the helper is what makes Android report a hardware keyboard,
    // and a reconnect is exactly the case where the container is up but its helper is not (it died,
    // or the container predates the feature). Idempotent, so the usual warm reconnect pays one
    // `pidof` for it and moves on.
    QVector<Step> steps{{"phantom-kbd", "Phantom keyboard (hide the on-screen IME)",
                         [&sp, ctx](LogSink &s) { return phantomKeyboard(sp, ctx, s); }},
                        // A reconnect is exactly when a share was just ticked in the GUI: the
                        // container is up, so no deploy chain will ever run for it. Also the
                        // wake-from-pause path — a paused container fails the watcher's docker
                        // execs, so the watcher may have exited and needs restarting (found by
                        // the first real `up` over a running container, bd remora-4ei.36).
                        {"shared-inputs", "Share host input devices",
                         [&sp, ctx](LogSink &s) { return shareInputDevices(sp, ctx, s); }},
                        {"connect", "Connect + launch mirror",
                         [&sp, ctx](LogSink &s) { return doConnect(sp, ctx, s); }}};
    return runChain(steps, sink);
}

// Restart the running Android container (fresh boot) and SEE it boot: the one-window boot splash
// launches first (status text), the restart step appends REMORA_BOOTING so the splash plays the
// device's bootanimation.zip host-side — the SR-IOV VF can't render it on-device — and once the
// screen composites the same window becomes the mirror (doConnect adopts the splash).
bool watchBootRun(Backend backend, const RemoraConfig &cfg, LogSink &sink, Spawner &sp,
                  const std::optional<HostCapabilities> &caps, const QString &windowTitle) {
    RunContext ctx = makeContext(cfg, backend, caps, windowTitle);
    startSplash(sp, ctx);
    const auto restart = [&sp, ctx, backend](LogSink &s) -> StepResult {
        const LineSink log = [&s](const QString &stream, const QString &text) {
            s.line(QStringLiteral("restart"), stream, text);
        };
        const QString c = ctx.rc.containerName;
        const ProcResult r =
            backend == Backend::Bare
                ? sp.run({"docker", "restart", "-t", "3", c}, {}, log)
                : sp.run(Spawner::sshArgv(ctx.guest,
                                          QStringLiteral("docker restart -t 3 %1").arg(c)),
                         {}, log);
        if (r.out.trimmed() != c && r.rc != 0)
            return StepResult::fail(
                QStringLiteral("could not restart container %1 — is it deployed? (run Connect/Up "
                               "first)").arg(c),
                r.stderrTail);
        if (ctx.splash) appendStatusLine(ctx.statusPath, QStringLiteral("REMORA_BOOTING"));
        return StepResult::good(QStringLiteral("restarted %1 — booting").arg(c));
    };
    QVector<Step> steps{
        {"restart", "Restart device (fresh boot)", restart},
        {"connect", "Attach mirror during boot", [&sp, ctx](LogSink &s) {
             // watch-boot restarts the device and adopts the boot-anim splash exactly like a deploy,
             // so it needs the same two safeguards a deploy passes: bootStableMs (survive a fresh
             // first-boot reboot) and mirrorHoldMs (relaunch a plain mirror if the adopted splash
             // drops at the hand-off — the "watch the boot, mirror closes, reconnect holds" crash).
             return doConnect(sp, ctx, s, /*retries=*/30, /*retryDelayMs=*/2000,
                              /*livenessDelayMs=*/1000, /*mirrorRetries=*/8, /*bootWaitMs=*/240000,
                              /*waitLit=*/true, /*bootStableMs=*/15000,
                              /*mirrorHoldMs=*/12000);
         }},
        // AFTER connect, not straight after the restart: the restart killed the helper, but for the
        // first seconds of a container's life /system is not mounted, so there is no `sh` to run the
        // idempotency check with and (on remote) no `chmod` to stage the binary — the same trap that
        // moved netfix behind boot-wait. doConnect already waits for boot and for the screen to
        // composite, so by here Android is up. EventHub watches /dev/input with inotify, so a
        // keyboard that appears now is picked up just the same as one that was there all along.
        {"phantom-kbd", "Phantom keyboard (hide the on-screen IME)",
         [&sp, ctx](LogSink &s) { return phantomKeyboard(sp, ctx, s); }},
        // The restart rebuilt the container's /dev tmpfs, taking every exposed node with it, and
        // the watcher may have died while the container was unresponsive (two failed polls is its
        // exit condition) — re-expose and respawn, same as a deploy.
        {"shared-inputs", "Share host input devices",
         [&sp, ctx](LogSink &s) { return shareInputDevices(sp, ctx, s); }}};
    StatusSink ssink(sink, ctx.splash ? ctx.statusPath : QString(), stepTitles(steps));
    const bool ok = runChain(steps, ssink);
    if (!ok && ctx.splash) {
        appendStatusLine(ctx.statusPath, QStringLiteral("REMORA_ABORT"));
        if (sp.detachedAlive(*ctx.splash))
            sp.run({"kill", QString::number(ctx.splash->pid)}, {}, {});
    }
    return ok;
}

bool sleepRun(Backend backend, const RemoraConfig &cfg, LogSink &sink, Spawner &sp,
              const std::optional<HostCapabilities> &caps, const QString &windowTitle) {
    RunContext ctx = makeContext(cfg, backend, caps, windowTitle);
    const auto freeze = [&sp, ctx, backend](LogSink &s) -> StepResult {
        const LineSink log = [&s](const QString &stream, const QString &text) {
            s.line(QStringLiteral("sleep"), stream, text);
        };
        const QString c = ctx.rc.containerName;
        const auto guestRun = [&](const QString &cmd, const QStringList &local) {
            return backend == Backend::Bare ? sp.run(local, {}, log)
                                            : sp.run(Spawner::sshArgv(ctx.guest, cmd), {}, log);
        };
        const ProcResult st = guestRun(
            QStringLiteral("docker inspect -f '{{.State.Running}} {{.State.Paused}}' %1").arg(c),
            {"docker", "inspect", "-f", "{{.State.Running}} {{.State.Paused}}", c});
        const QStringList state = st.out.simplified().split(QLatin1Char(' '));
        if (state.value(1) == QLatin1String("true"))
            return StepResult::good(QStringLiteral("%1 is already asleep").arg(c));
        if (state.value(0) != QLatin1String("true"))
            return StepResult::fail(
                QStringLiteral("container %1 is not running — nothing to freeze").arg(c),
                st.stderrTail);
        // Close the LOCAL end of the mirror session before freezing the container, so the window
        // is not left sitting on a dead frame. The guest end needs nothing: the agent is an init
        // service that pauses and resumes with the container, not a per-session server that would
        // leak the VF encoder context if frozen (killStaleServer, removed).
        if (!ctx.rc.target.isEmpty())
            sp.run({"pkill", "-f", mirrorTitlePattern(ctx.rc.target, ctx.windowTitle)}, {}, log);
        const ProcResult p = guestRun(QStringLiteral("docker pause %1").arg(c),
                                      {"docker", "pause", c});
        if (p.rc != 0)
            return StepResult::fail(QStringLiteral("docker pause %1 failed").arg(c),
                                    p.stderrTail);
        return StepResult::good(
            QStringLiteral("%1 frozen — zero CPU, session preserved; Connect wakes it in ~1s")
                .arg(c));
    };
    QVector<Step> steps{{"sleep", "Freeze device (docker pause)", freeze}};
    return runChain(steps, sink);
}

bool smartRun(Backend backend, const RemoraConfig &cfg, LogSink &sink, Spawner &sp,
              const std::optional<HostCapabilities> &caps, const QString &windowTitle,
              const ConflictFn &onConflict, bool fullscreenDisplay) {
    RunContext ctx = makeContext(cfg, backend, caps, windowTitle, fullscreenDisplay);
    // Reconnect (fast path) only when the running container matches what this profile needs:
    //   (1) created from the image the tag currently points to (a rebuild/retag must recreate), and
    //   (2) booted with the codec2 pipeline when the profile wants it. the c2 boot arg is baked at
    //       `docker run` — a container missing it has NO working HEVC/opus encoder, so the mirrory's
    //       encoders die NAME_NOT_FOUND on every attempt. Reconnecting can't fix that; only a
    //       recreate (the deploy chain's bringup) can. Otherwise fall through to the full deploy.
    const QString probe = QStringLiteral("docker inspect -f '{{.State.Running}} {{.Image}} "
                                         "c2={{range .Args}}{{println .}}{{end}}"
                                         "binds={{range .HostConfig.Binds}}{{println .}}{{end}}"
                                         "devs={{range .HostConfig.Devices}}{{println .PathOnHost}}"
                                         "{{end}}\nnets=%3' %1 "
                                         "&& docker image inspect -f '{{.Id}}' %2")
                              .arg(ctx.rc.containerName, ctx.rc.imageTag,
                                   QString::fromUtf8(kNetworkInspectFormat));
    const ProcResult st =
        backend == Backend::Bare
            ? sp.run({"sh", "-c", probe}, {}, {})
            : sp.run(Spawner::sshArgv(ctx.guest, probe), {}, {});
    const QString out = st.out;
    const QStringList parts = out.simplified().split(QLatin1Char(' '));
    const bool running = parts.value(0) == QLatin1String("true");
    const QString containerImg = parts.value(1);
    const QString tagImg = parts.value(parts.size() - 1);
    const bool fresh = !containerImg.isEmpty() && containerImg == tagImg;
    // The RESOLVED boot-arg set against the container's own .Args, as ONE comparison
    // (bd remora-wlun). The per-arg checks this replaces — c2, geometry, fps, gpu_mode/node —
    // lagged the resolver by construction: gralloc became a boot arg and its drift reconnected
    // onto a stale container the same week, and every future arg (va_driver, remora_hwc, …)
    // would have reopened the hole. foldBootArgs folds ONE namespace now (step 4 dropped the
    // pre-flip tolerance, deliberately: a container still carrying the old spellings predates
    // the cutover and SHOULD be recreated rather than adopted). BOTH directions on purpose — an arg the
    // profile DROPPED is as stale as one it changed, because androidboot.* lands in
    // first-write-wins ro. props that keep the old value until a recreate (the exact shape
    // this bead was filed over: gralloc un-set, container still on the old HAL).
    const QStringList wantList = foldBootArgs(buildBootArgs(ctx.rc));
    auto want = QSet<QString>(wantList.cbegin(), wantList.cend());
    // Slice by FIRST occurrence and run to the end, not section(): the args block itself contains
    // "use_remora_c2=1", so splitting on every "c2=" truncates the block mid-line — and the fold
    // below drops every non-androidboot line, so the binds/devs/nets sections that follow simply
    // fall out regardless of their order.
    const int argsStart = int(out.indexOf(QLatin1String("c2=")));
    QStringList haveTrimmed;
    if (argsStart >= 0)
        for (const QString &l :
             out.mid(argsStart + 3).split(QLatin1Char('\n'), Qt::SkipEmptyParts))
            haveTrimmed << l.trimmed();
    const QStringList haveList = foldBootArgs(haveTrimmed);
    auto have = QSet<QString>(haveList.cbegin(), haveList.cend());
    // RETIRED keys leave the container side: an arg the current builder cannot produce in ANY
    // configuration is outside the contract this compares, and flagging it would recreate a
    // healthy instance to change nothing the builder controls. Found on the first live diff:
    // the daily container still carried an androidboot.hardware= arg (retired with the identity
    // rename — the image sets ro.hardware itself), and strict-both-ways would
    // have rebooted the user's session on the first post-upgrade `up`. Every key here should
    // eventually age out as pre-retirement containers get recreated for real reasons.
    have.removeIf([](const QString &kv) { return kv.startsWith(QLatin1String("hardware=")); });
    // A NEWLY ADDED key gets the mirror-image tolerance, in ONE direction only (bd remora-xqt1).
    // A container predating remora_vulkan is running on a pre-removal IMAGE, whose build.prop
    // still carries the native_vulkan pin and is therefore already supplying ro.hardware.vulkan —
    // so its absence here is not a disagreement about anything live, and recreating on it would
    // reboot a healthy session to change nothing, the trade this function already refuses for
    // retired keys. The arg starts mattering only on an image built without the pin, and that is
    // a NEW image, which staleImage recreates on its own — so the case where this tolerance would
    // hide a real problem cannot be reached. A container carrying a DIFFERENT value still drifts.
    // Ages out as pre-removal images leave; retire it with the rest of the step-4 set.
    bool haveVulkanKey = false;
    for (const QString &kv : haveList)
        if (kv.startsWith(QLatin1String("vulkan="))) {
            haveVulkanKey = true;
            break;
        }
    if (!haveVulkanKey)
        want.removeIf([](const QString &kv) { return kv.startsWith(QLatin1String("vulkan=")); });
    // Without a live capability probe (an app/desktop window opens without one) the resolver
    // legitimately cannot derive the node-dependent args — gpu_node, and the gralloc/va_driver
    // that follow the selected driver — so those keys leave BOTH sides rather than force a
    // recreate the popup path could never justify. `up` always probes, so the full set applies
    // exactly where a recreate can actually happen.
    if (!caps.has_value()) {
        const auto capDerived = [](const QString &kv) {
            return kv.startsWith(QLatin1String("gpu_node=")) ||
                   kv.startsWith(QLatin1String("gralloc=")) ||
                   kv.startsWith(QLatin1String("va_driver=")) ||
                   // vulkan follows the probed driver too (and the venus decision, which needs
                   // the NVIDIA node) — without a probe host mode cannot name it either.
                   kv.startsWith(QLatin1String("vulkan="));
        };
        want.removeIf(capDerived);
        have.removeIf(capDerived);
    }
    const bool argsOk = want == have;
    // Shared folders are docker-run binds, baked like the rest: check BOTH directions — a share
    // added to the config must exist in the container, and one removed must be gone (a folder the
    // user un-shared staying mounted would be a silent privacy leak). Count the /data/media/0
    // sub-binds, then match each configured one exactly.
    const bool sharesOk = [&] {
        // overlay deployments stage their share binds at /remora/share/ (Builders.cpp) — count
        // and compare whichever spec THIS config would emit
        const QLatin1String marker = ctx.rc.useOverlayfs ? QLatin1String(":/remora/share/")
                                                         : QLatin1String(":/data/media/0/");
        int n = 0;
        for (int i = 0; (i = int(out.indexOf(marker, i))) >= 0; ++n, ++i) {}
        if (n != ctx.rc.sharedFolders.size()) return false;
        for (const QString &f : ctx.rc.sharedFolders)
            if (!out.contains(ctx.rc.useOverlayfs ? stagedShareBind(f) : sharedFolderBind(f)))
                return false;
        return true;
    }();
    // Companion apps are docker-run binds too, and the failure is quieter than a stale share: the
    // app is a SYSTEM app, so a removed one keeps its Settings tile and its WRITE_SECURE_SETTINGS
    // reach until someone recreates, and an added one simply never appears while the deploy
    // reports success. Same both-directions check as the shares above.
    const bool appsOk = [&] {
        int n = 0;
        for (int i = 0;
             (i = int(out.indexOf(QLatin1String(":/system/system_ext/app/"), i))) >= 0; ++n, ++i) {
        }
        if (n != ctx.rc.companionApps.size()) return false;
        for (const QString &a : ctx.rc.companionApps)
            if (!out.contains(companionAppBind(a, ctx.rc.rezmodsDir.value_or(QString()))))
                return false;
        return true;
    }();
    // DEVICES are docker-run flags baked like everything above, and the camera is the one that
    // DRIFTS: /dev/video* appears and vanishes with a replug, the resolver probes it live, and a
    // container created while the camera was absent carries only the GPU nodes — so the fast path
    // reported healthy while the new nodes never reached Android, and the manual `docker rm` this
    // path exists to remove was back (bd remora-usn0). Judged by the shared rule (both
    // directions, like the shares); `running` gates the fast path below, so an empty section here
    // is a genuinely device-less container, not a missing one.
    const bool devsOk = !containerDevicesStale(out.section(QLatin1String("devs="), 1)
                                                   .section(QLatin1String("nets="), 0, 0)
                                                   .simplified()
                                                   .split(QLatin1Char(' '), Qt::SkipEmptyParts),
                                               ctx.rc);
    // NETWORKING is baked at docker run like everything above, and it is the one whose warm-
    // reconnect no-op is invisible from inside Android: a profile switched to macvlan reconnects
    // onto the bridged container it already had, the deploy reports success, and the only symptom
    // is an adb target that never answers — while `plan` and `check` both describe the macvlan
    // deployment that was never created (bd remora-400).
    const bool netOk = !containerNetworkStale(
        out.section(QLatin1String("nets="), 1).section(QLatin1Char('\n'), 0, 0), ctx.rc);
    // Every check above asks whether the RUNNING container still matches the profile — a question
    // only a deploy can act on, by recreating it. That is right for `up`, and wrong for an
    // ADDITIONAL window: opening one app popup would `docker rm -f` the device out from under
    // every window already on it, the mirror included, and the user asked for a window. So a
    // running container is the whole test here; a stale one is the mirror's problem to fix, on the
    // next `up`. Not running still falls through — an extra window does need the device up.
    if (ctx.extraWindow() && running)
        return reconnectRun(backend, cfg, sink, sp, caps, windowTitle, fullscreenDisplay);
    if (running && fresh && argsOk && sharesOk && appsOk && devsOk && netOk)
        return reconnectRun(backend, cfg, sink, sp, caps, windowTitle, fullscreenDisplay);
    // Falling through to a full deploy while OUR OWN container is running (stale image, missing
    // c2, or changed geometry) is a recreate, not a foreign-holder conflict — auto-consent when
    // the holder is our own target so `up` transparently rebuilds it (the deploy's bringup does
    // the docker rm). A genuinely different holder still defers to the caller's onConflict.
    const ConflictFn recreateOwn = [&](const Conflict &c) {
        if (c.sameName) return true;
        return onConflict ? onConflict(c) : false;
    };
    return connectRun(backend, cfg, sink, sp, caps, windowTitle, recreateOwn, fullscreenDisplay);
}

void ConsoleSink::state(const QString &step, StepState st, const QString &detail,
                        const QString &stderrTail) {
    QString glyph;
    if (st == StepState::Ok)
        glyph = QStringLiteral("✓");
    else if (st == StepState::Failed)
        glyph = QStringLiteral("✗");
    else if (st == StepState::Skipped)
        glyph = QStringLiteral("·");
    else
        return;  // skip pending/running for a clean one-line-per-step log

    QTextStream out(stdout);
    out << "  " << glyph << " " << step;
    if (!detail.isEmpty()) out << " — " << detail;
    out << "\n";
    out.flush();
    if (st == StepState::Failed && !stderrTail.isEmpty()) {
        QTextStream err(stderr);
        for (const QString &l : stderrTail.split(QLatin1Char('\n')))
            err << "      | " << l << "\n";
    }
}

}  // namespace remora
