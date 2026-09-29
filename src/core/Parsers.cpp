#include "core/Parsers.h"

#include <QPair>
#include <QRegularExpression>
#include <QStringList>

#include <limits>

namespace remora {

static const QRegularExpression kGles(QStringLiteral("GLES: [^,]+, [^,]+"));

static QString stripCr(const QString &s) { return QString(s).remove(QLatin1Char('\r')).trimmed(); }

std::optional<bool> parseBootCompleted(const QString &getpropOut) {
    const QString v = stripCr(getpropOut);
    if (v.isEmpty()) return std::nullopt;
    return v == QLatin1String("1");
}

std::optional<QString> parseGralloc(const QString &getpropOut) {
    const QString v = stripCr(getpropOut);
    return v.isEmpty() ? std::optional<QString>{} : std::optional<QString>{v};
}

std::optional<QString> parseSurfaceflingerGles(const QString &dumpsysOut) {
    const auto m = kGles.match(dumpsysOut);
    return m.hasMatch() ? std::optional<QString>{m.captured(0)} : std::nullopt;
}

// The scan always emits its first line, empty or not, so line 1 stays the verdict even when no id
// was found. A non-numeric line 0 is DROPPED rather than passed through: shell noise landing there
// would otherwise be presented to the user as an id to paste into Google's registration form. "0"
// is likewise treated as absent — it is what GSF holds before it has ever checked in.
PlayCertification parsePlayCertification(const QString &scanOut) {
    static const QRegularExpression digits(QStringLiteral("\\A[0-9]{1,25}\\z"));
    PlayCertification c;
    const QString id = stripCr(scanOut.section(QLatin1Char('\n'), 0, 0));
    if (digits.match(id).hasMatch() && id != QLatin1String("0")) c.androidId = id;
    const QString verdict = stripCr(scanOut.section(QLatin1Char('\n'), 1, 1));
    if (verdict == QLatin1String("1")) c.certified = true;
    else if (verdict == QLatin1String("0")) c.certified = false;
    // Line 2 is optional: a scan that predates it, or a device whose authoritative checkin file is
    // absent, simply leaves the token unknown. Validated the same way as the id — the pair is
    // written back onto a device, so a malformed half must never reach it.
    const QString token = stripCr(scanOut.section(QLatin1Char('\n'), 2, 2));
    if (digits.match(token).hasMatch() && token != QLatin1String("0")) c.securityToken = token;
    // Lines 3/4: the identity file's mtime and the device's own clock, same host so no skew. A
    // young identity DOWNGRADES certified-by-absence to pending: absence proves nothing until GMS
    // has had a verdict cycle to look (bd remora-4ei.90). The sticky uncertified verdict is left
    // alone — a posted notice is positive evidence whatever the identity's age. Payloads without
    // the lines (older scans, absent identity file) keep the old reading: age unknowable is not
    // age young, and the device whose file is missing has no freshly-minted identity to distrust.
    bool okM = false, okN = false;
    const qint64 mtime = stripCr(scanOut.section(QLatin1Char('\n'), 3, 3)).toLongLong(&okM);
    const qint64 now = stripCr(scanOut.section(QLatin1Char('\n'), 4, 4)).toLongLong(&okN);
    if (okM && okN && c.certified.value_or(false)) {
        const qint64 age = now - mtime;
        c.identityAgeSec = age;
        if (age >= 0 && age < kPlayVerdictCycleSec) {
            c.certified.reset();
            c.pendingEvaluation = true;
        }
    }
    return c;
}

// Labeled lines, any order, unknown ones ignored — the probe script and this parser can then
// evolve independently in either direction without a lockstep deploy.
ForwarderProbe parseForwarderProbe(const QString &probeOut) {
    ForwarderProbe p;
    for (const QString &line : probeOut.split(QLatin1Char('\n'))) {
        const QString t = stripCr(line);
        if (t.startsWith(QLatin1String("fwd_l=")))
            p.fwdListening = t.mid(6).toInt() > 0;
        else if (t.startsWith(QLatin1String("adbd_l=")))
            p.adbdListening = t.mid(7).toInt() > 0;
        else if (t.startsWith(QLatin1String("svc=")))
            p.service = t.mid(4);
    }
    return p;
}

QString pendingEvaluationNote(qint64 ageSec) {
    const qint64 mins = qMax<qint64>(1, ageSec / 60);
    const QString age = mins < 120 ? QStringLiteral("%1 min").arg(mins)
                                   : QStringLiteral("%1 h").arg(mins / 60);
    return QStringLiteral(
               "not yet evaluated — this identity is %1 old and GMS has not judged it (no verdict "
               "is posted either way; the check-in that evaluates runs on a ~11 h cycle, or at a "
               "container restart). If the id is registered, re-check later or restart the "
               "container; if not, register it now — an unregistered id WILL come back "
               "uncertified.")
        .arg(age);
}

DataDirShape parseDataDirShape(const QString &probeOut) {
    const QString s = stripCr(probeOut.trimmed().section(QLatin1Char('\n'), 0, 0));
    if (s == QLatin1String("layered")) return DataDirShape::Layered;
    if (s == QLatin1String("flat")) return DataDirShape::Flat;
    if (s == QLatin1String("empty")) return DataDirShape::Empty;
    // Anything else — including a probe that could not run at all — is read as Missing, i.e. as
    // "nothing to protect". An unreadable data dir must not block a deploy: the shapes worth
    // refusing are the two the probe states POSITIVELY, and inferring a mismatch from silence would
    // turn every ssh hiccup into a refused deploy.
    return DataDirShape::Missing;
}

QString dataShapeRefusal(bool useOverlayfs, DataDirShape shape, const QString &dataDir,
                         const QString &dataBaseDir) {
    if (shape == DataDirShape::Missing || shape == DataDirShape::Empty) return QString();
    if (useOverlayfs && shape == DataDirShape::Flat)
        return QStringLiteral(
                   "%1 already holds an unlayered /data, but this instance deploys with overlayfs. "
                   "The overlay would mount %1/upper — which does not exist — over %2, and /data "
                   "would come up EMPTY while everything currently in %1 stayed on disk, "
                   "unreachable. Pick one:\n"
                   "  keep the data: mkdir %1/upper && mv %1/<everything-else> %1/upper/  (move the "
                   "existing tree in, so the overlay's upper layer IS your current /data)\n"
                   "  keep the shape: set use_overlayfs=false for this instance, which mounts %1 at "
                   "/data directly, the way it was written")
            .arg(dataDir, dataBaseDir.isEmpty() ? QStringLiteral("the shared base") : dataBaseDir);
    if (!useOverlayfs && shape == DataDirShape::Layered)
        return QStringLiteral(
                   "%1 holds an overlayfs layout (upper/, work/), but this instance deploys with a "
                   "plain bind. Android would get a /data whose entire contents are those two "
                   "directories — an empty device, with the real data one level down. Pick one:\n"
                   "  keep the data: set use_overlayfs=true for this instance, which is how %1 was "
                   "written\n"
                   "  keep the shape: mv %1/upper/* %1/ and remove upper/ and work/")
            .arg(dataDir);
    return QString();
}

ArchVariantProbe parseArchVariantProbe(const QString &probeOut) {
    ArchVariantProbe p;
    for (const QString &raw : probeOut.split(QLatin1Char('\n'))) {
        const QString line = stripCr(raw);
        if (line.startsWith(QLatin1String("arch_variant="))) {
            const QString v = line.mid(sizeof("arch_variant=") - 1).trimmed();
            if (v != QLatin1String("<no value>")) p.variant = v;
        } else if (line.startsWith(QLatin1String("flags"))) {
            p.flagsLine = line;
        }
    }
    return p;
}

// parseInputDevicesKbdNode and parseHwKbd stood here and are gone (bd remora-28ix.4
// step 4). They located a guest uinput keyboard BY DEVICE NAME, defaulting to the vm-era
// persist-kbd's — a device nothing has created since that mechanism left with the vm backend (bd
// remora-gf7 / remora-d5v). No production code called either; only their tests kept them alive,
// and a parser whose only caller is its own test is not coverage, it is embalming. The live
// phantom-keyboard path needs neither: it checks liveness by pidof and names the device through
// REMORA_KBD_NAME.

std::optional<EncoderStatus> parseEncoderStatus(const QString &fileText) {
    // One line of space-separated key=value; unknown keys pass through so a newer encoder can add
    // fields without breaking older readers. state= is the one required key — a file without it
    // (or no file at all, arriving here as empty text) is no status.
    EncoderStatus s;
    const auto wxh = [](const QString &v, int *w, int *h) {
        const int x = v.indexOf(QLatin1Char('x'));
        if (x <= 0) return;
        bool okW = false, okH = false;
        const int pw = v.left(x).toInt(&okW), ph = v.mid(x + 1).toInt(&okH);
        if (okW && okH) *w = pw, *h = ph;
    };
    const QStringList toks =
        fileText.trimmed().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    for (const QString &t : toks) {
        const int eq = t.indexOf(QLatin1Char('='));
        if (eq <= 0) continue;
        const QString k = t.left(eq), v = t.mid(eq + 1);
        if (k == QLatin1String("state")) s.state = v;
        else if (k == QLatin1String("native")) wxh(v, &s.nativeW, &s.nativeH);
        else if (k == QLatin1String("encode")) wxh(v, &s.encodeW, &s.encodeH);
        else if (k == QLatin1String("bitrate")) s.bitrate = v.toLongLong();
        else if (k == QLatin1String("degrade")) s.degrade = v.toInt();
        else if (k == QLatin1String("publishers")) s.publishers = v.toInt();
        else if (k == QLatin1String("consumers")) s.consumers = v.toInt();
        else if (k == QLatin1String("black_stream")) s.blackStream = v.toInt();
    }
    if (s.state.isEmpty()) return std::nullopt;
    return s;
}

QString encoderDegradationNote(const EncoderStatus &s) {
    if (s.state == QLatin1String("failed"))
        return QStringLiteral(
                   "host encoder GAVE UP: could not sustain an encode at or above a quarter of "
                   "%1x%2 — free VRAM and reconnect")
            .arg(s.nativeW)
            .arg(s.nativeH);
    if (s.state == QLatin1String("rebuilding"))
        return QStringLiteral(
                   "host encoder hit a GPU allocation failure at %1x%2 and is rebuilding smaller "
                   "— free VRAM and reconnect for full resolution")
            .arg(s.encodeW)
            .arg(s.encodeH);
    if (s.degrade > 0)
        return QStringLiteral(
                   "host encoder DEGRADED to %1x%2 (%3 halving%4 of %5x%6) after GPU allocation "
                   "failures — free VRAM and reconnect for full resolution")
            .arg(s.encodeW)
            .arg(s.encodeH)
            .arg(s.degrade)
            .arg(s.degrade == 1 ? QString() : QStringLiteral("s"))
            .arg(s.nativeW)
            .arg(s.nativeH);
    // "ok" — and any state this Remora does not know, which a newer encoder may write; claiming
    // trouble it cannot describe would send the user chasing the note instead of the mirror.
    return QString();
}

std::optional<QString> parseBakedGpuNode(const QString &inspectArgs) {
    // ONE NAMESPACE (bd remora-28ix.4 step 4): only remora_gpu_node is read. A container baked
    // under any other spelling reads as having no baked node, which is the same answer as any
    // container Remora did not bake — and the caller already handles that by probing rather than
    // by trusting a stale value.
    static const QRegularExpression re(QStringLiteral("remora_gpu_node=(\\S+)"));
    const auto m = re.match(inspectArgs);
    return m.hasMatch() ? std::optional<QString>{m.captured(1)} : std::nullopt;
}

bool parseIdGroups(const QString &idOut, const QString &group) {
    return idOut.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts).contains(group);
}

