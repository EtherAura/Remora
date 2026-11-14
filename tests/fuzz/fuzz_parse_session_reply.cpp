// libFuzzer harness for parseSessionReply (bd remora-fdcv): the agent's answer to a session
// request, which the client parses from bytes the guest controls (MirrorSession.cpp).
//
// The whole input is the receive buffer, so the seeds are the pin tests' own vectors byte for
// byte. The parser is incremental and a reply may be followed by more of the stream, so consumed
// must never reach past the buffer: the caller drops that many bytes and parses the rest.
#include "core/MirrorProto.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>

using remora::mirror::parseSessionReply;
using remora::mirror::SessionReply;

namespace {

void check(bool ok) {
    if (!ok) std::abort();
}

bool same(const SessionReply &a, const SessionReply &b) {
    return a.status == b.status && a.sessionId == b.sessionId && a.reason == b.reason;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    const QByteArray buf(reinterpret_cast<const char *>(data), qsizetype(size));

    int consumed = -1;
    const auto r = parseSessionReply(buf, &consumed);
    // consumed is optional: leaving it out must not change whether a reply is there.
    check(parseSessionReply(buf, nullptr).has_value() == r.has_value());
    if (!r) return 0;

    // The issue's invariant: a reply is never longer than the bytes it was parsed from.
    check(consumed > 0 && qsizetype(consumed) <= buf.size());
    if (r->status == 0) check(r->reason.isEmpty());

    // Incremental: the reply is complete at exactly consumed bytes and not a byte earlier, so what
    // follows it on the wire plays no part in it.
    int again = -1;
    const auto whole = parseSessionReply(buf.left(consumed), &again);
    check(whole.has_value() && same(*whole, *r) && again == consumed);
    check(!parseSessionReply(buf.left(consumed - 1), nullptr).has_value());
    return 0;
}
