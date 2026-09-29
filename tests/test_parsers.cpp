#include <QFile>
#include <QtTest>

#include "core/Backend.h"
#include "core/Parsers.h"

using namespace remora;

static QString fx(const QString &name) {
    QFile f(QStringLiteral(FIXTURES_DIR) + "/" + name);
    if (!f.open(QIODevice::ReadOnly)) return QString();
    return QString::fromUtf8(f.readAll());
}

class TestParsers : public QObject {
    Q_OBJECT
private slots:
    void bootCompleted() {
        QVERIFY(parseBootCompleted(fx("getprop_boot.txt")) == std::optional<bool>(true));
        QVERIFY(parseBootCompleted("0\r\n") == std::optional<bool>(false));
        QVERIFY(!parseBootCompleted("").has_value());
    }

    void gralloc() {
        QVERIFY(parseGralloc(fx("getprop_gralloc.txt")) == std::optional<QString>("gbm"));
    }

    void gles() {
        auto g = parseSurfaceflingerGles(fx("surfaceflinger.txt"));
        QVERIFY(g.has_value());
        QCOMPARE(*g, QString("GLES: Intel, Mesa Intel(R) UHD Graphics 770 (ADL-S GT1)"));
    }

    // kbdNodeIsEvent5 and hwKbdPresent were deleted with the parsers they tested:
    // parseInputDevicesKbdNode and parseHwKbd located the vm-era persist-kbd device by name, had
    // no production caller left, and a parser kept alive only by its own test is not coverage.
    // Their fixtures went with them.

    // The front-door probe (bd remora-yn4): labeled lines in any order, CRs stripped, unknown
    // labels ignored, and an empty payload — a container that answered nothing — leaves every
    // field unknown rather than down.
    void forwarderProbe() {
        const auto up = parseForwarderProbe(QStringLiteral("fwd_l=1\r\nadbd_l=1\r\nsvc=running\r\n"));
        QVERIFY(up.fwdListening == std::optional<bool>(true));
        QVERIFY(up.adbdListening == std::optional<bool>(true));
        QCOMPARE(up.service.value_or(QString()), QString("running"));

        // The state the bead exists for: adbd healthy, forwarder gone.
        const auto door = parseForwarderProbe(QStringLiteral("fwd_l=0\nadbd_l=1\nsvc=stopped\n"));
        QVERIFY(door.fwdListening == std::optional<bool>(false));
        QVERIFY(door.adbdListening == std::optional<bool>(true));

        // Pre-.rc container: the property line is present but empty — service is KNOWN-unset
        // (netfix's shell supervisor owns fwd), which is not the same as never probed.
        const auto shellOwned = parseForwarderProbe(QStringLiteral("fwd_l=1\nadbd_l=1\nsvc=\n"));
        QVERIFY(shellOwned.service.has_value());
        QVERIFY(shellOwned.service->isEmpty());

        const auto unknown = parseForwarderProbe(QString());
        QVERIFY(!unknown.fwdListening.has_value());
        QVERIFY(!unknown.adbdListening.has_value());
        QVERIFY(!unknown.service.has_value());
    }

    void playCertification() {
        const auto uncertified = parsePlayCertification(QStringLiteral("3804052647508877742\n0\n"));
        QVERIFY(uncertified.androidId.has_value());
        QCOMPARE(*uncertified.androidId, QString("3804052647508877742"));
        QVERIFY(uncertified.certified == std::optional<bool>(false));

        const auto certified = parsePlayCertification(QStringLiteral("3804052647508877742\r\n1\r\n"));
        QVERIFY(certified.certified == std::optional<bool>(true));
    }

    // A device that has never checked in has no id, but the scan still emits the (empty) first
    // line — so the verdict must survive, and the id must read as unknown rather than as "".
    void playCertificationNoCheckinYet() {
        const auto c = parsePlayCertification(QStringLiteral("\n1\n"));
        QVERIFY(!c.androidId.has_value());
        QVERIFY(c.certified == std::optional<bool>(true));
        QVERIFY(!parsePlayCertification(QStringLiteral("0\n0\n")).androidId.has_value());
    }

    // Absence of the uncertified notice has TWO causes — GMS passed the device, or GMS has not
    // looked yet — and the probe used to fold them together: a brand-new never-registered identity
    // read "certified" for a continuously-polled five minutes (bd remora-4ei.90). A young identity
    // downgrades certified-by-absence to PENDING; the sticky posted notice stays authoritative at
    // any age; and payloads without the timestamp lines keep the old reading, because age unknown
    // is not age young.
    void playCertificationPendingWhenIdentityYoung() {
        // Young (5 min) + no notice → pending, and explicitly NOT certified.
        const auto young = parsePlayCertification(
            QStringLiteral("4554079615384372578\n1\n2857179371199824165\n1785771497\n1785771797\n"));
        QVERIFY(young.pendingEvaluation);
        QVERIFY(!young.certified.has_value());
        QCOMPARE(young.identityAgeSec.value_or(-1), qint64(300));
        // The identity pair itself is still read — pending must not cost the snapshot.
        QCOMPARE(young.identityPair(),
                 QStringLiteral("4554079615384372578:2857179371199824165"));

        // Older than one verdict cycle + no notice → certified, as before.
        const auto old = parsePlayCertification(
            QStringLiteral("3648232585666691482\n1\n2914131252766307936\n1785700000\n1785771797\n"));
        QVERIFY(!old.pendingEvaluation);
        QVERIFY(old.certified == std::optional<bool>(true));

        // A POSTED notice is positive evidence whatever the age — young must not soften it.
        const auto posted = parsePlayCertification(
            QStringLiteral("4554079615384372578\n0\n2857179371199824165\n1785771497\n1785771797\n"));
        QVERIFY(!posted.pendingEvaluation);
        QVERIFY(posted.certified == std::optional<bool>(false));

        // No timestamp lines (older scan, or the identity file is absent) → the old reading.
        const auto legacy = parsePlayCertification(QStringLiteral("3648232585666691482\n1\n"));
        QVERIFY(!legacy.pendingEvaluation);
        QVERIFY(legacy.certified == std::optional<bool>(true));
        const auto noMtime = parsePlayCertification(
            QStringLiteral("3648232585666691482\n1\n\n\n1785771797\n"));
        QVERIFY(!noMtime.pendingEvaluation);
        QVERIFY(noMtime.certified == std::optional<bool>(true));
    }

    // Shell noise on line 0 must NOT become an id: it would be shown to the user as the number to
    // paste into Google's registration form. An unreachable container yields no verdict either,
    // which has to stay unknown rather than collapse to "uncertified".
    void playCertificationRejectsNoise() {
        const auto c = parsePlayCertification(
            QStringLiteral("sed: /data/...: No such file or directory\n"));
        QVERIFY(!c.androidId.has_value());
        QVERIFY(!c.certified.has_value());
        QVERIFY(!parsePlayCertification(QString()).certified.has_value());
        QVERIFY(!parsePlayCertification(QStringLiteral("12ab34\n1\n")).androidId.has_value());
    }

    // ONE NAMESPACE (bd remora-28ix.4 step 4): the node is read from remora_gpu_node and nothing
    // else. The fixture is a whole `docker inspect` .Args dump, so the key is found among its
    // neighbours; a container baked under any other spelling reads as having NO baked node, which
    // is the same answer as any container Remora did not bake — the caller probes instead of
    // trusting a stale value.
    void bakedGpuNode() {
        const auto baked = parseBakedGpuNode(fx("docker_inspect_args.txt"));
        QVERIFY(baked.has_value());
        QCOMPARE(*baked, QString("/dev/dri/renderD129"));
        QVERIFY(!parseBakedGpuNode(QStringLiteral("androidboot.legacy_gpu_node=/dev/dri/renderD129\n"
                                                  "androidboot.gpu_node=/dev/dri/renderD129\n"))
                     .has_value());
        auto m = parseBakedGpuNode(
            QStringLiteral("androidboot.remora_gpu_node=/dev/dri/renderD128\n"));
        QVERIFY(m.has_value());
        QCOMPARE(*m, QString("/dev/dri/renderD128"));
    }