bool parseLsmodHas(const QString &lsmodOut, const QString &module) {
    const QStringList lines = lsmodOut.split(QLatin1Char('\n'));
    for (int i = 1; i < lines.size(); ++i) {  // skip the "Module Size Used by" header
        const QStringList parts =
            lines[i].split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        if (!parts.isEmpty() && parts[0].contains(module)) return true;
    }
    return false;
}

QSet<QString> parseDockerPsNames(const QString &psOut) {
    QSet<QString> names;
    for (const QString &ln : psOut.split(QLatin1Char('\n')))
        if (!ln.trimmed().isEmpty()) names.insert(ln.trimmed());
    return names;
}

QVector<ContainerFacts> parseDockerInspectFacts(const QString &out) {
    QVector<ContainerFacts> facts;
    for (const QString &ln : out.split(QLatin1Char('\n'))) {
        if (ln.trimmed().isEmpty()) continue;
        const QStringList f = ln.split(QLatin1Char('\t'));
        if (f.size() < 3) continue;  // malformed → drop, never a fact-free container
        ContainerFacts c;
        // docker inspect renders .Name with a leading slash; every other surface (docker ps,
        // our own containerName) is unslashed, so normalise here or no comparison ever matches.
        c.name = f[0].trimmed();
        if (c.name.startsWith(QLatin1Char('/'))) c.name = c.name.mid(1);
        if (c.name.isEmpty()) continue;
        for (const QString &p : f[1].split(QLatin1Char('|')))
            if (!p.trimmed().isEmpty()) c.hostPorts.insert(p.trimmed());
        for (const QString &b : f[2].split(QLatin1Char('|'))) {
            const QString bind = b.trimmed();
            if (bind.isEmpty()) continue;
            // "src:dst" or "src:dst:ro" — the host side is everything before the FIRST colon.
            const int colon = bind.indexOf(QLatin1Char(':'));
            c.bindSources.insert(colon > 0 ? bind.left(colon) : bind);
        }
        // Devices are optional: a record from before this field existed still parses, it simply
        // reports no devices. Kept lenient so the size>=3 guard above stays the only shape rule.
        if (f.size() > 3)
            for (const QString &d : f[3].split(QLatin1Char('|')))
                if (!d.trimmed().isEmpty()) c.devices.insert(d.trimmed());
        facts.push_back(c);
    }
    return facts;
}

