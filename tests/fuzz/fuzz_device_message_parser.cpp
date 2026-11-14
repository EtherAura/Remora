// libFuzzer harness for DeviceMessageParser (bd remora-fdcv): the control connection's
// agent→client direction, which parses bytes the guest controls (MirrorSession.cpp).
//
// The whole input is the wire stream, so the seeds are the pin tests' own vectors byte for byte.
// The stream is fed in chunks whose lengths are taken from the stream itself, the parser drained
// after every chunk, and the result held against one parser fed everything at once: the parser is
// incremental, so where the chunk boundaries fall must not change what it yields.
#include "core/MirrorProto.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

using remora::mirror::DeviceMessage;
using remora::mirror::DeviceMessageParser;
using remora::mirror::DeviceRecord;

namespace {

void check(bool ok) {
    if (!ok) std::abort();
}

bool same(const DeviceMessage &a, const DeviceMessage &b) {
    return a.type == b.type && a.clipboardText == b.clipboardText && a.sequence == b.sequence;
}

std::vector<DeviceMessage> drain(DeviceMessageParser &p, std::vector<DeviceMessage> out = {}) {
    while (auto m = p.next()) {
        check(!p.error());  // a latched parser yields nothing
        check(m->type == DeviceRecord::Clipboard || m->type == DeviceRecord::ClipboardAck);
        if (m->type == DeviceRecord::ClipboardAck) check(m->clipboardText.isEmpty());
        out.push_back(std::move(*m));
    }
    return out;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    const QByteArray stream(reinterpret_cast<const char *>(data), qsizetype(size));

    DeviceMessageParser whole;
    whole.feed(stream);
    const std::vector<DeviceMessage> expected = drain(whole);
    // Every message is a whole record: a u32 size, a type byte, a body.
    check(expected.size() <= size / 5);

    DeviceMessageParser chunked;
    std::vector<DeviceMessage> got;
    for (size_t pos = 0; pos < size;) {
        const size_t len = std::min<size_t>(size - pos, 1 + data[pos] % 13);
        chunked.feed(stream.mid(qsizetype(pos), qsizetype(len)));
        got = drain(chunked, std::move(got));
        pos += len;
    }

    check(chunked.error() == whole.error());
    check(got.size() == expected.size());
    for (size_t i = 0; i < got.size(); ++i) check(same(got[i], expected[i]));

    // A framing error latches: a valid clipboard record after it is not parsed.
    if (whole.error()) {
        whole.feed(QByteArray::fromHex("00000003016869"));
        check(!whole.next().has_value());
        check(whole.error());
    }
    return 0;
}