    void idGroups() {
        const QString g = fx("id_groups.txt");
        QVERIFY(parseIdGroups(g, "docker"));
        QVERIFY(parseIdGroups(g, "libvirt"));
        QVERIFY(!parseIdGroups(g, "kvm"));
    }

    void lsmodHas() {
        const QString s = "Module Size Used\nashmem_linux 1 0\ntun 1 0\n";
        QVERIFY(parseLsmodHas(s, "ashmem"));
        QVERIFY(parseLsmodHas(s, "tun"));
        QVERIFY(!parseLsmodHas(s, "nonexistent"));
    }

    void dockerPsNames() {
        const auto names = parseDockerPsNames(fx("host_docker_ps.txt"));
        QVERIFY(names.contains("jellyfin"));
        QVERIFY(!names.contains("remora-bm"));
    }

    // bd remora-4u4.1 — the exclusion facts probe. Sample is REAL `docker inspect --format` output
    // captured from a running host, so the shape (leading slash on .Name, trailing '|' from the Go
    // range, ":rw"/":ro" suffixes, multiple ports) is docker's, not an invention of the test.
    void dockerInspectFacts() {
        const QString out =
            "/remora-nvidia\t5555|\t/home/user/.remora-nvidia-data:/data|"
            "/usr/share/remora/vendor/container-scripts/remora-netfix.sh:/remora/netfix.sh:ro|\n"
            "/jellyfin\t7359|8096|\t/srv/docker/compose/jellyfin/cache:/cache:rw|"
            "/mnt/Ark/Multimedia/Videos:/media/videos:ro|\n";
        const auto facts = parseDockerInspectFacts(out);
        QCOMPARE(facts.size(), 2);
        QCOMPARE(facts[0].name, QStringLiteral("remora-nvidia"));  // leading slash stripped
        QVERIFY(facts[0].hostPorts.contains(QStringLiteral("5555")));
        // the bind SOURCE only — matching on the whole "src:dst:opts" would never equal a dataDir
        QVERIFY(facts[0].bindSources.contains(QStringLiteral("/home/user/.remora-nvidia-data")));
        QVERIFY(!facts[0].bindSources.contains(QStringLiteral("/data")));
        QCOMPARE(facts[1].name, QStringLiteral("jellyfin"));
        QCOMPARE(facts[1].hostPorts.size(), 2);  // a container may publish several
        QVERIFY(facts[1].hostPorts.contains(QStringLiteral("8096")));
        QVERIFY(facts[1].bindSources.contains(QStringLiteral("/mnt/Ark/Multimedia/Videos")));
    }

    // A container with no ports and no binds is still a container — it must appear, with empty
    // sets, or a name collision against it goes undetected.
    void dockerInspectFactsKeepsBareContainers() {
        const auto facts = parseDockerInspectFacts(QStringLiteral("/lonely\t\t\n"));
        QCOMPARE(facts.size(), 1);
        QCOMPARE(facts[0].name, QStringLiteral("lonely"));
        QVERIFY(facts[0].hostPorts.isEmpty());
        QVERIFY(facts[0].bindSources.isEmpty());
        QVERIFY(facts[0].devices.isEmpty());  // a 3-field record still parses (bd remora-4u4.6)
    }

    // bd remora-4u4.6 — the devices field. Sample is real output: GPU render nodes and camera
    // nodes arrive in the same list, and only the cameras are treated as exclusive downstream.
    void dockerInspectFactsReadsDevices() {
        const auto facts = parseDockerInspectFacts(QStringLiteral(
            "/remora-nvidia\t5555|\t/home/user/.remora-nvidia-data:/data|"
            "\t/dev/dri/renderD129|/dev/dri/renderD128|/dev/video0|/dev/video1|\n"));
        QCOMPARE(facts.size(), 1);
        QCOMPARE(facts[0].devices.size(), 4);
        QVERIFY(facts[0].devices.contains(QStringLiteral("/dev/video0")));
        QVERIFY(facts[0].devices.contains(QStringLiteral("/dev/dri/renderD129")));
    }

    // Malformed lines are dropped, never turned into a fact-free container that every collision
    // check then passes.
    void dockerInspectFactsDropsMalformed() {
        const auto facts = parseDockerInspectFacts(
            QStringLiteral("garbage-with-no-tabs\n\t\t\n/good\t5555|\t\n"));
        QCOMPARE(facts.size(), 1);
        QCOMPARE(facts[0].name, QStringLiteral("good"));
    }

    // Real /proc/bus/input/devices from the host. Every device in this fixture — two Power
    // Buttons, a consumer-control endpoint and a PC Speaker — sets "kbd" in Handlers, so a
    // handler-based or name-based filter would accept all of them. Only the Keychron is a
    // keyboard, and only the KEY bitmap says so (bd remora-a41).
    void inputDevicesKeyboardsPicksOnlyRealKeyboards() {
        const QList<KbdIdentity> ks = parseInputDevicesKeyboards(fx("proc_input_devices.txt"));
        QCOMPARE(ks.size(), 1);
        QCOMPARE(ks.first().name, QStringLiteral("Keychron K8 Pro Keyboard"));
        QCOMPARE(ks.first().bus, 0x0005u);       // Bluetooth, not USB
        QCOMPARE(ks.first().vendor, 0x3434u);
        QCOMPARE(ks.first().product, 0x0280u);
        QCOMPARE(ks.first().version, 0x0129u);
        QVERIFY(!ks.first().phys.isEmpty());
    }

    void inputDevicesKeyboardsHandlesEmptyAndGarbage() {
        QVERIFY(parseInputDevicesKeyboards(QString()).isEmpty());
        QVERIFY(parseInputDevicesKeyboards(QStringLiteral("not a device dump")).isEmpty());
        // an alphabetic KEY bitmap but no identity line must not produce a half-filled entry
        QVERIFY(parseInputDevicesKeyboards(
                    QStringLiteral("N: Name=\"x\"\nH: Handlers=kbd\nB: KEY=1 0 0 ffffffffffffffff"))
                    .isEmpty());
    }

    // Verbatim from this host's journal for the kill that motivated bd remora-4ei.23. systemd-oomd
    // never says "killed", so the original contains("Killed") check could not have fired on a real
    // kill; keep these strings byte-exact against `journalctl -u systemd-oomd`.
    void oomdKilledMatchesRealJournalWording() {
        const QString marked = QStringLiteral(
            "Jul 25 14:31:34 the dev host systemd-oomd[1332]: Marked "
            "/system.slice/docker-85218894dbf37131575eeb1555f137493d396606214aad2b6bead07a3a55c800"
            ".scope for killing due to memory used (60776927232) / total (66940563456) and swap "
            "used (61069553664) / total (67830013952) being more than 90.00%");
        QVERIFY(parseOomdKilled(marked));
    }

    // The survey line is emitted while oomd is merely looking and nothing has died. Counting it
    // would report "killed by systemd-oomd" on every unrelated exit-137.
    void oomdConsideredIsNotAKill() {
        const QString considered = QStringLiteral(
            "Jul 25 14:31:34 the dev host systemd-oomd[1332]: Considered 139 cgroups for killing, "
            "top candidates were:\n"
            "Jul 25 14:31:34 the dev host systemd-oomd[1332]: Swap Usage: 19.7G");
        QVERIFY(!parseOomdKilled(considered));
        QVERIFY(!parseOomdKilled(QStringLiteral("-- No entries --")));
        QVERIFY(!parseOomdKilled(QString()));
    }