bool parseReadOnlyTree(const QString &buildLog) {
    // Three independent signatures, any one of which is conclusive. The kernel/btrfs ones are the
    // cause; the errno string is what the build itself prints and is what an operator actually sees.
    // "Read-only file system" is errno EROFS's strerror text and is not otherwise emitted by a
    // build — a compiler complaining about a read-only VARIABLE says something else entirely.
    static const QRegularExpression re(
        QStringLiteral("Read-only file system"
                       "|forced readonly"
                       "|BTRFS[^\\r\\n]*Transaction aborted"),
        QRegularExpression::CaseInsensitiveOption);
    return re.match(buildLog).hasMatch();
}


bool parseOomdKilled(const QString &journalOut) {
    // "for killing due to" is the Marked line and only the Marked line — "Considered N cgroups for
    // killing" lacks the "due to". "Killed" is kept for systemd versions that phrase it that way.
    return journalOut.contains(QLatin1String("for killing due to"), Qt::CaseInsensitive)
           || journalOut.contains(QLatin1String("Killed"), Qt::CaseSensitive);
}


bool parseInnerOomKill(const QString &buildLog) {
    // A build action SIGKILLed INSIDE the container. TWO REPORTERS, two shapes — and the first cut
    // of this only knew the first one, so it MISSED the real kill it was written for.
    //
    // sbox, via the shell that ran the action, on one line:
    //   sbox_command.0.bash: line 1: 1777 Killed ... metalava ... exit status 137
    // siso, across four lines, and NOT the words "exit status" (observed, soong_build
    // killed at 29.8 GiB against a 30 GiB cap):
    //   stderr:
    //   Killed
    //   FAILED: ninja:
    //    1 steps failed: exit=137 # signal:killed
    //
    // AND THE REAL sbox OUTPUT SPLITS THEM TOO — observed, which is the second time this
    // returned FALSE on a genuine kill:
    //   sbox_command.0.bash: line 1:  1890 Killed   ANDROID_PREFS_ROOT=... metalava ...
    //   stderr:
    //   The failing command was run inside an sbox sandbox in temporary directory
    //   ...
    //   exit status 137
    // Six lines apart. The bead's recorded sample had them adjacent because it was a condensed
    // quote, and requiring adjacency was reading a quote as a format.
    //
    // So stop trying to correlate two tokens and just recognise each reporter's own statement.
    // 137 is 128+SIGKILL: a build tool choosing that as a deliberate status is not a thing that
    // happens, and every alternative below is specific enough to stand alone —
    //   "exit status 137"          ninja/soong reporting a killed action
    //   "exit=137"                 siso reporting the same
    //   "line <n>: <pid> Killed"   the shell's own killed-job report, which no prose reproduces
    // Bare "Killed" is still NOT matched on its own: it is ordinary build text (a test name, a
    // killall in a script), and matching it loosely would report an OOM on unrelated failures.
    static const QRegularExpression re(
        QStringLiteral("\\bexit status 137\\b"
                       "|\\bexit=137\\b"
                       "|line \\d+:\\s+\\d+ Killed\\b"));
    return re.match(buildLog).hasMatch();
}


