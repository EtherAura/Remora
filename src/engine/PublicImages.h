#pragma once
#include "engine/Engine.h"
#include <QJsonObject>

namespace remora {

// The official image source (bd remora-qksj): one fixed GitHub release on the public repository,
// holding a signed catalog (index.json + index.json.sig) and the audited archives it names. The
// GUI's "Official downloads" button adds exactly this location, and it is the only location whose
// listing and fetching go through vendor/release/catalog.py's verification.
inline const QString kPublicImageSource =
    QStringLiteral("https://github.com/EtherAura/Remora/releases/download/android-images");

// Run vendor/release/catalog.py (list | fetch | rollback) with Remora's public trust anchor, on
// this machine or — given `host` — on that Docker host over ssh. Remotely the helper and the key
// are copied into an owned mktemp directory, run there and removed afterwards, so the verification
// always happens on the host whose Docker is about to load the image.
ProcResult runPublicImages(Spawner &sp, const QStringList &args, const QString &host = {},
                           const LineSink &log = {});

// The helper's machine-readable result: the JSON on its REMORA_IMAGES_RESULT= line, or empty.
QJsonObject publicImagesResult(const QString &output);

}  // namespace remora
