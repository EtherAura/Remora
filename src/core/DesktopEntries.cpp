#include "DesktopEntries.h"

#include <QHash>
#include <QSet>

#include <algorithm>

namespace remora::desktop {

QString desktopFileId(const QString &relativePath) {
    QString id = relativePath;
    if (id.endsWith(QLatin1String(".desktop"))) id.chop(8);
    return id.replace(QLatin1Char('/'), QLatin1Char('-'));
}

bool isRemoraLauncher(const QString &id) { return id.startsWith(QLatin1String("remora-")); }

namespace {

// The spec's escapes for string values: \s \n \t \r \\.
QString unescape(const QString &v) {
    QString out;
    out.reserve(v.size());
    for (int i = 0; i < v.size(); ++i) {
        if (v[i] != QLatin1Char('\\') || i + 1 == v.size()) {
            out += v[i];
            continue;
        }
        const QChar c = v[++i];
        if (c == QLatin1Char('s')) out += QLatin1Char(' ');
        else if (c == QLatin1Char('n')) out += QLatin1Char('\n');
        else if (c == QLatin1Char('t')) out += QLatin1Char('\t');
        else if (c == QLatin1Char('r')) out += QLatin1Char('\r');
        else out += c;
    }
    return out;
}

QStringList splitList(const QString &v) {
    QStringList out;
    for (const QString &s : v.split(QLatin1Char(';'), Qt::SkipEmptyParts)) out << s.trimmed();
    return out;
}

bool isTrue(const QString &v) { return v.trimmed() == QLatin1String("true"); }

// The spec's lookup order for "Name[lang_COUNTRY@MODIFIER]": exact, lang_COUNTRY, lang@MODIFIER,
// lang, then the unlocalized key. The encoding part of a POSIX locale (".UTF-8") never matches.
QStringList localeKeys(const QString &key, const QString &locale) {
    QString loc = locale;
    const int dot = loc.indexOf(QLatin1Char('.'));
    const int at = loc.indexOf(QLatin1Char('@'));
    QString modifier;
    if (at >= 0) modifier = loc.mid(at + 1);
    if (dot >= 0) loc = loc.left(dot);
    else if (at >= 0) loc = loc.left(at);
    const QString lang = loc.section(QLatin1Char('_'), 0, 0);
    const QString country = loc.contains(QLatin1Char('_')) ? loc.section(QLatin1Char('_'), 1)
                                                            : QString();
    QStringList keys;
    auto add = [&](const QString &l) { keys << key + QLatin1Char('[') + l + QLatin1Char(']'); };
    if (!country.isEmpty() && !modifier.isEmpty())
        add(lang + QLatin1Char('_') + country + QLatin1Char('@') + modifier);
    if (!country.isEmpty()) add(lang + QLatin1Char('_') + country);
    if (!modifier.isEmpty()) add(lang + QLatin1Char('@') + modifier);
    if (!lang.isEmpty()) add(lang);
    keys << key;
    return keys;
}

// The [Desktop Entry] group's keys; other groups (actions) are not entries.
QHash<QString, QString> mainGroup(const QString &text) {
    QHash<QString, QString> keys;
    bool inMain = false;
    const auto lines = text.split(QLatin1Char('\n'));
    for (QString line : lines) {
        line = line.trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) continue;
        if (line.startsWith(QLatin1Char('['))) {
            if (inMain) break;  // the main group ended
            inMain = line == QLatin1String("[Desktop Entry]");
            continue;
        }
        if (!inMain) continue;
        const int eq = line.indexOf(QLatin1Char('='));
        if (eq <= 0) continue;
        const QString k = line.left(eq).trimmed();
        if (!keys.contains(k)) keys.insert(k, line.mid(eq + 1).trimmed());
    }
    return keys;
}

}  // namespace

std::optional<DesktopEntry> parseDesktopEntry(const QString &id, const QString &text,
                                              const QStringList &currentDesktops,
                                              const QString &locale) {
    const auto keys = mainGroup(text);
    if (keys.value(QStringLiteral("Type")) != QLatin1String("Application")) return std::nullopt;
    if (isTrue(keys.value(QStringLiteral("Hidden"))) ||
        isTrue(keys.value(QStringLiteral("NoDisplay"))))
        return std::nullopt;
    const auto only = splitList(keys.value(QStringLiteral("OnlyShowIn")));
    const auto notIn = splitList(keys.value(QStringLiteral("NotShowIn")));
    if (!only.isEmpty() &&
        std::none_of(only.begin(), only.end(),
                     [&](const QString &d) { return currentDesktops.contains(d); }))
        return std::nullopt;
    if (std::any_of(notIn.begin(), notIn.end(),
                    [&](const QString &d) { return currentDesktops.contains(d); }))
        return std::nullopt;
    if (keys.value(QStringLiteral("Exec")).isEmpty() &&
        !isTrue(keys.value(QStringLiteral("DBusActivatable"))))
        return std::nullopt;

    DesktopEntry e;
    e.id = id;
    for (const QString &k : localeKeys(QStringLiteral("Name"), locale)) {
        if (keys.contains(k)) {
            e.name = unescape(keys.value(k));
            break;
        }
    }
    if (e.name.isEmpty()) return std::nullopt;  // Name is required
    e.icon = unescape(keys.value(QStringLiteral("Icon")));
    e.tryExec = unescape(keys.value(QStringLiteral("TryExec")));
    return e;
}

QList<DesktopEntry> collectApps(const QList<QPair<QString, QString>> &files,
                                const QStringList &currentDesktops, const QString &locale) {
    QSet<QString> decided;
    QList<DesktopEntry> apps;
    for (const auto &[id, text] : files) {
        if (decided.contains(id)) continue;
        decided.insert(id);
        if (isRemoraLauncher(id)) continue;
        if (auto e = parseDesktopEntry(id, text, currentDesktops, locale)) apps << *e;
    }
    std::sort(apps.begin(), apps.end(), [](const DesktopEntry &a, const DesktopEntry &b) {
        const int c = a.name.compare(b.name, Qt::CaseInsensitive);
        return c != 0 ? c < 0 : a.id < b.id;
    });
    return apps;
}

QByteArray listAppsBlob(const QList<DesktopEntry> &apps) {
    QByteArray out;
    for (const DesktopEntry &e : apps) {
        // A name with a line break would split the record; the listing is line-oriented.
        QString name = e.name;
        name.replace(QLatin1Char('\n'), QLatin1Char(' '));
        out += (QStringLiteral(" - ") + name + QStringLiteral("  ") + e.id + QLatin1Char('\n'))
                   .toUtf8();
    }
    return out;
}

}  // namespace remora::desktop