QList<KbdIdentity> parseInputDevices(const QString &procInput) {
    // KEY bitmap words run high-order first, so the last word holds bits 0-63 — where every letter
    // key lives. Require Q(16), A(30), Z(44) and M(50): a full alphabetic row set, which a power
    // button or a consumer-control endpoint never has.
    const auto isAlphabetic = [](const QString &keyLine) {
        const QStringList words = keyLine.trimmed().split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (words.isEmpty()) return false;
        bool ok = false;
        const quint64 low = words.last().toULongLong(&ok, 16);
        if (!ok) return false;
        for (int bit : {16, 30, 44, 50})
            if (!(low & (Q_UINT64_C(1) << bit))) return false;
        return true;
    };
    static const QRegularExpression idRe(QStringLiteral(
        "Bus=([0-9a-fA-F]+)\\s+Vendor=([0-9a-fA-F]+)\\s+Product=([0-9a-fA-F]+)\\s+Version=([0-9a-fA-F]+)"));
    static const QRegularExpression nameRe(QStringLiteral("Name=\"([^\"]*)\""));

    QList<KbdIdentity> out;
    const auto flush = [&](const QStringList &blk) {
        if (blk.isEmpty()) return;
        QString keyLine;
        KbdIdentity k;
        bool haveKbd = false, haveId = false;
        for (const QString &line : blk) {
            if (line.startsWith(QLatin1String("I:"))) {
                const auto m = idRe.match(line);
                if (m.hasMatch()) {
                    k.bus = m.captured(1).toUInt(nullptr, 16);
                    k.vendor = m.captured(2).toUInt(nullptr, 16);
                    k.product = m.captured(3).toUInt(nullptr, 16);
                    k.version = m.captured(4).toUInt(nullptr, 16);
                    haveId = true;
                }
            } else if (line.startsWith(QLatin1String("N:"))) {
                const auto m = nameRe.match(line);
                if (m.hasMatch()) k.name = m.captured(1);
            } else if (line.startsWith(QLatin1String("P:"))) {
                k.phys = line.section(QLatin1Char('='), 1).trimmed();
            } else if (line.startsWith(QLatin1String("S:"))) {
                k.sysfs = line.section(QLatin1Char('='), 1).trimmed();
            } else if (line.startsWith(QLatin1String("H:"))) {
                haveKbd = line.contains(QLatin1String("kbd"));
                static const QRegularExpression evRe(QStringLiteral("\\b(event\\d+)"));
                const auto m = evRe.match(line);
                if (m.hasMatch()) k.node = m.captured(1);
            } else if (line.contains(QLatin1String("KEY="))) {
                keyLine = line.section(QLatin1String("KEY="), 1);
            } else if (line.startsWith(QLatin1String("B: REL="))) {
                k.hasRel = true;
            } else if (line.startsWith(QLatin1String("B: ABS="))) {
                k.hasAbs = true;
            }
        }
        k.alphabetic = haveKbd && isAlphabetic(keyLine);
        if (haveId && !k.name.isEmpty() && !k.node.isEmpty()) out << k;
    };

    QStringList block;
    for (const QString &line : procInput.split(QLatin1Char('\n'))) {
        if (line.trimmed().isEmpty()) {
            flush(block);
            block.clear();
        } else {
            block << line;
        }
    }
    flush(block);
    return out;
}

