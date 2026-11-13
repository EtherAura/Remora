#pragma once
#include "engine/Engine.h"

namespace remora {
QString playTermsVersion();
QString playNotice();
// Arguments use the helper's strict parser; only our own source/manifest are staged remotely.
ProcResult runPlayInstall(Spawner &sp, const QStringList &arguments, const QString &host,
                          const LineSink &log = {});
}
