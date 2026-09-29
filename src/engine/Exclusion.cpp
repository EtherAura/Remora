#include "engine/Engine.h"

#include "core/Parsers.h"

namespace remora {

namespace {

// One record per running container: name, host ports, bind sources. Two calls rather than one
// because `docker ps` alone cannot report bind SOURCES, and those are what a /data collision is.
// Tab-separated fields, '|'-separated list items — a bind is "src:dst:opts" and a path may
// contain spaces, so neither of those can be the separator.
constexpr const char *kInspectFormat =
    "{{.Name}}\t"
    "{{range $p, $b := .HostConfig.PortBindings}}{{range $b}}{{.HostPort}}|{{end}}{{end}}\t"
    "{{range .HostConfig.Binds}}{{.}}|{{end}}\t"
    "{{range .HostConfig.Devices}}{{.PathOnHost}}|{{end}}";

QVector<ContainerFacts> inspectAll(Spawner &sp, const QSet<QString> &names, const QString &guest) {
    if (names.isEmpty()) return {};
    QStringList sorted(names.begin(), names.end());
    sorted.sort();  // deterministic order so a conflict message is stable across runs
    if (guest.isEmpty()) {
        QStringList argv{"docker", "inspect", "--format", QString::fromUtf8(kInspectFormat)};
        argv += sorted;
        return parseDockerInspectFacts(sp.run(argv).out);
    }
    const QString cmd = QStringLiteral("docker inspect --format '%1' %2")
                            .arg(QString::fromUtf8(kInspectFormat), sorted.join(QLatin1Char(' ')));
    auto r = sp.run(Spawner::sshArgv(guest, cmd));
    return r.rc == 0 ? parseDockerInspectFacts(r.out) : QVector<ContainerFacts>{};
}

}  // namespace

ClusterState ExclusionService::probe(bool includeGuest) {
    ClusterState st;
    // includeGuest is "the docker host is not this machine". In that case the local containers
    // are irrelevant to the decision, so do not pay for a second local call to inspect them.
    if (!includeGuest) {
        st.local = inspectAll(
            sp_, parseDockerPsNames(sp_.run({"docker", "ps", "--format", "{{.Names}}"}).out),
            QString());
        return st;
    }
    auto r = sp_.run(Spawner::sshArgv(guest_, "docker ps --format '{{.Names}}'"));
    if (r.rc == 0) st.guest = inspectAll(sp_, parseDockerPsNames(r.out), guest_);
    return st;
}

void ExclusionService::stop(Backend backend, const QString &containerName, const QString &target) {
    if (containerName.isEmpty()) return;
    if (backend == Backend::Bare)
        sp_.run({"docker", "rm", "-f", containerName});
    else
        sp_.run(Spawner::sshArgv(guest_, "docker rm -f " + containerName));
    // Always local, whatever the backend: the mirror clients run on THIS machine even when the
    // container is remote, and the encoder only ever exists where the venus stack does — bare.
    // Without this the session outlived its container (bd remora-e5x.19.1).
    reapSessionOrphans(sp_, containerName, target, {});
}

std::optional<Conflict> conflictFor(const ClusterState &s, Backend backend,
                                    const ResolvedConfig &rc) {
    // bare arbitrates against local docker; remote against the ssh host's. An unreachable host
    // yields no facts at all, and we claim no conflict rather than guessing at one — the deploy
    // then fails on its own terms (ssh/docker unreachable), which is the honest error.
    if (backend != Backend::Bare && !s.guest) return std::nullopt;
    const QVector<ContainerFacts> &running = (backend == Backend::Bare) ? s.local : *s.guest;

    // Pass 1 — our own container. It collides with itself on every resource, so it must be
    // recognised before any resource check or `up` would prompt to take over from itself.
    //
    // "Ours" USED TO INCLUDE THE PRE-RENAME TWIN and no longer does (bd remora-28ix.4 step 4).
    // A profile whose container_name had just moved to remora-<x> was still looking at the
    // container it had always owned, and without the twin match the rename read as a FOREIGN
    // holder of the data dir: `up` refused, and refused BEFORE provision, so the adoption rename
    // was never reached — the user's own instance became un-deployable by a rename meant to be
    // transparent. That window closed with the adoption shim itself: nothing renames such a
    // container any more, so auto-consenting to it would recreate over a container this profile
    // has no live claim to. A pre-rename leftover is now a foreign holder in fact as well as in
    // name, and the explicit takeover prompt is the correct answer rather than a regression.
    // (This removal was first made alongside the shim's and LOST in an unrelated revert; the
    // tail-sweep that found the stray twin-name constructor is what caught it.)
    for (const ContainerFacts &c : running)
        if (c.name == rc.containerName)
            return Conflict{c.name, QStringLiteral("the same container name"), true};

    // Pass 2 — a genuinely different container holding a resource we need. Only two matter, and
    // both are fatal rather than cosmetic: docker refuses the port bind, and two Android instances
    // writing one /data is corruption. dataBaseDir is deliberately NOT checked — under
    // use_overlayfs it is the shared lowerdir that every profile on the host is meant to share.
    const QString port = QString::number(rc.hostAdbPort);
    // Device nodes this profile claims EXCLUSIVELY. GPU render nodes are deliberately absent:
    // they are genuinely multi-client and every instance is meant to share one. Cameras are not —
    // V4L2 capture is single-opener, so the second instance's camera simply fails to open. That is
    // the recorded policy for bd remora-4u4.6: an explicitly passed host device is exclusive unless
    // it is a GPU node.
    const QStringList exclusive = rc.cameraDevices;

    for (const ContainerFacts &c : running) {
        if (c.hostPorts.contains(port))
            return Conflict{c.name, QStringLiteral("the host adb port %1").arg(port), false};
        if (!rc.dataDir.isEmpty() && c.bindSources.contains(rc.dataDir))
            return Conflict{c.name, QStringLiteral("the data dir %1").arg(rc.dataDir), false};
        for (const QString &dev : exclusive)
            if (c.devices.contains(dev))
                return Conflict{c.name, QStringLiteral("the device %1").arg(dev), false};
    }
    return std::nullopt;
}

}  // namespace remora