QList<KbdIdentity> parseInputDevicesKeyboards(const QString &procInput) {
    QList<KbdIdentity> out;
    for (const KbdIdentity &k : parseInputDevices(procInput))
        if (k.alphabetic) out << k;
    return out;
}

QList<RenderNode> parseRenderNodes(const QString &out) {
    QList<RenderNode> nodes;
    for (const QString &raw : out.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        const QStringList parts = raw.trimmed().split(QLatin1Char(' '), Qt::SkipEmptyParts);
        // Two fields (node driver) is the pre-n4qx scan and stays valid — a remote host running
        // an older staged scan, or a fixture, parses as before with no card sibling known.
        if (parts.size() < 2 || parts.size() > 3 ||
            !parts.at(0).startsWith(QLatin1String("/dev/dri/")))
            continue;
        nodes << RenderNode{parts.at(0), parts.at(1),
                            parts.size() == 3 ? parts.at(2) : QString()};
    }
    return nodes;
}

// ─────────── host LAN (macvlan parameters) ───────────

// A dotted quad as a 32-bit host-order integer. nullopt on anything else — deliberately strict,
// because every caller below feeds its result to `docker network create`, where a silently
// mis-parsed octet becomes a network on the wrong subnet rather than an error.
static std::optional<quint32> parseIpv4(const QString &s) {
    const QStringList o = s.trimmed().split(QLatin1Char('.'));
    if (o.size() != 4) return std::nullopt;
    quint32 v = 0;
    for (const QString &part : o) {
        bool ok = false;
        const uint n = part.toUInt(&ok);
        // Reject "01" and "" as well as out-of-range: toUInt accepts leading zeros, and a
        // zero-padded octet in a config file means the author was thinking of some other notation.
        if (!ok || n > 255 || part.isEmpty() || (part.size() > 1 && part.startsWith(QLatin1Char('0'))))
            return std::nullopt;
        v = (v << 8) | n;
    }
    return v;
}

static QString formatIpv4(quint32 v) {
    return QStringLiteral("%1.%2.%3.%4")
        .arg((v >> 24) & 0xff)
        .arg((v >> 16) & 0xff)
        .arg((v >> 8) & 0xff)
        .arg(v & 0xff);
}