    // Verbatim from the A17 build that died at 1% (bd remora-4ei.30). The container
    // survived — ninja failed, so the OUTER code was 1, not 137 — which is why oomdKillNote cannot
    // see this class at all. The line reads exactly like a metalava failure and is not one: the JVM
    // was SIGKILLed by the cgroup OOM killer against the --memory cap.
    void innerOomKillIsRecognisedInABuildLog() {
        const QString log = QStringLiteral(
            "[ 12% 1234/98765] Metalava executing for frameworks/base/api\n"
            "FAILED: out/soong/.intermediates/frameworks/base/api/test-api-stubs-docs-non-updatable/"
            "android_common/everything/test-api-stubs-docs-non-updatable-stubs.srcjar\n"
            "out/soong/.temp/sbox/xyz/sbox_command.0.bash: line 1: 1777 Killed  "
            "prebuilts/jdk/jdk21/linux-x86/bin/java -jar metalava.jar ... : exit status 137\n"
            "ninja: build stopped: subcommand failed.\n");
        QVERIFY(parseInnerOomKill(log));
    }

    // THE SHAPE THE FIRST CUT MISSED. Verbatim from the build: soong_build was killed by
    // the cgroup OOM killer at anon-rss 29.8 GiB against a 30 GiB cap, and this detector returned
    // FALSE on its own log — because siso reports across four lines and says "exit=137", not
    // "exit status 137". It was written against the single sbox sample recorded on the bead and
    // never checked against the other reporter (bd remora-4ei.30).
    void innerOomKillRecognisesSisoFormat() {
        const QString log = QStringLiteral(
            "cd \"$(dirname \"out/host/linux-x86/bin/soong_build\")\" && BUILDER=...\n"
            "stderr:\n"
            "Killed\n"
            "FAILED: ninja: \n"
            "3m47.15s Build Failure: 1 done, 1 failed, 0 remaining - 0.00/s\n"
            " 1 steps failed: exit=137 # signal:killed\n");
        QVERIFY2(parseInnerOomKill(log), "siso's four-line kill report is not recognised");
        // the token alone is sufficient — 137 WITH an explicit signal:killed is the statement
        QVERIFY(parseInnerOomKill(QStringLiteral(" 1 steps failed: exit=137 # signal:killed\n")));
        // a bare exit=137 counts too, deliberately: requiring "# signal:killed" alongside it is
        // the same over-correlation that made this miss two real kills. 137 is 128+SIGKILL.
        QVERIFY(parseInnerOomKill(QStringLiteral(" 1 steps failed: exit=137\n")));
        // …but a killed signal at a NON-137 status is some other tool's business
        QVERIFY(!parseInnerOomKill(QStringLiteral("exit=1 # signal:killed\n")));
    }

    // THE THIRD SHAPE, and the second time this detector returned FALSE on a real kill. Verbatim
    // from the compile-phase failure: a metalava JVM OOM-killed at 4.0 GiB, with the
    // shell's report and ninja's status SIX lines apart. Requiring them adjacent was reading the
    // bead's condensed quote as if it were the format (bd remora-4ei.30).
    void innerOomKillRecognisesSplitSboxReport() {
        const QString log = QStringLiteral(
            "sbox_command.0.bash: line 1:  1890 Killed   ANDROID_PREFS_ROOT=out/soong/... metalava\n"
            "stderr:\n"
            "The failing command was run inside an sbox sandbox in temporary directory\n"
            "out/soong/.temp/sbox/1f32edf1efdd19db0aa05363a7208b9b2e944554\n"
            "The failing command line can be found in\n"
            "out/soong/.temp/sbox/1f32edf1efdd19db0aa05363a7208b9b2e944554/sbox_command.0.bash\n"
            "exit status 137\n"
            "FAILED: ninja: \n"
            " 1 steps failed: exit=1\n");
        QVERIFY2(parseInnerOomKill(log), "the split sbox kill report is not recognised");
        // each reporter's statement stands alone
        QVERIFY(parseInnerOomKill(QStringLiteral("exit status 137\n")));
        QVERIFY(parseInnerOomKill(QStringLiteral("sh: line 1:  1890 Killed  javac ...\n")));
    }

    // A tree whose filesystem latched read-only mid-build (bd remora-4ei.20). btrfs does this by
    // design after a failed write barrier, and the incident was a SINGLE NVMe flush
    // timeout with wr 0, rd 0, corrupt 0 — not a failing drive. The build reports a generic exit 1
    // and the reason sits in out/verbose.log.gz, so it reads as a build error and invites a retry
    // that cannot possibly work.
    void readOnlyTreeIsRecognised() {
        // what the build itself prints
        QVERIFY(parseReadOnlyTree(QStringLiteral(
            "ninja: error: WriteFile(out/soong/build.ninja): Read-only file system\n")));
        // what the kernel prints, verbatim from the incident
        QVERIFY(parseReadOnlyTree(QStringLiteral(
            "BTRFS: error (device nvme2n1p1) in btrfs_commit_transaction:2412: errno=-5 IO failure\n"
            "BTRFS info (device nvme2n1p1): forced readonly\n")));
        QVERIFY(parseReadOnlyTree(QStringLiteral(
            "BTRFS: Transaction aborted (error -5)\n")));
        QVERIFY(!parseReadOnlyTree(QString()));
    }

    // Must NOT fire on ordinary build text. "read-only" is everywhere in compiler diagnostics, and
    // a false positive here sends someone unmounting a perfectly healthy filesystem.
    void readOnlyTreeIgnoresCompilerDiagnostics() {
        QVERIFY(!parseReadOnlyTree(QStringLiteral(
            "error: cannot assign to variable 'x' with const-qualified type; it is read-only\n")));
        QVERIFY(!parseReadOnlyTree(QStringLiteral("  mounting /data read-only for check\n")));
        QVERIFY(!parseReadOnlyTree(QStringLiteral("note: 'foo' declared readonly here\n")));
    }

    // Bare "Killed" must still NOT count on its own: it is ordinary build text — a test name, a
    // killall in a script — and matching it loosely would report an OOM on unrelated failures,
    // which is the same false-positive trap oomdConsideredIsNotAKill guards on the journal side.
    void innerOomKillIgnoresBareKilled() {
        QVERIFY(!parseInnerOomKill(QStringLiteral(
            "  compiling KillSwitchTest.java\n  Killed process list written\n")));
        // "Killed" without a pid+line prefix and without any 137 is prose, not a kill report
        QVERIFY(!parseInnerOomKill(QStringLiteral("Killed the stale daemon before starting\n")));
        QVERIFY(!parseInnerOomKill(QStringLiteral("exit=1 # signal:killed\n")));
        QVERIFY(!parseInnerOomKill(QString()));
        // Split across lines DOES count — that is the real sbox shape, and asserting otherwise
        // here is what let two genuine kills through (see innerOomKillRecognisesSplitSboxReport).
        QVERIFY(parseInnerOomKill(QStringLiteral("1777 Killed\nsomething: exit status 137\n")));
        QVERIFY(parseInnerOomKill(QStringLiteral("cmd: exit status 137 (Killed)\n")));
    }

    // transfer-apps: the package list. -3 already excludes system apps, so this only has to strip
    // the "package:" prefix — but some builds emit the -f form, and a path handed on as a package
    // name would make every later `pm path` miss.
    // bd remora-4ei.79 — the all-nodes GPU scan. Malformed lines (unreadable driver, junk) must
    // DROP, not become nodes with empty drivers that some equality check then matches.
    void renderNodeScanDropsMalformed() {
        const auto n = parseRenderNodes(
            QStringLiteral("/dev/dri/renderD128 nvidia\n"
                           "/dev/dri/renderD129 xe\n"
                           "not-a-node something\n"
                           "/dev/dri/renderD130\n"));
        QCOMPARE(n.size(), 2);
        QCOMPARE(n.at(0).node, QStringLiteral("/dev/dri/renderD128"));
        QCOMPARE(n.at(0).driver, QStringLiteral("nvidia"));
        QCOMPARE(n.at(1).driver, QStringLiteral("xe"));
        QVERIFY(parseRenderNodes(QString()).isEmpty());
        // The card sibling (bd remora-n4qx): third field when the scan paired it, absent on the
        // pre-n4qx two-field form — an older staged scan on a remote must keep parsing.
        const auto c = parseRenderNodes(
            QStringLiteral("/dev/dri/renderD128 amdgpu /dev/dri/card0\n"
                           "/dev/dri/renderD129 radeon\n"));
        QCOMPARE(c.size(), 2);
        QCOMPARE(c.at(0).card, QStringLiteral("/dev/dri/card0"));
        QVERIFY(c.at(1).card.isEmpty());
    }

