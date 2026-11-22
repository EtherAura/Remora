#include "engine/UpdateWatch.h"

#include "engine/PublicImages.h"

#include <QChar>

namespace remora {

UpdateWatchPass runUpdateWatchPass(SelfImageWatch &self, UpdateWatchState &st,
                                   const UpdateWatchOps &ops) {
    // A failed exec keeps this process serving and tries the same file again next pass.
    switch (self.check()) {
    case SelfImageWatch::Outcome::Replaced:
        return UpdateWatchPass::Restarted;
    case SelfImageWatch::Outcome::ExecFailed:
        if (self.lastErrno() != st.selfExecErr)
            ops.say(QStringLiteral("remora update-watch: could not start the replaced %1 (%2) — "
                                   "still serving from the running copy")
                        .arg(self.launchPath(), qt_error_string(self.lastErrno())));
        st.selfExecErr = self.lastErrno();
        break;
    default:
        st.selfExecErr = 0;
        break;
    }

    const UpdateWatchProfile p = ops.profile();
    // An image without the updater has nothing on the device to read the state. Stay resident,
    // so a profile that moves onto one is served without a restart.
    if (!p.hasUpdater) return UpdateWatchPass::NoUpdater;

    // OFFICIAL RELEASES, for a profile that follows the official image and nothing else: a pinned
    // version stays where it was pinned, and a private build was never published. Checked against
    // the signed catalog, never downloaded here — the download is Apply's. Once the catalog has
    // answered, a failed check waits for the next slot like a good one: a dead network is not
    // retried every 15 seconds, and the last answer stands until a check replaces it. Before its
    // first answer there is no answer to stand, so every pass asks.
    const bool official = p.imageTag == kPublicImageTag;
    const QDateTime now = ops.now();
    if (!official) {
        st.offer = {};
        st.offerFor.clear();
    } else if (st.offerFor != p.imageTag || !st.nextCheck.isValid() || now >= st.nextCheck) {
        const ProcResult c = ops.checkRelease();
        const QJsonObject r = publicImagesResult(c.out);
        if (c.rc == 0 && !r.isEmpty()) {
            st.offer = r.value(QLatin1String("update")).toBool() ? r : QJsonObject();
            st.offerFor = p.imageTag;
        } else {
            ops.say(QStringLiteral("release check failed: ") +
                    (c.stderrTail.isEmpty() ? c.out.right(500) : c.stderrTail));
        }
        st.nextCheck = now.addSecs(kReleaseCheckSecs);
    }

    if (!ops.probe(st.offer, st.lastError)) {
        st.heldSaid = false;
        return UpdateWatchPass::Idle;
    }
    // The catalog has not answered since this process started, so the release the device asked
    // for is unknown here: keep the signal, and the next pass asks again.
    if (official && st.offerFor != p.imageTag) {
        if (!st.heldSaid)
            ops.say(QStringLiteral("apply requested from the device — waiting for the release "
                                   "catalog to answer before updating; the request is kept"));
        st.heldSaid = true;
        return UpdateWatchPass::Held;
    }
    st.heldSaid = false;

    ops.say(QStringLiteral("apply requested from the device — updating…"));
    st.lastError.clear();
    bool ok = true;
    // A release the host does not have yet is downloaded, verified and imported first; the deploy
    // then finds the tag moved, layers Google Play over the new base when the profile has it, and
    // recreates the device on it with its data kept.
    if (!st.offer.isEmpty()) {
        const ProcResult f = ops.fetchRelease();
        if (f.rc != 0) {
            ok = false;
            st.lastError = QStringLiteral("the download of %1 was not completed (%2)")
                               .arg(st.offer.value(QLatin1String("version")).toString(),
                                    f.stderrTail.trimmed().section(QLatin1Char('\n'), -1));
        } else {
            st.offer = {};
            st.nextCheck = QDateTime();  // confirm against the catalog on the next pass
        }
    }
    if (ok && !ops.deploy()) {
        ok = false;
        st.lastError = QStringLiteral("the device could not be restarted on the new image — see "
                                      "journalctl --user -u remora-updater-*");
    }
    ops.clearApply();
    return ok ? UpdateWatchPass::Applied : UpdateWatchPass::Failed;
}

}  // namespace remora