// "a.b.c.d/N" → (address, N). Both halves must be present and valid.
static std::optional<QPair<quint32, int>> parseCidr(const QString &cidr) {
    const QStringList parts = cidr.trimmed().split(QLatin1Char('/'));
    if (parts.size() != 2) return std::nullopt;
    const auto addr = parseIpv4(parts.at(0));
    bool ok = false;
    const int prefix = parts.at(1).toInt(&ok);
    if (!addr || !ok || prefix < 0 || prefix > 32) return std::nullopt;
    return QPair<quint32, int>{*addr, prefix};
}

static quint32 maskFor(int prefix) {
    return prefix == 0 ? 0u : (0xffffffffu << (32 - prefix));
}

QString ipv4NetworkCidr(const QString &addrCidr) {
    const auto c = parseCidr(addrCidr);
    if (!c) return {};
    return QStringLiteral("%1/%2").arg(formatIpv4(c->first & maskFor(c->second))).arg(c->second);
}

QString ipv4TopRange(const QString &subnetCidr, int prefixLen) {
    const auto c = parseCidr(subnetCidr);
    if (!c || prefixLen < c->second || prefixLen > 32) return {};
    const quint32 net = c->first & maskFor(c->second);
    // The last block of `prefixLen` size inside the subnet: broadcast address masked down.
    const quint32 last = net | ~maskFor(c->second);
    return QStringLiteral("%1/%2").arg(formatIpv4(last & maskFor(prefixLen))).arg(prefixLen);
}

bool ipv4InSubnet(const QString &ip, const QString &cidr) {
    const auto a = parseIpv4(ip);
    const auto c = parseCidr(cidr);
    if (!a || !c) return false;
    return (*a & maskFor(c->second)) == (c->first & maskFor(c->second));
}

QString ipv4Offset(const QString &ipOrCidr, int delta) {
    const QString bare = ipOrCidr.section(QLatin1Char('/'), 0, 0);
    const auto a = parseIpv4(bare);
    if (!a) return {};
    const qint64 v = qint64(*a) + delta;
    if (v < 0 || v > 0xffffffffLL) return {};
    return formatIpv4(quint32(v));
}

std::optional<LanInterface> parseHostLan(const QString &routeOut, const QString &addrOut) {
    // Pick the default route with the lowest metric. A missing metric reads as 0 (the kernel's
    // own default), which correctly beats an explicit one.
    QString nic, gateway;
    int bestMetric = std::numeric_limits<int>::max();
    for (const QString &raw : routeOut.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        const QStringList f = raw.trimmed().split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (f.isEmpty() || f.at(0) != QLatin1String("default")) continue;
        QString via, dev;
        int metric = 0;
        for (int i = 1; i + 1 < f.size(); ++i) {
            if (f.at(i) == QLatin1String("via")) via = f.at(i + 1);
            else if (f.at(i) == QLatin1String("dev")) dev = f.at(i + 1);
            else if (f.at(i) == QLatin1String("metric")) metric = f.at(i + 1).toInt();
        }
        // docker's own bridges carry no default route in practice, but a compose stack with
        // gateway containers can put one there — never parent a macvlan on a docker bridge.
        if (dev.isEmpty() || !parseIpv4(via) || dev.startsWith(QLatin1String("docker")) ||
            dev.startsWith(QLatin1String("br-")))
            continue;
        if (metric < bestMetric) {
            bestMetric = metric;
            nic = dev;
            gateway = via;
        }
    }
    if (nic.isEmpty()) return std::nullopt;

    // `ip -o -4 addr show`: "3: UMN    inet 192.168.0.12/24 brd … scope global …". Take the first
    // global-scope address on the chosen NIC — a secondary alias must not displace the primary.
    for (const QString &raw : addrOut.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        const QStringList f = raw.trimmed().split(QLatin1Char(' '), Qt::SkipEmptyParts);
        const int dev = f.indexOf(nic);
        const int inet = f.indexOf(QLatin1String("inet"));
        if (dev < 0 || inet < 0 || inet + 1 >= f.size()) continue;
        const QString subnet = ipv4NetworkCidr(f.at(inet + 1));
        if (subnet.isEmpty()) continue;
        return LanInterface{nic, subnet, gateway, f.at(inet + 1).section(QLatin1Char('/'), 0, 0)};
    }
    return std::nullopt;
}

QStringList parsePmPackages(const QString &out) {
    QStringList pkgs;
    for (const QString &raw : out.split(QLatin1Char('\n'))) {
        const QString line = raw.trimmed();
        if (!line.startsWith(QLatin1String("package:"))) continue;
        // "package:com.foo" — and on some builds "package:/path/base.apk=com.foo" when -f slipped
        // in, so take what follows a '=' when there is one.
        QString p = line.mid(8).trimmed();
        const int eq = p.lastIndexOf(QLatin1Char('='));
        if (eq >= 0) p = p.mid(eq + 1).trimmed();
        if (!p.isEmpty()) pkgs << p;
    }
    pkgs.removeDuplicates();
    return pkgs;
}