    void pmPackagesTakesNamesNotPaths() {
        QCOMPARE(parsePmPackages(QStringLiteral(
                     "package:com.reddit.frontpage\npackage:org.fdroid.fdroid\n")),
                 QStringList({"com.reddit.frontpage", "org.fdroid.fdroid"}));
        // the -f shape: "package:<apk path>=<name>" — the NAME is after the last '='
        QCOMPARE(parsePmPackages(QStringLiteral(
                     "package:/data/app/~~aB==/com.foo-x==/base.apk=com.foo\n")),
                 QStringList({"com.foo"}));
        QVERIFY(parsePmPackages(QStringLiteral("Error: no packages\n")).isEmpty());
        QVERIFY(parsePmPackages(QString()).isEmpty());
    }

    // The one that actually breaks apps if it is wrong. A Play-installed app is usually SPLIT, and
    // installing only base.apk yields an app that launches and then dies on a missing resource —
    // so every path must come back, not just the first.
    void pmPathsReturnsEverySplit() {
        const QStringList p = parsePmPaths(QStringLiteral(
            "package:/data/app/~~x/com.foo-1/base.apk\n"
            "package:/data/app/~~x/com.foo-1/split_config.en.apk\n"
            "package:/data/app/~~x/com.foo-1/split_config.x86_64.apk\n"));
        QCOMPARE(p.size(), 3);
        QVERIFY(p.first().endsWith(QLatin1String("base.apk")));
        QVERIFY(p.contains(QStringLiteral("/data/app/~~x/com.foo-1/split_config.x86_64.apk")));
    }

    // ─────────── arm64 update ownership (bd remora-4ei.91) ───────────

    // The probe's stdout arrives with stderr merged in, so anything that is not "pkg=owner" has
    // to be dropped rather than trusted — an adb banner line becoming a package would badge (and
    // auto-pin) an app that does not exist.
    void armAppOwnersKeepsOnlyAnswerShapedLines() {
        const auto owners = parseArmAppOwners(QStringLiteral(
            "software.sonar.sonar_mobile=com.android.shell\n"
            "com.zhiliaoapp.musically=com.android.vending\n"
            "adb: device offline\n"
            "Error: no package found\n"
            "com.example.noclaim=\n"));
        QCOMPARE(owners.size(), 3);
        QCOMPARE(owners.value(QStringLiteral("software.sonar.sonar_mobile")),
                 QStringLiteral("com.android.shell"));
        QCOMPARE(owners.value(QStringLiteral("com.zhiliaoapp.musically")),
                 QStringLiteral("com.android.vending"));
        QVERIFY(owners.contains(QStringLiteral("com.example.noclaim")));
        QVERIFY(owners.value(QStringLiteral("com.example.noclaim")).isEmpty());
        QVERIFY(parseArmAppOwners(QString()).isEmpty());
    }

    // dumpsys prints the literal "null" for an absent owner on some builds; that must read as
    // "no claim", not as an owner named null — the badge for it is "unclaimed", and a literal
    // string would sail past every equality test in the UI.
    void armAppOwnersNormalisesNull() {
        const auto owners =
            parseArmAppOwners(QStringLiteral("com.example.app=null\n"));
        QVERIFY(owners.contains(QStringLiteral("com.example.app")));
        QVERIFY(owners.value(QStringLiteral("com.example.app")).isEmpty());
    }

    // Only the shell's own claim prevents Play from overwriting an arm64 install. Play owning it,
    // nobody owning it, and whatever a future store might claim all leave the app exposed — the
    // guard can then only repair after the strike.
    void armUpdateOwnerOnlyShellProtects() {
        QVERIFY(armUpdateOwnerProtects(QStringLiteral("com.android.shell")));
        QVERIFY(!armUpdateOwnerProtects(QStringLiteral("com.android.vending")));
        QVERIFY(!armUpdateOwnerProtects(QString()));
        QVERIFY(!armUpdateOwnerProtects(QStringLiteral("org.fdroid.fdroid")));
    }

    // Non-APK noise must be dropped: install-multiple fails the WHOLE transaction on a bad member,
    // so one stray line would take a good app down with it rather than being ignored.
    void pmPathsDropsNonApkLines() {
        QCOMPARE(parsePmPaths(QStringLiteral("package:/data/app/com.foo-1/base.apk\n"
                                             "package:/data/app/com.foo-1/lib\n"
                                             "Failure [NOT_INSTALLED]\n")),
                 QStringList({"/data/app/com.foo-1/base.apk"}));
        QVERIFY(parsePmPaths(QString()).isEmpty());
    }

    // ─────────── host LAN → macvlan parameters (bd remora-400) ───────────

    // docker rejects a --subnet whose host bits are set, so the host's own address must be masked
    // down to its network before it can become one.
    void ipv4NetworkCidrMasksHostBits() {
        QCOMPARE(ipv4NetworkCidr(QStringLiteral("192.168.0.12/24")),
                 QStringLiteral("192.168.0.0/24"));
        QCOMPARE(ipv4NetworkCidr(QStringLiteral("10.4.37.200/16")), QStringLiteral("10.4.0.0/16"));
        // The prefix is READ, never assumed to be /24 — a /22 LAN masked as a /24 would produce a
        // network that silently excludes three quarters of the addresses on it.
        QCOMPARE(ipv4NetworkCidr(QStringLiteral("172.20.5.9/22")), QStringLiteral("172.20.4.0/22"));
        QCOMPARE(ipv4NetworkCidr(QStringLiteral("192.168.0.0/24")),
                 QStringLiteral("192.168.0.0/24"));  // already a network
        for (const char *bad : {"192.168.0.12", "192.168.0.12/33", "192.168.0/24", "not/24",
                                "192.168.0.256/24", "192.168.0.01/24"})
            QVERIFY2(ipv4NetworkCidr(QLatin1String(bad)).isEmpty(), bad);
    }

    void ipv4TopRangeIsTheHighSliceOfTheSubnet() {
        QCOMPARE(ipv4TopRange(QStringLiteral("192.168.0.0/24"), 28),
                 QStringLiteral("192.168.0.240/28"));
        QCOMPARE(ipv4TopRange(QStringLiteral("10.0.0.0/8"), 28), QStringLiteral("10.255.255.240/28"));
        QCOMPARE(ipv4TopRange(QStringLiteral("192.168.0.0/24"), 24),
                 QStringLiteral("192.168.0.0/24"));  // same size = the whole subnet
        // A slice LARGER than the subnet is not a slice; refuse rather than widen the network.
        QVERIFY(ipv4TopRange(QStringLiteral("192.168.0.0/24"), 16).isEmpty());
        QVERIFY(ipv4TopRange(QStringLiteral("nonsense"), 28).isEmpty());
    }

    void ipv4InSubnetChecksMembership() {
        QVERIFY(ipv4InSubnet(QStringLiteral("192.168.0.77"), QStringLiteral("192.168.0.0/24")));
        QVERIFY(ipv4InSubnet(QStringLiteral("192.168.0.240"), QStringLiteral("192.168.0.0/24")));
        QVERIFY(!ipv4InSubnet(QStringLiteral("192.168.1.77"), QStringLiteral("192.168.0.0/24")));
        QVERIFY(!ipv4InSubnet(QStringLiteral("garbage"), QStringLiteral("192.168.0.0/24")));
        QVERIFY(!ipv4InSubnet(QStringLiteral("192.168.0.77"), QStringLiteral("garbage")));
    }

