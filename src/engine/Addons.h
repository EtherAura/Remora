#pragma once
#include "engine/Engine.h"

namespace remora {

// Where the add-on layer finds what it copies (bd remora-6laf.10). payloadRoots are searched in
// order, each holding <name>/ laid out as vendor/source-build/features/<name> is; vendorDir is the
// vendor tree carrying Remora's own halves (magisk.rc, stage-arm-layer.sh) and fetch-payloads.sh;
// stageParent is where a build context is assembled before Docker gets it.
// stageOnHost assembles the context on the Docker host instead (bd remora-7oih.3): the payloads
// and Remora's halves are copied into a fresh mktemp directory there, staged and built there, and
// a missing fetchable payload is fetched there and copied back. It is what a client with no local
// sh, cp or tar — Windows — does; elsewhere the context is assembled here.
struct AddonPaths {
    QStringList payloadRoots;
    QString vendorDir;
    QString stageParent;
    bool stageOnHost = false;
};
// The user's payload directory first ($REMORA_PAYLOAD_DIR, else ~/.local/share/remora/payloads),
// then the vendored features tree, which carries the payloads only in a checkout that has them.
// stageOnHost is set on Windows.
AddonPaths defaultAddonPaths();
// Where a payload Remora cannot fetch is put, and where it fetches the one it can.
QString userPayloadDir();

struct AddonPayload {
    QString feature;         // the add-on: magisk_root, arm_translate, widevine_l3
    QString name;            // its directory under a payload root: magisk, ndk_translation, widevine
    QString dir;             // the payload root/<name> it was found in; empty when it was not
    bool fetchable = false;  // fetch-payloads.sh can download it from a recorded upstream by hash
};
AddonPayload locateAddonPayload(const QString &feature, const AddonPaths &paths = defaultAddonPaths());

// The profile's add-ons made real for one deploy: build or reuse the remora-local:addons- layer
// over the image the profile resolves to (the Google Play layer when that is on, so call this
// after ensurePlayOverlay), on the Docker host that runs it, and record it in
// cfg.image.addonOverlay so resolve() deploys it. Nothing to do when addons= is empty or the
// registered base already carries every one. A payload that is missing and cannot be fetched is a
// refusal that names the directory to put it in.
StepResult ensureAddonOverlay(Spawner &sp, RemoraConfig &cfg, Backend backend,
                              const LineSink &log = {},
                              const AddonPaths &paths = defaultAddonPaths());

}  // namespace remora
