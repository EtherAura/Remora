#include <QApplication>
#include <QIcon>
#include <QCommandLineParser>
#include <QSurfaceFormat>
#include <QRandomGenerator>
#include <QSocketNotifier>
#include <csignal>
#include <unistd.h>

#include "core/Branding.h"
#include "CudaInterop.h"
#include "MirrorSession.h"
#include "MirrorWindow.h"
#include "Splash.h"

// `remora mirror` — the in-house mirror client (bd remora-28ix.2). Speaks protocol v2 to the
// in-image agent (docs/MIRROR_AGENT.md) — the v1 jar path is gone since the purge
// (bd remora-28ix.5). Launched detached in its own scope by the engine; runnable by hand:
//   remora mirror -s 127.0.0.1:5555

namespace remora::mirror {

// The engine reaps a mirror with TERM (then KILL), and teardown — removing the adb tunnel,
// stopping the server — only runs if TERM leads back to the event loop. Async-signal-safe
// self-pipe: the handler writes a byte, the notifier quits the app, destructors do the rest.
static int sigPipe[2] = {-1, -1};
static void onTermSignal(int) {
    const char c = 1;
    (void)!::write(sigPipe[1], &c, 1);
}

int runMirrorMain(QApplication &app) {
    // Present without waiting for vsync. A blocking swap costs the ONE thing that also decodes
    // and uploads: miss a deadline and the whole pipeline halves to 30 fps and stays there,
    // which is what a 60 fps stream looked like before this. The compositor still paces what
    // reaches the screen, so this trades a lock we cannot afford for pacing we do not control.
    // REMORA_MIRROR_VSYNC=1 restores the blocking swap.
    QSurfaceFormat fmt = QSurfaceFormat::defaultFormat();
    if (qEnvironmentVariableIsEmpty("REMORA_MIRROR_VSYNC")) fmt.setSwapInterval(0);
    // The zero-copy decode-to-texture path (bd remora-28ix.2.3) needs a DESKTOP GL context —
    // cuGraphicsGLRegisterImage does not take GLES, and Qt's EGL auto-detection picked GLES on
    // the very host that has CUDA. Only steered when libcuda is present: such a host is NVIDIA,
    // whose EGL always offers desktop GL, while GLES-only stacks stay untouched.
    if (CudaInterop::available() && qEnvironmentVariableIsEmpty("REMORA_MIRROR_NO_INTEROP"))
        fmt.setRenderableType(QSurfaceFormat::OpenGL);
    QSurfaceFormat::setDefaultFormat(fmt);
    QCommandLineParser p;
    p.setApplicationDescription(QStringLiteral("Remora mirror client"));
    p.addHelpOption();
    p.addOptions({
        {{QStringLiteral("s"), QStringLiteral("serial")}, QStringLiteral("adb target"),
         QStringLiteral("serial")},
        {QStringLiteral("window-title"), QStringLiteral("window title"), QStringLiteral("title")},
        {QStringLiteral("video-codec"), QStringLiteral("h264|h265|av1"), QStringLiteral("codec")},
        {QStringLiteral("video-bit-rate"), QStringLiteral("bits per second"), QStringLiteral("n")},
        {QStringLiteral("max-size"), QStringLiteral("cap the larger dimension"), QStringLiteral("n")},
        {QStringLiteral("max-fps"), QStringLiteral("encoder frame cap"), QStringLiteral("n")},
        {QStringLiteral("mouse-bind"),
         QStringLiteral("what right/middle/4th/5th click do, then the same four with Shift; "
                        "b=back h=home s=recents n=notifications +=pass through -=ignore"),
         QStringLiteral("WXYZ:wxyz")},
        {QStringLiteral("key-bind"),
         QStringLiteral("unmodified keys bound to an action, e.g. Escape:b,F2:h"),
         QStringLiteral("Key:a,…")},
        {QStringLiteral("shortcut-mod"),
         QStringLiteral("modifier(s) arming MOD+b/h/s/n and Alt+D/Alt+F, e.g. lctrl+lalt,lsuper"),
         QStringLiteral("mods")},
        {QStringLiteral("shot"), QStringLiteral("save the first frame as PNG and exit"),
         QStringLiteral("path")},
        {QStringLiteral("external-video-socket"),
         QStringLiteral("compose mode: read video from this host unix socket"),
         QStringLiteral("path")},
        {QStringLiteral("boot-animation"),
         QStringLiteral("splash mode: status text and boot animation until hand-off")},
        {QStringLiteral("boot-animation-cache"),
         QStringLiteral("bootanimation.zip cache path (pull lands here)"), QStringLiteral("zip")},
        {QStringLiteral("boot-animation-status"),
         QStringLiteral("append-only status file to tail"), QStringLiteral("path")},
        {QStringLiteral("boot-animation-size"), QStringLiteral("initial splash size"),
         QStringLiteral("WxH")},
        {QStringLiteral("boot-animation-external-geometry"),
         QStringLiteral("an external authority positions the window (accepted; never self-move)")},
        {QStringLiteral("video-encoder"), QStringLiteral("pin a device encoder by name"),
         QStringLiteral("name")},
        {QStringLiteral("new-display"),
         QStringLiteral("desktop mode: mirror a new virtual display (WxH/dpi, or 'auto')"),
         QStringLiteral("size")},
        {QStringLiteral("start-app"),
         QStringLiteral("launch this app once connected (+pkg force-stops first)"),
         QStringLiteral("name")},
        {QStringLiteral("server-param"),
         QStringLiteral("raw key=value forwarded to the server (repeatable)"),
         QStringLiteral("k=v")},
        {QStringLiteral("window-width"), QStringLiteral("explicit window width"), QStringLiteral("n")},
        {QStringLiteral("window-height"), QStringLiteral("explicit window height"), QStringLiteral("n")},
        {QStringLiteral("window-x"), QStringLiteral("window x position"), QStringLiteral("n")},
        {QStringLiteral("window-y"), QStringLiteral("window y position"), QStringLiteral("n")},
        {QStringLiteral("always-on-top"), QStringLiteral("keep the window above others")},
        {QStringLiteral("fullscreen"), QStringLiteral("start fullscreen")},
        {QStringLiteral("app-id"),
         QStringLiteral("Wayland app_id / X11 class for this window (PIP and app windows)"),
         QStringLiteral("id")},
        {QStringLiteral("hwdec"),
         QStringLiteral("hardware decode: off|auto|cuda|nvdec|vaapi|/dev/dri/renderD*"),
         QStringLiteral("mode")},
        {QStringLiteral("list-apps"),
         QStringLiteral("print the device's app list (the app-menu contract) and exit")},
        {QStringLiteral("record"),
         QStringLiteral("record the received video (and, with --audio, audio) to this file "
                        "(mp4/mkv), no re-encode"),
         QStringLiteral("path")},
        {QStringLiteral("splash-shot"),
         QStringLiteral("save the first animation frame as PNG (debug)"), QStringLiteral("path")},
        {QStringLiteral("audio"),
         QStringLiteral("play device audio (opus via the second tunnel socket)")},
    });
    QCommandLineOption debugToggle(QStringLiteral("debug-toggle-display"),
                                   QStringLiteral("fire the Alt+D toggle after the first frame"));
    debugToggle.setFlags(QCommandLineOption::HiddenFromHelp);
    p.addOption(debugToggle);
    QCommandLineOption shotDelay(QStringLiteral("shot-delay-ms"),
                                 QStringLiteral("delay --shot past the first frame"),
                                 QStringLiteral("ms"));
    shotDelay.setFlags(QCommandLineOption::HiddenFromHelp);
    p.addOption(shotDelay);
    p.process(app);

    SessionOptions opts;
    opts.serial = p.value(QStringLiteral("serial"));
    if (opts.serial.isEmpty()) {
        fprintf(stderr, "mirror: need --serial\n");
        return 2;
    }
    opts.videoCodec = p.value(QStringLiteral("video-codec"));
    opts.videoEncoder = p.value(QStringLiteral("video-encoder"));
    opts.externalVideoSocket = p.value(QStringLiteral("external-video-socket"));
    opts.newDisplay = p.value(QStringLiteral("new-display"));
    opts.startApp = p.value(QStringLiteral("start-app"));
    opts.serverParams = p.values(QStringLiteral("server-param"));
    opts.hwDecode = p.value(QStringLiteral("hwdec"));
    opts.audio = p.isSet(QStringLiteral("audio"));
    opts.recordPath = p.value(QStringLiteral("record"));
    // Boot-splash sessions connect the moment the screen composites, which on a cold boot is
    // 10+ s before the in-image agent service starts — give the agent socket the same patience
    // the flow already gives adbd, instead of taking the old-image refusal for a boot race and
    // killing the splash window with it (bd remora-j6yf). Plain sessions keep the instant
    // refusal: their device is supposed to be up already.
    if (p.isSet(QStringLiteral("boot-animation"))) opts.agentWaitMs = 45000;
    if (p.isSet(QStringLiteral("video-bit-rate"))) {
        // Accept the inherited unit suffixes the remorarc carries ("30M", "800K").
        QString v = p.value(QStringLiteral("video-bit-rate")).trimmed().toUpper();
        int mult = 1;
        if (v.endsWith(QLatin1Char('M'))) { mult = 1000000; v.chop(1); }
        else if (v.endsWith(QLatin1Char('K'))) { mult = 1000; v.chop(1); }
        opts.videoBitRate = int(v.toDouble() * mult);
    }
    if (p.isSet(QStringLiteral("max-size")))
        opts.maxSize = p.value(QStringLiteral("max-size")).toInt();
    if (p.isSet(QStringLiteral("max-fps")))
        opts.maxFps = p.value(QStringLiteral("max-fps")).toDouble();
    if (p.isSet(QStringLiteral("mouse-bind"))) {
        // Refused, not defaulted: a typo here silently rebinds or disarms a button, and the point
        // of the option is that the user's chosen bindings are the ones that run.
        const QString spec = p.value(QStringLiteral("mouse-bind"));
        const auto parsed = parseMouseBind(spec);
        if (!parsed) {
            qCritical("mirror: --mouse-bind=%s is not a WXYZ:wxyz vector over b/h/s/n/+/-",
                      qUtf8Printable(spec));
            return 2;
        }
        opts.mouseBind = *parsed;
    }
    if (p.isSet(QStringLiteral("key-bind"))) {
        const QString spec = p.value(QStringLiteral("key-bind"));
        const auto parsed = parseKeyBind(spec);
        if (!parsed) {
            qCritical("mirror: --key-bind=%s is not a <Key>:<b|h|s|n> list", qUtf8Printable(spec));
            return 2;
        }
        opts.keyBind = *parsed;
    }
    if (p.isSet(QStringLiteral("shortcut-mod"))) {
        const QString spec = p.value(QStringLiteral("shortcut-mod"));
        const auto parsed = parseShortcutMod(spec);
        if (!parsed) {
            qCritical("mirror: --shortcut-mod=%s is not a list of lctrl/rctrl/lalt/ralt/lsuper/"
                      "rsuper combinations", qUtf8Printable(spec));
            return 2;
        }
        opts.shortcutMods = *parsed;
    }

    if (p.isSet(QStringLiteral("list-apps"))) {
        // kind=4, one request/response on a control connection (docs/MIRROR_AGENT.md): the
        // " * Name  pkg" lines, verbatim — the app-menu script's parsing contract. Agent only
        // since the v1 purge (bd remora-28ix.5): absent means the image predates the bake.
        QByteArray listing;
        QString err;
        bool absent = false;
        if (agentListApps(opts.serial, &listing, &err, &absent)) {
            fwrite(listing.constData(), 1, size_t(listing.size()), stdout);
            return 0;
        }
        fprintf(stderr, absent ? "mirror: no in-image agent (%s) — rebuild the image with the "
                                 "mirror_agent feature\n"
                               : "mirror: %s\n",
                qUtf8Printable(err));
        return 1;
    }

    // The window class identity: Wayland app_id and X11 WM_CLASS both derive from the desktop
    // file name — this is what keeps the PIP's constant class and per-app StartupWMClass
    // matching working.
    //
    // A window WITHOUT --app-id is Remora itself, and must say so: an unset desktop file name
    // leaves the compositor nothing to resolve, so the mirror showed a generic Wayland glyph
    // instead of the Remora icon. Only the per-app windows (PIP, app windows) carry their own id,
    // because the taskbar should show that app's name and icon rather than ours.
    const QString appId = p.value(QStringLiteral("app-id"));
    QGuiApplication::setDesktopFileName(appId.isEmpty() ? QString::fromLatin1(kDesktopId) : appId);
    // The association drives the TASKBAR; the titlebar needs a real QIcon on top of it (App.cpp
    // sets the same pair for the workspace window, for the same reason).
    QIcon icon = QIcon::fromTheme(appId.isEmpty() ? QString::fromLatin1(kDesktopId) : appId);
    if (icon.isNull() && appId.isEmpty())
        icon = QIcon(QString::fromLatin1(kIconPathTemplate).arg(QString::fromLatin1(kDesktopId)));
    if (!icon.isNull()) QApplication::setWindowIcon(icon);

    MirrorSession session(opts);
    QString title = p.value(QStringLiteral("window-title"));
    if (title.isEmpty()) title = QStringLiteral("Remora mirror — %1").arg(opts.serial);
    MirrorWindow window(&session, title, p.value(QStringLiteral("shot")));

    std::optional<QSize> exSize;
    std::optional<QPoint> exPos;
    if (p.isSet(QStringLiteral("window-width")) && p.isSet(QStringLiteral("window-height")))
        exSize = QSize(p.value(QStringLiteral("window-width")).toInt(),
                       p.value(QStringLiteral("window-height")).toInt());
    if (p.isSet(QStringLiteral("window-x")) && p.isSet(QStringLiteral("window-y")))
        exPos = QPoint(p.value(QStringLiteral("window-x")).toInt(),
                       p.value(QStringLiteral("window-y")).toInt());
    window.setExplicitGeometry(exSize, exPos);
    window.setAlwaysOnTop(p.isSet(QStringLiteral("always-on-top")));
    window.setStartFullscreen(p.isSet(QStringLiteral("fullscreen")));
    if (p.isSet(shotDelay)) window.setShotDelayMs(p.value(shotDelay).toInt());

    if (p.isSet(debugToggle)) {
        QObject::connect(&session, &MirrorSession::frameReady, &window,
                         [&window, fired = false]() mutable {
                             if (fired) return;
                             fired = true;
                             QTimer::singleShot(300, &window, &MirrorWindow::toggleDisplayMode);
                         });
    }

    // Bring-up's verdict, which used to be start()'s return value. Both entry points (hand-off
    // and the no-splash path) report here, so there is one place that decides a failed connect
    // is fatal.
    QObject::connect(&session, &MirrorSession::started, &app, [](bool ok, const QString &err) {
        if (ok) return;
        fprintf(stderr, "mirror: %s\n", qUtf8Printable(err));
        QCoreApplication::exit(1);
    });
    QObject::connect(&session, &MirrorSession::failed, &app, [](const QString &why) {
        fprintf(stderr, "mirror: fatal: %s\n", qUtf8Printable(why));
        QCoreApplication::exit(1);
    });
    QObject::connect(&session, &MirrorSession::finished, &app, [] {
        QCoreApplication::exit(0);  // stream EOF: the server or the device went away
    });

    SplashController *splash = nullptr;
    if (p.isSet(QStringLiteral("boot-animation"))) {
        QSize initial;
        const QStringList wh = p.value(QStringLiteral("boot-animation-size")).split(QLatin1Char('x'));
        if (wh.size() == 2) initial = QSize(wh[0].toInt(), wh[1].toInt());
        window.showSplash(initial);

        splash = new SplashController(
            {opts.serial, p.value(QStringLiteral("boot-animation-cache")),
             p.value(QStringLiteral("boot-animation-status"))},
            &app);
        const QString splashShot = p.value(QStringLiteral("splash-shot"));
        QObject::connect(splash, &SplashController::frame, &window,
                         [&window, splashShot, saved = false](const QImage &f) mutable {
                             window.setSplashFrame(f);
                             if (!splashShot.isEmpty() && !saved) {
                                 saved = true;
                                 f.save(splashShot);
                             }
                         });
        QObject::connect(splash, &SplashController::statusText, &window,
                         &MirrorWindow::setStatusText);
        // The window holds live video until the animation says it is done, so the ending is seen
        // instead of being preempted by the first frame a few hundred ms after hand-off.
        QObject::connect(splash, &SplashController::animationDone, &window,
                         &MirrorWindow::endSplash);
        // The window takes live video the instant it arrives; playback stops there so a
        // finished-with animation cannot keep decoding a PNG per frame behind the mirror.
        QObject::connect(&window, &MirrorWindow::liveVideoStarted, splash,
                         &SplashController::stopPlayback);
        QObject::connect(splash, &SplashController::abortRequested, &app,
                         [] { QCoreApplication::exit(3); });
        // Hand-off only KICKS OFF the bring-up: its adb steps run on a worker thread and the
        // outcome arrives on started(), so the animation keeps painting through the whole thing.
        // While this called a blocking start(), those subprocesses froze the GUI thread for ~0.23 s
        // — right at the moment that also winds the animation up to its ending, which is what made
        // the ending predictable (bd remora-xrlh).
        QObject::connect(splash, &SplashController::handOff, &app,
                         [&session] { session.startAsync(); });
    }

    if (::pipe(sigPipe) != 0) return 1;
    QSocketNotifier sigNotifier(sigPipe[0], QSocketNotifier::Read);
    QObject::connect(&sigNotifier, &QSocketNotifier::activated, &app, [] {
        char c;
        (void)!::read(sigPipe[0], &c, 1);
        QCoreApplication::quit();
    });
    std::signal(SIGTERM, onTermSignal);
    std::signal(SIGINT, onTermSignal);
    std::signal(SIGHUP, onTermSignal);

    if (splash) {
        // Pay for the hw decoder's device context HERE, in the dead time before the animation
        // has a frame to show, instead of on the first packet — which arrives at hand-off, when
        // the animation is mid-flight and a ~226 ms freeze is exactly what gave the ending away
        // (bd remora-xrlh). Blocking is fine at this point: nothing is animating yet.
        session.warmDecoder();
        splash->begin();  // the session starts at hand-off, driven by the status file / probe
    } else {
        // Queued onto the event loop below, not run here: bring-up is asynchronous now, and a
        // failure arrives on started() (wired above) rather than as a return value.
        session.startAsync();
        // The window shows itself on the first decoded frame (MirrorWindow::onFrame).
    }
    const int rc = app.exec();
    session.stop();  // explicit: teardown before the QApplication unwinds
    return rc;
}

}  // namespace remora::mirror

namespace remora {
int runMirror(int argc, char **argv) {
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("remora"));
    return remora::mirror::runMirrorMain(app);
}
}  // namespace remora
