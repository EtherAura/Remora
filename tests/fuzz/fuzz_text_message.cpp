// libFuzzer harness for textMessage (bd remora-fdcv): the client→agent TEXT control record, which
// carries whatever text the user types or pastes. An encode→parse round trip: the record is taken
// apart again by the documented wire format and by RecordReader at the control limit, and its body
// must be the text's UTF-8, or the longest prefix of it that fits, cut on a codepoint boundary.
//
// The first input byte picks how the rest becomes the QString handed to textMessage:
//   bit 7 clear  the rest is the text, through QString::fromUtf8 (invalid bytes become U+FFFD,
//                exactly as for any QString built from bytes);
//   bit 7 set    the rest is a prefix length byte P, P bytes of prefix, then a unit; the text is
//                prefix + unit repeated until its UTF-8 reaches (limit - 64 + low 7 bits) bytes,
//                where limit is the 1 MiB - 1 a body may hold. This reaches the truncation
//                boundary, which no input of libFuzzer's default length could, within a bounded
//                ~1 MiB per run.
#include "core/MirrorProto.h"

#include <QtEndian>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

using remora::mirror::ControlRecord;
using remora::mirror::kControlRecordMax;
using remora::mirror::RecordReader;
using remora::mirror::textMessage;

namespace {

constexpr qsizetype kBodyMax = qsizetype(kControlRecordMax) - 1;

void check(bool ok) {
    if (!ok) std::abort();
}

bool continuation(char c) { return (uchar(c) & 0xC0) == 0x80; }

QString textFrom(const uint8_t *data, size_t size) {
    const char *rest = reinterpret_cast<const char *>(data + 1);
    const qsizetype n = qsizetype(size) - 1;
    if (!(data[0] & 0x80)) return QString::fromUtf8(rest, n);
    if (n < 1) return {};
    const qsizetype p = qMin(qsizetype(uchar(rest[0])), n - 1);
    QString text = QString::fromUtf8(rest + 1, p);
    const QString unit = QString::fromUtf8(rest + 1 + p, n - 1 - p);
    const qsizetype unitLen = unit.toUtf8().size();
    const qsizetype target = kBodyMax - 64 + (data[0] & 0x7F);
    const qsizetype have = text.toUtf8().size();
    if (unitLen > 0 && have < target) text += unit.repeated((target - have + unitLen - 1) / unitLen);
    return text;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 1) return 0;
    const QString text = textFrom(data, size);
    const QByteArray utf8 = text.toUtf8();
    const QByteArray enc = textMessage(text);

    // The header by hand: u32 big-endian size (type byte + body), the TEXT type, the body.
    check(enc.size() >= 5);
    const quint32 declared = qFromBigEndian<quint32>(enc.constData());
    check(declared >= 1 && declared <= kControlRecordMax);
    check(qsizetype(declared) == enc.size() - 4);
    check(quint8(enc.at(4)) == quint8(ControlRecord::Text));
    const QByteArray body = enc.mid(5);

    // And by the client's own record reader at the control limit: one record, nothing left over.
    RecordReader reader(kControlRecordMax);
    reader.feed(enc);
    const auto rec = reader.next();
    check(rec.has_value() && !reader.error());
    check(rec->type == quint8(ControlRecord::Text) && rec->body == body);
    check(!reader.next().has_value() && !reader.error());

    if (utf8.size() <= kBodyMax) {
        // Fits: goes over unchanged, and decodes back to the text it came from.
        check(body == utf8);
        check(QString::fromUtf8(body) == text);
    } else {
        // Over: the longest prefix of at most kBodyMax bytes that ends on a codepoint boundary.
        const qsizetype cut = body.size();
        check(cut <= kBodyMax && cut > kBodyMax - 4);
        check(utf8.startsWith(body));
        check(!continuation(utf8.at(cut)));
        for (qsizetype i = cut + 1; i <= kBodyMax; ++i) check(continuation(utf8.at(i)));
        check(text.startsWith(QString::fromUtf8(body)));
    }
    // Either way the body is valid UTF-8, and re-encoding what the agent decodes is the same record.
    const QString decoded = QString::fromUtf8(body);
    check(decoded.toUtf8() == body);
    check(textMessage(decoded) == enc);
    return 0;
}
