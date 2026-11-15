#pragma once
#include "engine/Engine.h"
#include <QJsonObject>

namespace remora {
QString playTermsVersion();
QString playNotice();
// Arguments use the helper's strict parser; only our own source/manifest are staged remotely.
ProcResult runPlayInstall(Spawner &sp, const QStringList &arguments, const QString &host,
                          const LineSink &log = {});
// The helper's machine-readable result: the JSON on its REMORA_PLAY_RESULT= line, or empty.
QJsonObject playInstallResult(const QString &output);
// A profile's Google Play option made real for one deploy (bd remora-6laf.8): build or reuse the
// remora-local: overlay for the base the profile resolves to, on the Docker host that runs it, and
// record it in cfg.image.playOverlay so resolve() deploys it. Nothing to do when the option is off
// or the base already carries Google apps; refused when the accepted terms are not the current
// ones, since accepting them is the user's act and never this function's.
StepResult ensurePlayOverlay(Spawner &sp, RemoraConfig &cfg, Backend backend,
                             const LineSink &log = {});
}
