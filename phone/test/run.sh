#!/bin/bash
# The phone app's tests that need no phone (bd remora-6a1k.4): its QR decoder, which uses no
# Android classes, on this computer's JVM against the codes qrencode draws.
#
# Usage: phone/test/run.sh [<scratch dir>]   (default phone/out/test)
# Exit 77, which ctest reports as skipped, without a JDK or qrencode.
set -euo pipefail
SELF="$(cd "$(dirname "$0")" && pwd)"
OUT="${1:-$SELF/../out/test}"
for tool in javac java qrencode; do
    command -v "$tool" >/dev/null || { echo "skipped: no $tool" >&2; exit 77; }
done
rm -rf "$OUT" && mkdir -p "$OUT"
javac -encoding UTF-8 -nowarn -d "$OUT" \
    "$SELF/../src/io/github/etheraura/remora/phone/QrDecoder.java" "$SELF/QrDecoderTest.java"
exec java -Djava.awt.headless=true -cp "$OUT" io.github.etheraura.remora.phone.QrDecoderTest