QHash<QString, QString> parseArmAppOwners(const QString &out) {
    // "pkg=owner" per line, both sides package-name-shaped, the owner optionally absent. Package
    // names cannot contain '=', so the FIRST '=' splits unambiguously.
    static const QRegularExpression kLine(QStringLiteral("^([A-Za-z0-9_.]+)=([A-Za-z0-9_.]*)$"));
    QHash<QString, QString> owners;
    for (const QString &raw : out.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        const auto m = kLine.match(raw.trimmed());
        if (!m.hasMatch()) continue;
        const QString owner = m.captured(2);
        owners.insert(m.captured(1), owner == QLatin1String("null") ? QString() : owner);
    }
    return owners;
}

bool armUpdateOwnerProtects(const QString &owner) {
    return owner == QLatin1String("com.android.shell");
}

QStringList parsePmPaths(const QString &out) {
    QStringList paths;
    for (const QString &raw : out.split(QLatin1Char('\n'))) {
        const QString line = raw.trimmed();
        if (!line.startsWith(QLatin1String("package:"))) continue;
        const QString p = line.mid(8).trimmed();
        // Only real APK paths: pm also emits diagnostic lines on some builds, and a non-.apk entry
        // handed to install-multiple fails the whole transaction rather than being ignored.
        if (p.endsWith(QLatin1String(".apk"))) paths << p;
    }
    paths.removeDuplicates();
    return paths;
}

std::optional<QString> parseDetachedLogPath(const QString &procCgroup) {
    // cgroup v2 lines are "0::/user.slice/…/remora-detached-673632-0.scope". Match the unit name
    // rather than the whole path: the slices above it differ between a desktop launch and a
    // terminal one, and neither shape is ours to depend on.
    static const QRegularExpression kScope(
        QStringLiteral("(remora-detached-[0-9]+-[0-9]+)\\.scope"));
    const QRegularExpressionMatch m = kScope.match(procCgroup);
    if (!m.hasMatch()) return std::nullopt;
    return QStringLiteral("/tmp/%1.log").arg(m.captured(1));
}

// Both counters are cumulative and printed periodically, so the LAST match is the current value.
static std::optional<qint64> lastCounter(const QString &logText, const QRegularExpression &re) {
    std::optional<qint64> found;
    for (const QString &line : logText.split(QLatin1Char('\n'))) {
        const QRegularExpressionMatch m = re.match(line);
        if (!m.hasMatch()) continue;
        bool ok = false;
        const qint64 v = m.captured(1).toLongLong(&ok);
        if (ok) found = v;
    }
    return found;
}

std::optional<qint64> parseComposedFrames(const QString &logText) {
    static const QRegularExpression kRe(QStringLiteral("Composed ([0-9]+) frames"));
    return lastCounter(logText, kRe);
}

std::optional<qint64> parseEncodedPackets(const QString &logText) {
    static const QRegularExpression kRe(QStringLiteral("([0-9]+) packets encoded"));
    return lastCounter(logText, kRe);
}

MirrorFlow judgeMirrorFlow(qint64 composedDelta, qint64 encodedDelta, int windowMs) {
    MirrorFlow f;
    if (windowMs <= 0) return f;
    const double secs = windowMs / 1000.0;
    f.composedFps = double(qMax<qint64>(composedDelta, 0)) / secs;
    f.encodedFps = double(qMax<qint64>(encodedDelta, 0)) / secs;
    // No frames composed means nothing to judge — a still screen is not a starved one, and the
    // encoder's keepalive legitimately outruns a compositor that has stopped submitting.
    if (composedDelta <= 0) return f;
    f.ratio = f.encodedFps / f.composedFps;
    // Three guards, all of which must fail before this cries wolf. The window has to be long
    // enough for the counters to move meaningfully (both sides print in batches, so a short
    // sample quantises badly); enough frames have to have been composed for the ratio to mean
    // anything; and the shortfall has to be large. The healthy case measured ~1.0 and the broken
    // one 0.57, so 0.75 sits well clear of both — close enough to catch the failure, far enough
    // that ordinary sampling skew never reaches it.
    // kMinComposed is set by the PRINT GRANULARITY, not by taste: the guest server reports once
    // per 120 composed frames, so a delta of 120 is a single line landing inside the window and
    // can over-read the true count by nearly all of itself. Demanding several lines' worth keeps
    // that error a fraction rather than a doubling. Below it the honest answer is "cannot tell",
    // which is what a slow window gets.
    constexpr int kMinWindowMs = 5000;
    constexpr qint64 kMinComposed = 240;
    f.starved = windowMs >= kMinWindowMs && composedDelta >= kMinComposed
                && f.ratio < kMirrorStarvedRatio;
    return f;
}

