#!/bin/bash
# Build the Remora phone app (docs/PHONE_APP.md, bd remora-6a1k.2) with an Android source
# tree's prebuilt SDK — the agent/build.sh recipe for an app: aapt2 for the manifest and resources,
# javac against the public android.jar, d8, zipalign and apksigner. No Gradle, no AndroidX: the app
# uses the framework alone.
#
# Usage: phone/build.sh [<android tree>]   (default $REMORA_SOURCE_TREE)
# Output: phone/out/remora-phone.apk, signed with the conventional debug key
#         (~/.android/debug.keystore, made here when missing).
set -euo pipefail
SELF="$(cd "$(dirname "$0")" && pwd)"
TREE="${1:-${REMORA_SOURCE_TREE:?usage: phone/build.sh <android tree>}}"
API=36
MIN_API=29  # Android 10: TLS 1.3, and MediaCodec's Opus decoder
BIN="$TREE/prebuilts/sdk/tools/linux/bin"
JAR="$TREE/prebuilts/sdk/$API/public/android.jar"
# D8 and apksigner by their jars: the prebuilt wrapper scripts look for their jars beside
# themselves and do not find them. r8.jar is the D8 the tree's own build runs.
R8="$TREE/prebuilts/r8/r8.jar"
APKSIGNER="$TREE/prebuilts/sdk/tools/linux/lib/apksigner.jar"
for f in "$BIN/aapt2" "$BIN/zipalign" "$R8" "$APKSIGNER" "$JAR"; do
    [ -e "$f" ] || { echo "missing $f" >&2; exit 1; }
done

VERSION_NAME="$(cat "$SELF/../VERSION")"
# 2026.11.8 -> 2026110800: monotonic across calendar versions.
VERSION_CODE="$(echo "$VERSION_NAME" | awk -F. '{ printf "%d%02d%02d00", $1, $2, $3 }')"

OUT="$SELF/out"
rm -rf "$OUT" && mkdir -p "$OUT/gen" "$OUT/classes"
"$BIN/aapt2" compile --dir "$SELF/res" -o "$OUT/res.zip"
"$BIN/aapt2" link -o "$OUT/base.apk" -I "$JAR" --manifest "$SELF/AndroidManifest.xml" \
    --java "$OUT/gen" --min-sdk-version "$MIN_API" --target-sdk-version "$API" \
    --version-code "$VERSION_CODE" --version-name "$VERSION_NAME" "$OUT/res.zip"
find "$SELF/src" "$OUT/gen" -name '*.java' > "$OUT/sources.txt"
# The framework jar on the class path, as agent/build.sh does; d8 desugars to the minimum API.
javac -source 17 -target 17 -nowarn -Xlint:-options -encoding UTF-8 -cp "$JAR" \
    -d "$OUT/classes" @"$OUT/sources.txt"
# shellcheck disable=SC2046  # one argument per class file; package paths carry no whitespace
java -cp "$R8" com.android.tools.r8.D8 --min-api "$MIN_API" --lib "$JAR" --output "$OUT" \
    $(find "$OUT/classes" -name '*.class')
cp "$OUT/base.apk" "$OUT/unsigned.apk"
(cd "$OUT" && zip -q unsigned.apk classes.dex)
"$BIN/zipalign" -f -p 4 "$OUT/unsigned.apk" "$OUT/aligned.apk"

KS="$HOME/.android/debug.keystore"
if [ ! -f "$KS" ]; then
    mkdir -p "$(dirname "$KS")"
    keytool -genkeypair -keystore "$KS" -storepass android -keypass android \
        -alias androiddebugkey -keyalg RSA -keysize 2048 -validity 10000 \
        -dname "CN=Android Debug,O=Android,C=US" >/dev/null
fi
java -jar "$APKSIGNER" sign --ks "$KS" --ks-pass pass:android --key-pass pass:android \
    --out "$OUT/remora-phone.apk" "$OUT/aligned.apk"
echo "built: phone/out/remora-phone.apk ($VERSION_NAME, $(stat -c%s "$OUT/remora-phone.apk") bytes)"
