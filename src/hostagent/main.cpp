// remora-host-agent — serves this computer's own Linux apps over the mirror protocol
// (docs/PHONE_APP.md §3). A separate executable from `remora` because KWin grants its
// restricted interfaces per executable: the privilege belongs to this program alone.
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QTextStream>
#include <QTimer>

#include <QSocketNotifier>

#include <csignal>
#include <fcntl.h>
#include <sys/prctl.h>
#include <unistd.h>

#include "AppCatalog.h"
#include "DeskLayout.h"
#include "Gpu.h"
#include "HostAgent.h"
#include "KWinWayland.h"
#include "PipeWireCapture.h"

#ifndef REMORA_VENDOR_DIR
#define REMORA_VENDOR_DIR ""
#endif

using namespace remora::hostagent;

namespace {

QString encoderBinary() {
    return QStringLiteral(REMORA_VENDOR_DIR "/native/remora-frame-encoder");
}

// `probe`: prove the grant, the virtual output and (with --capture N) the whole picture path
// without serving anything. Creates one output, holds the desk's layout still, and either keeps
// it for --hold seconds or encodes N frames of it to --out; then removes it and puts the desk
// back.
int runProbe(const QCommandLineParser &args) {
    QTextStream out(stdout), err(stderr);
    KWinWayland wl;
    DeskLayout desk;
    QString why;
    if (!wl.connectToCompositor(&why)) {
        err << "remora-host-agent: " << why << "\n  "
            << grantHint(QCoreApplication::applicationFilePath()) << "\n";
        return 1;
    }
    if (!desk.snapshot(&why)) {
        err << "remora-host-agent: " << why << "\n";
        return 1;
    }
    out << "granted: screencast" << (wl.hasFakeInput() ? ", fake input" : "") << "\n";
    const QSize pixels(args.value(QStringLiteral("width")).toInt(),
                       args.value(QStringLiteral("height")).toInt());
    const double scale = args.value(QStringLiteral("scale")).toDouble();
    const int frames = args.value(QStringLiteral("capture")).toInt();
    auto *vo = wl.createVirtualOutput(args.value(QStringLiteral("name")),
                                      QStringLiteral("Remora probe"), pixels);
    PipeWireCapture capture;
    QProcess encoder;
    encoder.setProcessChannelMode(QProcess::ForwardedErrorChannel);
    // The encoder holds GPU memory; it must not outlive this process however this one ends.
    encoder.setChildProcessModifier([] { ::prctl(PR_SET_PDEATHSIG, SIGTERM); });
    const QString runtime = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    const QString frameSocket = runtime + QStringLiteral("/remora-probe-frames.sock");
    int rc = 0;
    quint64 streamSerial = 0;
    bool finishing = false;
    auto finish = [&](int code) {
        rc = rc ? rc : code;
        if (finishing) return;
        finishing = true;
        capture.stop();
        if (encoder.state() != QProcess::NotRunning) {
            encoder.terminate();
            if (!encoder.waitForFinished(3000)) encoder.kill();
            encoder.waitForFinished(1000);
        }
        delete vo;
        vo = nullptr;
        wl.flush();
        // Give KWin a moment to drop the output before putting the desk back.
        QTimer::singleShot(500, qApp, [&] {
            desk.restore();
            qApp->quit();
        });
    };

    QObject::connect(vo, &VirtualOutput::ready, [&](quint64 serial) {
        out << "output " << vo->outputName() << " " << pixels.width() << "x" << pixels.height()
            << " → PipeWire serial " << serial << "\n";
        out.flush();
        streamSerial = serial;
        desk.repair({{vo->outputName(), scale}});
    });
    QObject::connect(&desk, &DeskLayout::settled, [&](bool ok, const QString &detail) {
        if (!ok) {
            err << "remora-host-agent: the desk layout did not settle: " << detail << "\n";
            return finish(1);
        }
        const auto geo = desk.geometryOf(vo->outputName());
        out << "desk settled; " << vo->outputName() << " at "
            << (geo ? QStringLiteral("%1,%2 %3x%4 (logical)")
                          .arg(geo->x()).arg(geo->y()).arg(geo->width()).arg(geo->height())
                    : QStringLiteral("?"))
            << "\n";
        out.flush();
        if (frames <= 0) {
            QTimer::singleShot(args.value(QStringLiteral("hold")).toInt() * 1000, qApp,
                               [&] { finish(0); });
            return;
        }
        const QString node = renderNodeForConnector(desk.primaryOutput());
        QFile::remove(frameSocket);
        const QStringList encArgs{QStringLiteral("--frame-socket"), frameSocket,
                                  QStringLiteral("--out"), args.value(QStringLiteral("out")),
                                  QStringLiteral("--drm-node"), node,
                                  QStringLiteral("--frames"), QString::number(frames),
                                  QStringLiteral("--timeout"), QStringLiteral("10")};
        out << "encoder on " << node << " (the GPU behind " << desk.primaryOutput() << ")\n";
        out.flush();
        encoder.start(encoderBinary(), encArgs);
        // The encoder creates its socket once it is listening.
        auto *wait = new QTimer(qApp);
        int tries = 0;
        QObject::connect(wait, &QTimer::timeout, [&, wait] {
            if (!QFileInfo::exists(frameSocket) && ++tries < 60) return;
            wait->stop();
            wait->deleteLater();
            QString e;
            if (!capture.start(streamSerial, frameSocket, &e)) {
                err << "remora-host-agent: " << e << "\n";
                finish(1);
            }
        });
        wait->start(50);
    });
    QObject::connect(vo, &VirtualOutput::failed, [&](const QString &e) {
        err << "remora-host-agent: KWin refused the virtual output: " << e << "\n";
        finish(1);
    });
    QObject::connect(&capture, &PipeWireCapture::streaming, [&](QSize size, bool dmabuf) {
        out << "streaming " << size.width() << "x" << size.height() << " via "
            << (dmabuf ? "DMA-BUF" : "shared memory") << "\n";
        out.flush();
    });
    QObject::connect(&capture, &PipeWireCapture::failed, [&](const QString &e) {
        // An encoder that has encoded its N frames closes the socket, and the capture sees that
        // first: give the encoder's own exit the chance to say it was done, not dead.
        QTimer::singleShot(300, qApp, [&, e] {
            if (finishing) return;
            err << "remora-host-agent: " << e << "\n";
            finish(1);
        });
    });
    QObject::connect(&encoder, &QProcess::finished, [&](int code, QProcess::ExitStatus) {
        out << "encoder exited " << code << "\n";
        finish(code == 0 ? 0 : 1);
    });
    QObject::connect(&wl, &KWinWayland::disconnected, [&](const QString &e) {
        err << "remora-host-agent: " << e << "\n";
        finish(1);
    });
    QCoreApplication::exec();
    return rc;
}

// `serve`: the agent proper, until KWin goes away or it is stopped.
int runServe(const QCommandLineParser &args) {
    QTextStream err(stderr);
    HostAgent agent;
    const QString reserve = args.value(QStringLiteral("vram-reserve"));
    agent.setVramReserve(reserve == QLatin1String("auto") ? HostAgent::kAutoReserve
                                                          : reserve.toLongLong());
    QString why;
    const QString socket = args.isSet(QStringLiteral("socket"))
                               ? args.value(QStringLiteral("socket"))
                               : HostAgent::defaultSocketPath();
    if (!agent.start(socket, encoderBinary(), &why)) {
        err << "remora-host-agent: " << why << "\n";
        return 1;
    }
    int rc = 0;
    QObject::connect(&agent, &HostAgent::fatal, [&](const QString &e) {
        err << "remora-host-agent: " << e << "\n";
        rc = 1;
        qApp->quit();
    });
    // SIGINT/SIGTERM end the event loop so every session is torn down and the desk put back. The
    // handler only writes a byte to a pipe — the one thing a signal handler may safely do — and
    // the event loop reads it.
    static int quitPipe[2] = {-1, -1};
    if (::pipe2(quitPipe, O_CLOEXEC) == 0) {
        for (const int sig : {SIGINT, SIGTERM}) {
            std::signal(sig, [](int) {
                const char b = 1;
                [[maybe_unused]] const auto n = ::write(quitPipe[1], &b, 1);
            });
        }
        auto *notifier = new QSocketNotifier(quitPipe[0], QSocketNotifier::Read, &agent);
        QObject::connect(notifier, &QSocketNotifier::activated, qApp, &QCoreApplication::quit);
    }
    err << "remora-host-agent: serving on " << socket << "\n";
    err.flush();
    QCoreApplication::exec();
    return rc;
}

}  // namespace