    void ipv4OffsetWalksAddresses() {
        QCOMPARE(ipv4Offset(QStringLiteral("192.168.0.240/28"), -1),
                 QStringLiteral("192.168.0.239"));
        QCOMPARE(ipv4Offset(QStringLiteral("192.168.0.1"), 1), QStringLiteral("192.168.0.2"));
        // Carries across octets rather than wrapping one.
        QCOMPARE(ipv4Offset(QStringLiteral("10.0.1.0/24"), -1), QStringLiteral("10.0.0.255"));
        QVERIFY(ipv4Offset(QStringLiteral("0.0.0.0"), -1).isEmpty());
        QVERIFY(ipv4Offset(QStringLiteral("nonsense"), -1).isEmpty());
    }

    // The real shape of this host's output: two default routes (wired and wifi on one /24) plus
    // docker's own bridges. The metric decides — it is the kernel's own answer to "which of these
    // is primary" — and the address line supplies the prefix the route line does not carry.
    void hostLanPicksLowestMetricDefaultRoute() {
        const QString routes = QStringLiteral(
            "default via 192.168.0.1 dev UMN proto static metric 50 \n"
            "default via 192.168.0.1 dev wlp6s0 proto dhcp src 192.168.0.235 metric 600 \n");
        const QString addrs = QStringLiteral(
            "3: wlp6s0    inet 192.168.0.235/24 brd 192.168.0.255 scope global dynamic wlp6s0\n"
            "6: UMN    inet 192.168.0.12/24 brd 192.168.0.255 scope global noprefixroute UMN\n"
            "10: docker0    inet 172.17.0.1/16 brd 172.17.255.255 scope global docker0\n");
        const auto lan = parseHostLan(routes, addrs);
        QVERIFY(lan.has_value());
        QCOMPARE(lan->parent, QStringLiteral("UMN"));
        QCOMPARE(lan->subnet, QStringLiteral("192.168.0.0/24"));
        QCOMPARE(lan->gateway, QStringLiteral("192.168.0.1"));
        QCOMPARE(lan->hostIp, QStringLiteral("192.168.0.12"));
    }

    // A missing metric is metric 0 — the kernel's own default, which correctly beats an explicit
    // one. Reading it as "unknown, skip" would ignore the primary route on a single-NIC host.
    void hostLanTreatsAbsentMetricAsZero() {
        const auto lan = parseHostLan(
            QStringLiteral("default via 10.0.0.1 dev eno1 \n"
                           "default via 10.0.0.1 dev wlan0 metric 100 \n"),
            QStringLiteral("2: eno1    inet 10.0.0.5/22 brd 10.0.3.255 scope global eno1\n"
                           "3: wlan0    inet 10.0.0.9/22 scope global wlan0\n"));
        QVERIFY(lan.has_value());
        QCOMPARE(lan->parent, QStringLiteral("eno1"));
        QCOMPARE(lan->subnet, QStringLiteral("10.0.0.0/22"));
    }

    // A macvlan parented on a docker bridge is a network that reaches nothing — the LAN is on the
    // other side of it. Never a candidate, even when a compose stack puts a default route there.
    void hostLanNeverParentsOnADockerBridge() {
        QVERIFY(!parseHostLan(QStringLiteral("default via 172.17.0.1 dev docker0 metric 0\n"
                                             "default via 172.19.0.1 dev br-52ee7eea5189 metric 1\n"),
                              QStringLiteral("10: docker0    inet 172.17.0.1/16 scope global docker0\n"))
                     .has_value());
    }

    void hostLanEmptyWhenNothingUsable() {
        QVERIFY(!parseHostLan(QString(), QString()).has_value());
        // A default route whose interface has no global address cannot supply a prefix, and a
        // subnet is not something to guess at.
        QVERIFY(!parseHostLan(QStringLiteral("default via 10.0.0.1 dev eno1 metric 100\n"),
                              QStringLiteral("3: wlan0    inet 10.0.0.9/24 scope global wlan0\n"))
                     .has_value());
        // A route with no gateway (a point-to-point link) yields no gateway to hand docker.
        QVERIFY(!parseHostLan(QStringLiteral("default dev tun0 scope link metric 50\n"),
                              QStringLiteral("4: tun0    inet 10.8.0.2/24 scope global tun0\n"))
                     .has_value());
    }
    // bd remora-d5v. An UNSET or unrecognised backend must mean the LOCAL host. It used to fall
    // through to Vm in both directions, so a profile that had never mentioned a backend silently
    // deployed to a KVM guest over ssh and failed at preflight with "no ssh host set" — which reads
    // as a missing setting rather than as the wrong target having been chosen for it. Local needs
    // no configuration to be reachable and is what someone who did not specify meant.
    void unsetBackendMeansLocalNotTheVmGuest() {
        QCOMPARE(backendFromString(QString()), Backend::Bare);
        QCOMPARE(backendFromString(QStringLiteral("")), Backend::Bare);
        QCOMPARE(backendFromString(QStringLiteral("nonsense")), Backend::Bare);
        for (Backend b : {Backend::Bare, Backend::Remote})
            QCOMPARE(backendFromString(backendToString(b)), b);
    }

    // "vm" is now one of the unrecognised names, so it lands on Bare like the rest — which is why
    // it must ALSO be recognisable as the retired spelling. Without that second signal a profile
    // that still says backend=vm would deploy to the local host without a word, and vm resolved
    // gpu_mode=host, no overlayfs, macvlan and container adb 5555 — the opposite of bare on all
    // four. isRetiredVmBackend is what lets preflight refuse instead (bd remora-d5v).
    void retiredVmBackendIsRecognisedNotSilentlyReinterpreted() {
        QCOMPARE(backendFromString(QStringLiteral("vm")), Backend::Bare);
        QVERIFY(isRetiredVmBackend(QStringLiteral("vm")));
        QVERIFY(isRetiredVmBackend(QStringLiteral("  vm  ")));
        QVERIFY(!isRetiredVmBackend(QStringLiteral("bare")));
        QVERIFY(!isRetiredVmBackend(QStringLiteral("remote")));
        QVERIFY(!isRetiredVmBackend(QString()));
    }

    // gpu_mode has exactly two values. "auto" used to parse into a third mode that nothing
    // handled, so it deployed as guest (SwiftShader) without a word; an unrecognised spelling is
    // now nullopt, for the loader to flag and preflight to refuse — never a silent default.
    void gpuModeParsesOnlyHostAndGuest() {
        for (GpuMode m : {GpuMode::Host, GpuMode::Guest})
            QCOMPARE(gpuModeFromString(gpuModeToString(m)), std::optional<GpuMode>(m));
        QCOMPARE(gpuModeFromString(QStringLiteral(" host ")),
                 std::optional<GpuMode>(GpuMode::Host));
        QVERIFY(!gpuModeFromString(QStringLiteral("auto")).has_value());
        QVERIFY(!gpuModeFromString(QStringLiteral("Host")).has_value());
        QVERIFY(!gpuModeFromString(QString()).has_value());
        const QString msg = unrecognisedGpuModeMessage(QStringLiteral("auto"));
        QVERIFY(msg.contains(QStringLiteral("gpu_mode=auto")));
        QVERIFY(msg.contains(QStringLiteral("gpu_mode=host")));
        QVERIFY(msg.contains(QStringLiteral("gpu_mode=guest")));
    }

    // ─────────── mirror flow (bd remora-j4z) ───────────

    void detachedLogPathComesFromTheCgroupScope() {
        // Real cgroup v2 lines, desktop launch and terminal launch — the slices above the scope
        // differ and neither is ours to depend on.
        QCOMPARE(*parseDetachedLogPath(QStringLiteral(
                     "0::/user.slice/user-1000.slice/user@1000.service/app.slice/"
                     "remora-detached-673632-0.scope\n")),
                 QStringLiteral("/tmp/remora-detached-673632-0.log"));
        QCOMPARE(*parseDetachedLogPath(QStringLiteral(
                     "0::/user.slice/user-1000.slice/user@1000.service/"
                     "remora-detached-552879-11.scope")),
                 QStringLiteral("/tmp/remora-detached-552879-11.log"));
        // Anything not in one of our scopes must not be guessed at — reading a log we do not own
        // would put another process's counters into the verdict.
        QVERIFY(!parseDetachedLogPath(QStringLiteral("0::/user.slice/app.slice/firefox.scope")));
        QVERIFY(!parseDetachedLogPath(QString()));
    }