bool mirrorGateIsClearlyHealthy(qint64 composedDelta, qint64 encodedDelta, int gateMs) {
    if (gateMs <= 0) return false;
    const double encodedFps = double(qMax<qint64>(encodedDelta, 0)) / (gateMs / 1000.0);
    // Above this an encoder is carrying real frames rather than its keepalive: starvation caps it
    // at the 300 ms keepalive, i.e. 3.33 fps, and the original fault measured 3.2. Eight leaves
    // that 2.5x of margin without straying into the rates real content produces.
    constexpr double kClearlyReceivingFps = 8.0;
    if (encodedFps <= kClearlyReceivingFps) return false;
    // NECESSARY BUT NOT SUFFICIENT, and treating it as sufficient was the bug. "Carrying real
    // frames" is not "keeping up": a mirror composing 30 and encoding 18 clears the floor twice
    // over and is still losing 40% of its frames, which the user sees as flicker. The floor is
    // absolute, the health question is relative, so ask both.
    const qint64 composed = qMax<qint64>(composedDelta, 0);
    // Nothing composed is not a shortfall — a still screen composes nothing while the encoder
    // legitimately keeps emitting, the same case judgeMirrorFlow declines to judge. Let the fast
    // path fire so an idle mirror is not dragged through the full window.
    if (composed <= 0) return true;
    return double(qMax<qint64>(encodedDelta, 0)) / double(composed) >= kMirrorStarvedRatio;
}

PtyExit judgePtyExit(int wrapperRc, const QString &outputTail) {
    // Built from the shared constant so the wrap (ptyRunArgv) and the parse cannot drift. \d{1,3}
    // because $? is 0–255, and anchored to the line end (\r? because a pty emits \r\n): a
    // pathological line like "REMORA-PTY-RC:2026-08-04 …" must not match AT ALL — a partial read
    // of it would be a verdict from garbage, and no match falls through to interrupted, which is
    // the safe direction.
    static const QRegularExpression re(QString::fromLatin1(kPtyExitSentinel)
                                           + QStringLiteral("(\\d{1,3})\\r?$"),
                                       QRegularExpression::MultilineOption);
    std::optional<int> reported;
    QRegularExpressionMatchIterator it = re.globalMatch(outputTail);
    // Last one wins: anything earlier is replayed noise (a catted log, a shell trace of the echo),
    // and the genuine sentinel is by construction the final thing the chain prints.
    while (it.hasNext()) reported = it.next().captured(1).toInt();
    if (reported) return {*reported, false};
    // No sentinel: the wrapper closed before the chain could report. Its exit code is worth
    // keeping when nonzero (255 says "ssh dropped"), but a zero is exactly the lie this exists to
    // catch — replace it with 128+SIGTERM, the shape of "something killed the wrapper".
    return {wrapperRc != 0 ? wrapperRc : 143, true};
}

NvidiaVersions parseNvidiaVersionScan(const QString &scan) {
    NvidiaVersions v;
    // Only the NVRM line, never a whole-file version grep: /proc/driver/nvidia/version's other
    // lines carry compiler banners with their own dotted numbers (a real one on this project's
    // host reads "GCC version: ... clang-21: error: ...").
    static const QRegularExpression kernelRe(
        QStringLiteral("^NVRM version:.*?(\\d+\\.\\d+(?:\\.\\d+)*)"),
        QRegularExpression::MultilineOption);
    if (const auto m = kernelRe.match(scan); m.hasMatch()) v.kernel = m.captured(1);
    // The ls half: one path per line, version = everything after ".so.". Distinct values only,
    // but ALL of them — a half-updated multilib (lib64 new, lib32 old) is as broken as a stale
    // pair, and collapsing to "first found" would hide it.
    static const QRegularExpression userRe(
        QStringLiteral("/libnvidia-glcore\\.so\\.(\\d+\\.\\d+(?:\\.\\d+)*)\\s*$"),
        QRegularExpression::MultilineOption);
    QRegularExpressionMatchIterator it = userRe.globalMatch(scan);
    while (it.hasNext()) {
        const QString ver = it.next().captured(1);
        if (!v.userspace.contains(ver)) v.userspace << ver;
    }
    return v;
}

bool nvidiaAbiMismatch(const NvidiaVersions &v) {
    if (!v.kernel || v.userspace.isEmpty()) return false;  // nothing to compare
    for (const QString &u : v.userspace)
        if (u != *v.kernel) return true;
    return false;
}

}  // namespace remora