int main(int argc, char **argv) {
    // A GUI application for QIcon and QPixmap (app icons), on the offscreen platform: the agent's
    // own Wayland connection is plain libwayland, and Qt must not open a second, visible one. The
    // variable is put back at once — every child inherits the environment, and kscreen-doctor on
    // the offscreen platform sees no outputs, and an app kstart launched with it would run
    // invisibly.
    const bool hadPlatform = qEnvironmentVariableIsSet("QT_QPA_PLATFORM");
    const QByteArray userPlatform = qgetenv("QT_QPA_PLATFORM");
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    if (hadPlatform) qputenv("QT_QPA_PLATFORM", userPlatform);
    else qunsetenv("QT_QPA_PLATFORM");
    useDeskIconTheme();
    QCoreApplication::setApplicationName(QStringLiteral("remora-host-agent"));
    QCommandLineParser args;
    args.setApplicationDescription(QStringLiteral("Serve this computer's apps to a paired phone."));
    args.addHelpOption();
    args.addPositionalArgument(QStringLiteral("command"), QStringLiteral("serve | probe"));
    args.addOption({QStringLiteral("socket"), QStringLiteral("serve: listen here."),
                    QStringLiteral("path")});
    // 1331 MiB (1.3 GiB): a monitor's mode set failed with about that much free.
    args.addOption({QStringLiteral("vram-reserve"),
                    QStringLiteral("serve: video memory (MiB) a session must leave free for the "
                                   "desk's own monitors, or auto: twice their screen buffers."),
                    QStringLiteral("MiB|auto"), QStringLiteral("1331")});
    args.addOption({QStringLiteral("name"), QStringLiteral("Output name."), QStringLiteral("name"),
                    QStringLiteral("remora-probe")});
    args.addOption({QStringLiteral("width"), QStringLiteral("Width in pixels."),
                    QStringLiteral("px"), QStringLiteral("1080")});
    args.addOption({QStringLiteral("height"), QStringLiteral("Height in pixels."),
                    QStringLiteral("px"), QStringLiteral("2400")});
    args.addOption({QStringLiteral("scale"), QStringLiteral("Scale factor."),
                    QStringLiteral("factor"), QStringLiteral("2.625")});
    args.addOption({QStringLiteral("hold"), QStringLiteral("Seconds to keep the output."),
                    QStringLiteral("s"), QStringLiteral("5")});
    args.addOption({QStringLiteral("capture"), QStringLiteral("Encode this many frames."),
                    QStringLiteral("n"), QStringLiteral("0")});
    args.addOption({QStringLiteral("out"), QStringLiteral("Where --capture writes Annex-B."),
                    QStringLiteral("file"), QStringLiteral("remora-probe.h265")});
    args.process(app);
    const QStringList pos = args.positionalArguments();
    if (pos.value(0) == QLatin1String("serve")) return runServe(args);
    if (pos.value(0) == QLatin1String("probe")) return runProbe(args);
    args.showHelp(2);
}