    void countersTakeTheNEWESTValueNotTheFirst() {
        const QString enc = QStringLiteral(
            "[encoder] listening for the video consumer on /tmp/x.sock\n"
            "[encoder] 1832755 ms: 58440 packets encoded\n"
            "[encoder] 1836755 ms: 58500 packets encoded\n");
        QCOMPARE(*parseEncodedPackets(enc), 58500LL);
        const QString srv = QStringLiteral(
            "[server] INFO: Composed 17760 frames\n"
            "[server] INFO: Composed 17880 frames\n");
        QCOMPARE(*parseComposedFrames(srv), 17880LL);
        // A log that has not printed a counter yet is UNKNOWN, never zero: zero composed frames
        // would read as a still screen and quietly disable the check.
        QVERIFY(!parseEncodedPackets(QStringLiteral("[encoder] listening\n")));
        QVERIFY(!parseComposedFrames(QStringLiteral("[server] INFO: Device: Pixel 8 Pro\n")));
    }

    // The numbers are the ones measured live on both sides of the fault: 57% while juddering,
    // ~100% once a container restart cleared it. A verdict that cannot separate those two is
    // worthless, and one that fires on the healthy case is worse than none.
    void starvationIsJudgedOnTheRatioNotTheRate() {
        const MirrorFlow bad = judgeMirrorFlow(/*composed=*/839, /*encoded=*/479, 15000);
        QVERIFY(bad.starved);
        QCOMPARE(qRound(bad.composedFps), 56L);
        QCOMPARE(qRound(bad.encodedFps), 32L);

        const MirrorFlow good = judgeMirrorFlow(719, 779, 15000);
        QVERIFY(!good.starved);
    }

    void aSlowSourceIsNeverCalledStarved() {
        // THE case this must not get wrong: Pluto's own 30 fps stream, carried perfectly. Same
        // low rate as a starved mirror, ratio 1.0 — the distinction a rate-only check cannot make
        // and the reason this function takes two counters instead of one.
        const MirrorFlow slowButHonest = judgeMirrorFlow(450, 450, 15000);
        QVERIFY(!slowButHonest.starved);
        QCOMPARE(qRound(slowButHonest.composedFps), 30L);

        // A still screen composes nothing; the encoder's keepalive still emits. Ratio is
        // meaningless there, so it must not be judged at all.
        const MirrorFlow still = judgeMirrorFlow(0, 50, 15000);
        QVERIFY(!still.starved);

        // Too short a window and too few frames both quantise badly — the counters print in
        // batches of 60 and 120 — so neither may reach a verdict however bad the ratio looks.
        QVERIFY(!judgeMirrorFlow(200, 20, 1000).starved);
        QVERIFY(!judgeMirrorFlow(20, 2, 15000).starved);
        QVERIFY(!judgeMirrorFlow(500, 100, 0).starved);
        // Right at the granularity boundary: 120 composed is ONE printed line landing inside the
        // window, so the true count could be almost anything up to it. A ratio of 0.17 looks
        // damning and still must not be believed on that little evidence.
        QVERIFY(!judgeMirrorFlow(120, 20, 15000).starved);
        QVERIFY(!judgeMirrorFlow(239, 40, 15000).starved);
        QVERIFY(judgeMirrorFlow(240, 40, 15000).starved);  // two lines' worth: now it counts
    }

    // bd remora-yxy, from a live fault: the mirror flickered because the encoder ran at 18 fps
    // against ~30 composed, and the self-heal never fired. judgeMirrorFlow was never wrong about
    // it — 18/30 = 0.60 is comfortably starved — but the fast-path gate returned "healthy" on the
    // rate alone before the ratio was ever computed, so the verdict was never asked for. THE gate
    // case: it must refuse to answer early here.
    void theGateWillNotWaveThroughAMirrorThatIsMerelyBusy() {
        // 4s gate: 120 encoded = 30 fps, 72 = 18 fps. Both clear the 8 fps floor by miles.
        QVERIFY(!mirrorGateIsClearlyHealthy(/*composed=*/120, /*encoded=*/72, 4000));  // 0.60
        QVERIFY(mirrorGateIsClearlyHealthy(120, 120, 4000));                           // 1.00
        // The boundary is the SAME number the verdict uses; if these two ever drift apart the gate
        // can clear a mirror judgeMirrorFlow would condemn, which is exactly what went wrong.
        QVERIFY(mirrorGateIsClearlyHealthy(120, 90, 4000));   // 0.75, right on it
        QVERIFY(!mirrorGateIsClearlyHealthy(120, 89, 4000));  // a hair under
    }

    void theGateStillFiresWhenItShould() {
        // A still screen composes nothing and the keepalive still emits — no ratio to speak of, and
        // dragging an idle mirror through the full window was the cost the gate exists to avoid.
        QVERIFY(mirrorGateIsClearlyHealthy(0, 60, 4000));
        // Genuine starvation is BELOW the rate floor (3.33 fps keepalive = ~13 packets in 4s), so
        // it never reaches the ratio test — the gate declines and the full window condemns it.
        QVERIFY(!mirrorGateIsClearlyHealthy(120, 13, 4000));
        // A slow SOURCE carried perfectly: 30 fps content, ratio 1.0. Must not be held up.
        QVERIFY(mirrorGateIsClearlyHealthy(120, 120, 4000));
        QVERIFY(!mirrorGateIsClearlyHealthy(120, 120, 0));  // no window, no answer
    }

    // bd remora-c7y, from a live false success: 'Session terminated, killing shell... ...killed.'
    // followed by 'source build OK'. A killed `script` exits 0, so the wrapper's exit code alone
    // cannot judge the run — the chain's own sentinel is the verdict, and its absence means no
    // verdict exists at all.
    void ptyVerdictBelievesTheSentinelNotTheWrapper() {
        // The honest paths are untouched: the chain reported, the sentinel is the answer.
        const PtyExit ok = judgePtyExit(0, QStringLiteral("ninja: done\nREMORA-PTY-RC:0\n"));
        QCOMPARE(ok.rc, 0);
        QVERIFY(!ok.interrupted);
        // pty output ends lines \r\n; the exact code must survive — oomdKillNote keys on 137.
        const PtyExit oom = judgePtyExit(137, QStringLiteral("x\r\nREMORA-PTY-RC:137\r\n"));
        QCOMPARE(oom.rc, 137);
        QVERIFY(!oom.interrupted);
        // Wrapper died AFTER the chain reported — teardown noise, not a failed build.
        const PtyExit late = judgePtyExit(128, QStringLiteral("REMORA-PTY-RC:0\n"));
        QCOMPARE(late.rc, 0);
        QVERIFY(!late.interrupted);

        // THE BUG: the wrapper was killed mid-build, exited 0, and no sentinel ever printed.
        // That must never read as success — and it is "interrupted", not "failed", because the
        // detached builder may well still be compiling.
        const PtyExit killed = judgePtyExit(
            0, QStringLiteral("[ 45% 12000/27000] compile x\n"
                              "Session terminated, killing shell... ...killed.\n"));
        QVERIFY(killed.interrupted);
        QVERIFY(killed.rc != 0);
        // A wrapper that did say nonzero keeps its code — 255 is ssh saying the link dropped.
        const PtyExit dropped = judgePtyExit(255, QStringLiteral("half a build\n"));
        QCOMPARE(dropped.rc, 255);
        QVERIFY(dropped.interrupted);
        // Replayed noise (a catted old log, a shell trace of the echo): the LAST sentinel is by
        // construction the chain's own, and a date is not an exit code.
        const PtyExit replay = judgePtyExit(
            0, QStringLiteral("REMORA-PTY-RC:1\nsecond attempt\nREMORA-PTY-RC:0\n"));
        QCOMPARE(replay.rc, 0);
        QVERIFY(!replay.interrupted);
        // A sentinel-shaped line that is not a sentinel (mid-line, trailing text) must not match
        // AT ALL — a partial read would be a verdict from garbage. No match → interrupted.
        const PtyExit noise = judgePtyExit(7, QStringLiteral("saw REMORA-PTY-RC:2026-08-04 once"));
        QCOMPARE(noise.rc, 7);
        QVERIFY(noise.interrupted);
    }

