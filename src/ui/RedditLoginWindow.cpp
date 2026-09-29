#include "ui/RedditLoginWindow.h"

#include <QApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QNetworkCookie>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStackedWidget>
#include <QThread>
#include <QUrl>
#include <QVBoxLayout>
#include <QWebEngineCookieStore>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineView>

#include "engine/Engine.h"
#include "engine/RedditLogin.h"
#include "store/Store.h"

namespace remora {

struct RedditLoginWindow::Impl {
    RemoraConfig cfg;
    Backend backend;
    QWebEngineProfile *profile = nullptr;
    QWebEngineView *view = nullptr;
    QStackedWidget *stack = nullptr;
    QLabel *status = nullptr;
    QPlainTextEdit *logView = nullptr;
    QPushButton *closeBtn = nullptr;
    QThread *worker = nullptr;
    bool captured = false;  // a valid logged-in session has been picked up — stop probing
    bool probing = false;   // an /api/me.json probe is in flight
};

// Parse the username out of /api/me.json ({"data":{"name":"…"}}). Empty when logged out / unparsable.
static QString nameFromMeJson(const QString &body) {
    return QJsonDocument::fromJson(body.toUtf8())
        .object()
        .value(QStringLiteral("data"))
        .toObject()
        .value(QStringLiteral("name"))
        .toString();
}

RedditLoginWindow::RedditLoginWindow(RemoraConfig cfg, Backend backend, QWidget *parent)
    : QWidget(parent), d_(new Impl) {
    d_->cfg = std::move(cfg);
    d_->backend = backend;

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    d_->stack = new QStackedWidget(this);
    root->addWidget(d_->stack);

    // Phase 1 — the login WebView on an off-the-record profile (cookies stay in memory).
    d_->profile = new QWebEngineProfile(this);  // no name = off-the-record
    d_->view = new QWebEngineView(this);
    d_->view->setPage(new QWebEnginePage(d_->profile, d_->view));
    d_->stack->addWidget(d_->view);

    // Phase 2 — transplant progress.
    auto *progress = new QWidget(this);
    auto *pl = new QVBoxLayout(progress);
    pl->setContentsMargins(16, 16, 16, 16);
    d_->status = new QLabel(QStringLiteral("Waiting for login…"), progress);
    QFont f = d_->status->font();
    f.setBold(true);
    d_->status->setFont(f);
    d_->logView = new QPlainTextEdit(progress);
    d_->logView->setReadOnly(true);
    d_->closeBtn = new QPushButton(QStringLiteral("Close"), progress);
    d_->closeBtn->setEnabled(false);
    connect(d_->closeBtn, &QPushButton::clicked, this, &QWidget::close);
    pl->addWidget(d_->status);
    pl->addWidget(d_->logView, 1);
    pl->addWidget(d_->closeBtn, 0, Qt::AlignRight);
    d_->stack->addWidget(progress);

    // Capture the session cookie as soon as it appears, then confirm the login by fetching the
    // username through a hidden probe page (same profile → same cookie jar → authenticated). The
    // probe is why we don't trust runJavaScript(fetch(...)): runJavaScript can't await a Promise.
    connect(d_->profile->cookieStore(), &QWebEngineCookieStore::cookieAdded, this,
            [this](const QNetworkCookie &c) {
                if (d_->captured || d_->probing) return;
                if (c.name() != QByteArrayLiteral("reddit_session")) return;
                if (!c.domain().contains(QStringLiteral("reddit.com"))) return;
                const QString cookie = QString::fromUtf8(c.value());
                if (cookie.length() < 40) return;  // ignore trivial/placeholder values

                d_->probing = true;
                auto *probe = new QWebEnginePage(d_->profile, this);
                connect(probe, &QWebEnginePage::loadFinished, this,
                        [this, probe, cookie](bool ok) {
                            if (!ok) {
                                d_->probing = false;
                                probe->deleteLater();
                                return;
                            }
                            probe->toPlainText([this, probe, cookie](const QString &body) {
                                const QString user = nameFromMeJson(body);
                                d_->probing = false;
                                probe->deleteLater();
                                if (!user.isEmpty() && !d_->captured) {
                                    d_->captured = true;
                                    startTransplant(user, cookie);
                                }
                            });
                        });
                probe->load(QUrl(QStringLiteral("https://www.reddit.com/api/me.json")));
            });

    d_->view->load(QUrl(QStringLiteral("https://www.reddit.com/login/")));
}

RedditLoginWindow::~RedditLoginWindow() {
    if (d_->worker) {
        d_->worker->wait();  // never tear down the window out from under the transplant
        delete d_->worker;
    }
    delete d_;
}

void RedditLoginWindow::startTransplant(const QString &username, const QString &cookie) {
    d_->status->setText(QStringLiteral("Signing in as u/%1 — this takes ~40s (the framework "
                                       "restarts once)…")
                            .arg(username));
    d_->stack->setCurrentIndex(1);

    // The transplant blocks (ssh, a framework restart). Run it off the GUI thread and marshal the
    // log lines and the result back with queued calls.
    d_->worker = QThread::create([this, username, cookie] {
        RealSpawner sp;
        RunContext ctx = makeContext(d_->cfg, d_->backend);
        const bool ok = injectRedditSession(
            sp, ctx, username, cookie, [this](const QString &, const QString &line) {
                QMetaObject::invokeMethod(this, [this, line] { appendLog(line); },
                                          Qt::QueuedConnection);
            });
        QMetaObject::invokeMethod(this, [this, ok, username] { onTransplantDone(ok, username); },
                                  Qt::QueuedConnection);
    });
    d_->worker->start();
}

void RedditLoginWindow::appendLog(const QString &line) { d_->logView->appendPlainText(line); }

void RedditLoginWindow::onTransplantDone(bool ok, const QString &username) {
    d_->status->setText(ok ? QStringLiteral("✓ Reddit is now logged in as u/%1.").arg(username)
                           : QStringLiteral("✗ Something went wrong — see the log above."));
    d_->closeBtn->setEnabled(true);
}

int runRedditLoginGui(int argc, char **argv) {
    // WebEngine wants a shared GL context set before the QApplication exists.
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("remora"));
    QApplication::setApplicationDisplayName(QStringLiteral("Remora"));

    // First non-flag token after "reddit-login" is the profile; empty → the active instance.
    QString profile;
    const QStringList args = QApplication::arguments();
    for (int i = 2; i < args.size(); ++i) {
        if (args[i].startsWith(QLatin1Char('-'))) {
            if (args[i] == QLatin1String("--backend") || args[i] == QLatin1String("--user")) ++i;
            continue;
        }
        profile = args[i];
        break;
    }
    const QString path = remora::defaultRemorarcPath();
    const QString inst = profile.isEmpty() ? remora::activeInstance(path) : profile;
    remora::RemoraConfig cfg = remora::loadInstance(path, inst);
    const remora::Backend backend = remora::inferBackend(cfg);

    remora::RedditLoginWindow w(cfg, backend);
    w.setWindowTitle(QStringLiteral("Remora — Reddit login (%1)").arg(inst));
    w.resize(920, 840);
    w.show();
    return app.exec();
}

}  // namespace remora