    // bd remora-4ei.84. The scan now carries the token on line 2, because the identity Google
    // authenticates is the PAIR — an android_id on its own cannot be restored onto a device.
    void playCertificationCarriesTheIdentityPair() {
        const auto full = parsePlayCertification(
            QStringLiteral("3648232585666691482\n1\n2914131252766307936\n"));
        QCOMPARE(*full.androidId, QStringLiteral("3648232585666691482"));
        QCOMPARE(*full.certified, true);
        QCOMPARE(*full.securityToken, QStringLiteral("2914131252766307936"));
        QCOMPARE(full.identityPair(), QStringLiteral("3648232585666691482:2914131252766307936"));

        // The Checkin.xml fallback answers two lines: id known, token not. identityPair() must stay
        // EMPTY there — half a pair written onto a device is the failure this guards.
        const auto idOnly = parsePlayCertification(QStringLiteral("3648232585666691482\n0\n"));
        QCOMPARE(*idOnly.androidId, QStringLiteral("3648232585666691482"));
        QVERIFY(!idOnly.securityToken.has_value());
        QVERIFY(idOnly.identityPair().isEmpty());

        // A malformed or zero token is not a token.
        QVERIFY(!parsePlayCertification(QStringLiteral("123\n1\nnot-a-number\n"))
                     .securityToken.has_value());
        QVERIFY(!parsePlayCertification(QStringLiteral("123\n1\n0\n")).securityToken.has_value());
        // …and a token with no id still yields no pair.
        QVERIFY(parsePlayCertification(QStringLiteral("\n1\n2914131252766307936\n"))
                    .identityPair()
                    .isEmpty());
    }

    // bd remora-4ei.89. The shape word comes back on its own line from the host-side probe.
    void dataDirShapes() {
        QCOMPARE(parseDataDirShape(QStringLiteral("missing\n")), DataDirShape::Missing);
        QCOMPARE(parseDataDirShape(QStringLiteral("empty\n")), DataDirShape::Empty);
        QCOMPARE(parseDataDirShape(QStringLiteral("layered\r\n")), DataDirShape::Layered);
        QCOMPARE(parseDataDirShape(QStringLiteral("flat\n")), DataDirShape::Flat);
        // Silence, noise and an ssh error must all read as Missing. Inferring a shape from a probe
        // that did not answer would turn an unreachable host into a refused deploy, which is a
        // worse failure than the one this check exists to prevent.
        QCOMPARE(parseDataDirShape(QString()), DataDirShape::Missing);
        QCOMPARE(parseDataDirShape(QStringLiteral("ssh: connect to host x port 22: No route\n")),
                 DataDirShape::Missing);
    }

    // The refusal matrix. Both mismatches must fire and BOTH matches must stay silent — a check
    // that refused a legitimate deploy would be worse than the bug it guards.
    void dataShapeRefusals() {
        const QString dir = QStringLiteral("/home/u/.remora-a17");
        const QString base = QStringLiteral("/home/u/.remora-data-base");

        // The failure itself: a flat /data about to be mounted as an overlay diff.
        const QString flatUnderOverlay = dataShapeRefusal(true, DataDirShape::Flat, dir, base);
        QVERIFY(!flatUnderOverlay.isEmpty());
        // The message has to carry the dir, or the user cannot act on it.
        QVERIFY(flatUnderOverlay.contains(dir));
        QVERIFY(flatUnderOverlay.contains(base));
        // …and both ways out, since which one is right depends on what they want to keep.
        QVERIFY(flatUnderOverlay.contains(QStringLiteral("upper")));
        QVERIFY(flatUnderOverlay.contains(QStringLiteral("use_overlayfs=false")));

        // The mirror case: a layered dir about to be bound in flat.
        const QString layeredUnderPlain = dataShapeRefusal(false, DataDirShape::Layered, dir, base);
        QVERIFY(!layeredUnderPlain.isEmpty());
        QVERIFY(layeredUnderPlain.contains(QStringLiteral("use_overlayfs=true")));

        // Matching shapes: silence.
        QVERIFY(dataShapeRefusal(true, DataDirShape::Layered, dir, base).isEmpty());
        QVERIFY(dataShapeRefusal(false, DataDirShape::Flat, dir, base).isEmpty());
        // A first deploy has nothing to protect, under either delivery.
        for (bool overlay : {true, false}) {
            QVERIFY(dataShapeRefusal(overlay, DataDirShape::Missing, dir, base).isEmpty());
            QVERIFY(dataShapeRefusal(overlay, DataDirShape::Empty, dir, base).isEmpty());
        }
        // An unset base still yields an actionable sentence rather than a dangling "over ."
        QVERIFY(!dataShapeRefusal(true, DataDirShape::Flat, dir, QString())
                     .contains(QStringLiteral("over ,")));
    }

    // The arch-variant probe's two lines (bd remora-bm7.13). Order-independent and either-absent,
    // because the two commands are independent and either can produce nothing.
    void archVariantProbeParses() {
        const QString both = QStringLiteral(
            "arch_variant=alderlake\n"
            "flags\t: fpu vme de pse tsc msr sse sse2 avx2 avx_vnni serialize\n");
        QCOMPARE(parseArchVariantProbe(both).variant, QStringLiteral("alderlake"));
        QVERIFY(parseArchVariantProbe(both).flagsLine.contains(QStringLiteral("avx_vnni")));

        // A HOST WITH NO `flags` LINE AT ALL. Not hypothetical: /proc/cpuinfo on ARM has no such
        // line, and an ssh that failed prints nothing. The variant still parses, and the empty
        // flags line is what makes missingArchVariantFlags() decline to judge.
        const ArchVariantProbe noCpu = parseArchVariantProbe(QStringLiteral("arch_variant=x86_64\n"));
        QCOMPARE(noCpu.variant, QStringLiteral("x86_64"));
        QVERIFY(noCpu.flagsLine.isEmpty());

        // An image predating the stamp: `docker image inspect` succeeded and the label is absent.
        // TWO SPELLINGS, and the second is the one that matters — Go's template prints the literal
        // "<no value>" when .Config.Labels is nil (an image with NO labels at all), and taken as a
        // variant NAME it would reach the flag table as an unknown, passing by accident. Normalised
        // to empty here so it passes by rule.
        QVERIFY(parseArchVariantProbe(QStringLiteral("arch_variant=\nflags\t: avx2\n"))
                    .variant.isEmpty());
        const ArchVariantProbe nil =
            parseArchVariantProbe(QStringLiteral("arch_variant=<no value>\nflags\t: avx2\n"));
        QVERIFY(nil.variant.isEmpty());
        QVERIFY(!nil.flagsLine.isEmpty());  // the CPU half still read fine

        // An inspect that failed outright prints no arch_variant line, so the cpuinfo line must not
        // slide into the variant slot — the failure mode a positional section() parse would have.
        const ArchVariantProbe noImage =
            parseArchVariantProbe(QStringLiteral("flags\t: fpu sse2 avx2\n"));
        QVERIFY(noImage.variant.isEmpty());
        QVERIFY(noImage.flagsLine.contains(QStringLiteral("avx2")));

        QVERIFY(parseArchVariantProbe(QString()).variant.isEmpty());
        QVERIFY(parseArchVariantProbe(QString()).flagsLine.isEmpty());
        // CRLF, since the same probe runs over ssh.
        QCOMPARE(parseArchVariantProbe(QStringLiteral("arch_variant=broadwell\r\n")).variant,
                 QStringLiteral("broadwell"));
    }

    // The host encoder's published shape (bd remora-e5x.18.3): the line the encoder writes at
    // <frame socket>.status must round-trip, and the note must speak only when something is
    // actually reduced — a false "degraded" would send the user chasing the note instead of
    // the mirror.
    void encoderStatusParsesAndSpeaksOnlyWhenReduced() {
        // The measured incident, as the encoder now reports it: two halvings of a 4K-ish display.
        const auto degraded = parseEncoderStatus(QStringLiteral(
            "state=degraded native=3760x1992 encode=944x504 bitrate=7500000 degrade=2 pid=42\n"));
        QVERIFY(degraded.has_value());
        QCOMPARE(degraded->state, QStringLiteral("degraded"));
        QCOMPARE(degraded->nativeW, 3760);
        QCOMPARE(degraded->nativeH, 1992);
        QCOMPARE(degraded->encodeW, 944);
        QCOMPARE(degraded->encodeH, 504);
        QCOMPARE(degraded->bitrate, Q_INT64_C(7500000));
        QCOMPARE(degraded->degrade, 2);
        const QString note = encoderDegradationNote(*degraded);
        // Both sizes and the count, or the user cannot judge how bad it is; and the remedy.
        QVERIFY(note.contains(QStringLiteral("944x504")));
        QVERIFY(note.contains(QStringLiteral("3760x1992")));
        QVERIFY(note.contains(QStringLiteral("2 halvings")));
        QVERIFY(note.contains(QStringLiteral("VRAM")));

        // Full size: silence. This is every healthy connect, so a word here is noise.
        const auto ok = parseEncoderStatus(QStringLiteral(
            "state=ok native=3760x1992 encode=3760x1992 bitrate=30000000 degrade=0 pid=42\n"));
        QVERIFY(ok.has_value());
        QVERIFY(encoderDegradationNote(*ok).isEmpty());
        // A line WITHOUT the attach counts is an encoder that predates them: -1 (unknown), which
        // the publisher gate must read as "cannot tell", never as "nobody attached"
        // (bd remora-r6au — the distinction is the whole gate).
        QCOMPARE(ok->publishers, -1);
        QCOMPARE(ok->consumers, -1);

        // The r6au shape itself: up, fresh, bound — and publishing for NOBODY. publishers=0 must
        // round-trip as a real zero so the connect step can refuse to hand a mirror a socket that
        // will never carry a frame.
        const auto orphaned = parseEncoderStatus(QStringLiteral(
            "state=ok native=3760x1992 encode=3760x1992 bitrate=30000000 degrade=0 pid=42 "
            "publishers=0 consumers=1\n"));
        QVERIFY(orphaned.has_value());
        QCOMPARE(orphaned->publishers, 0);
        QCOMPARE(orphaned->consumers, 1);
        QVERIFY(encoderDegradationNote(*orphaned).isEmpty());  // counts are not a degradation

        // black_stream (bd remora-pobz). Absent = -1, the same "cannot tell" the attach counts
        // use: an older encoder must never read as "the stream is fine", because the whole point
        // of the flag is that every OTHER field reads fine while the picture is black.
        QCOMPARE(ok->blackStream, -1);
        const auto black = parseEncoderStatus(QStringLiteral(
            "state=ok native=3760x1992 encode=3760x1992 bitrate=30000000 degrade=0 pid=42 "
            "publishers=1 consumers=1 black_stream=1\n"));
        QVERIFY(black.has_value());
        QCOMPARE(black->blackStream, 1);
        QCOMPARE(black->publishers, 1);  // healthy on every other axis — that IS the shape
        QCOMPARE(black->consumers, 1);
        // Not a degradation: the encoder is doing exactly what it was asked to, at full size.
        // Surfaced by the connect step as its own note instead (Chain.cpp), so this must stay
        // silent here or a black stream would be reported as a resolution problem.
        QVERIFY(encoderDegradationNote(*black).isEmpty());
        const auto notBlack = parseEncoderStatus(QStringLiteral(
            "state=ok native=3760x1992 encode=3760x1992 bitrate=30000000 degrade=0 pid=42 "
            "publishers=1 consumers=1 black_stream=0\n"));
        QCOMPARE(notBlack->blackStream, 0);

        // The two transitional states each carry their own sentence.
        const auto rebuilding = parseEncoderStatus(QStringLiteral(
            "state=rebuilding native=3760x1992 encode=1880x996 bitrate=15000000 degrade=2\n"));
        QVERIFY(encoderDegradationNote(*rebuilding).contains(QStringLiteral("rebuilding")));
        const auto failed = parseEncoderStatus(QStringLiteral(
            "state=failed native=3760x1992 encode=470x248 bitrate=1875000 degrade=3\n"));
        QVERIFY(encoderDegradationNote(*failed).contains(QStringLiteral("GAVE UP")));

        // No file (empty read), noise, and a missing state= are all "no status", never a note.
        QVERIFY(!parseEncoderStatus(QString()).has_value());
        QVERIFY(!parseEncoderStatus(QStringLiteral("cat: no such file\n")).has_value());
        QVERIFY(!parseEncoderStatus(QStringLiteral("native=1x1 encode=1x1\n")).has_value());
        // A state this Remora does not know (a newer encoder): parsed, but no claimed trouble.
        const auto future = parseEncoderStatus(QStringLiteral("state=resizing encode=10x10\n"));
        QVERIFY(future.has_value());
        QVERIFY(encoderDegradationNote(*future).isEmpty());
    }

    // bd remora-81cp. The kernel version comes off the NVRM line ALONE — the file's other lines
    // carry compiler banners with their own dotted numbers (the second line here is verbatim from
    // the host that motivated this, including the linker error). Userspace collects every
    // distinct libnvidia-glcore suffix: a matched multilib pair collapses to one entry, and a
    // HALF-updated multilib must read as a mismatch, not as "first found looks fine".
    void nvidiaVersionScan() {
        const QString scan = QStringLiteral(
            "NVRM version: NVIDIA UNIX Open Kernel Module for x86_64  610.43.03  Release Build  "
            "(portage@localhost)  Mon Aug 10 04:31:19 PM EDT 2026\n"
            "GCC version:  x86_64-pc-linux-gnu-clang-21: error: linker command failed with exit "
            "code 1 (use -v to see invocation)\n"
            "/usr/lib64/libnvidia-glcore.so.610.57.04\n"
            "/usr/lib/libnvidia-glcore.so.610.57.04\n");
        const NvidiaVersions v = parseNvidiaVersionScan(scan);
        QCOMPARE(v.kernel, std::optional<QString>(QStringLiteral("610.43.03")));
        QCOMPARE(v.userspace, QStringList{QStringLiteral("610.57.04")});
        QVERIFY(nvidiaAbiMismatch(v));

        // Matched pair: no mismatch.
        NvidiaVersions ok = v;
        ok.kernel = QStringLiteral("610.57.04");
        QVERIFY(!nvidiaAbiMismatch(ok));

        // Half-updated multilib: lib64 matches the module, lib32 lags — still a mismatch.
        NvidiaVersions half = ok;
        half.userspace << QStringLiteral("610.43.03");
        QVERIFY(nvidiaAbiMismatch(half));

        // Either side unknown declines to judge: no module loaded (non-NVIDIA host, or the scan
        // ran where /proc has no nvidia dir), or no glcore found at the known install locations.
        QVERIFY(!nvidiaAbiMismatch(parseNvidiaVersionScan(QString())));
        QVERIFY(!nvidiaAbiMismatch(
            parseNvidiaVersionScan(QStringLiteral("/usr/lib64/libnvidia-glcore.so.610.57.04\n"))));
        QVERIFY(!nvidiaAbiMismatch(parseNvidiaVersionScan(
            QStringLiteral("NVRM version: NVIDIA UNIX Open Kernel Module for x86_64  610.57.04  "
                           "Release Build\n"))));
    }

};

QTEST_MAIN(TestParsers)
#include "test_parsers.moc"
