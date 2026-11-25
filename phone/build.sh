#!/bin/bash
# Build the Remora phone app (docs/PHONE_APP.md, bd remora-6a1k.2) with an Android source
# tree's prebuilt SDK, or an Android SDK — the agent/build.sh recipe for an app: aapt2 for the
# manifest and resources, javac against the public android.jar, d8, zipalign and apksigner. No
# Gradle, no AndroidX: the app uses the framework alone.
#
# Usage: phone/build.sh [<android tree or SDK>]   (default $REMORA_SOURCE_TREE, then $ANDROID_HOME)
#        An SDK needs platforms;android-36 and build-tools;$REMORA_BUILD_TOOLS (default 36.0.0).
# Output: phone/out/remora-phone.apk, signed with the key in $REMORA_PHONE_KEYSTORE (a PKCS12
#         keystore whose password is in $REMORA_PHONE_KEYSTORE_PASSWORD; the release's), or else
#         with the conventional debug key (~/.android/debug.keystore, made here when missing).
set -euo pipefail
SELF="$(cd "$(dirname "$0")" && pwd)"
ROOT="${1:-${REMORA_SOURCE_TREE:-${ANDROID_HOME:?usage: phone/build.sh <android tree or SDK>}}}"
API=36
MIN_API=29  # Android 10: TLS 1.3, and MediaCodec's Opus decoder
# D8 and apksigner by their jars: the wrapper scripts beside them look for their jars in places a
# source tree's prebuilts do not have.
if [ -d "$ROOT/prebuilts/sdk" ]; then
    BIN="$ROOT/prebuilts/sdk/tools/linux/bin"
    JAR="$ROOT/prebuilts/sdk/$API/public/android.jar"
    R8="$ROOT/prebuilts/r8/r8.jar"  # the D8 the tree's own build runs
    APKSIGNER="$ROOT/prebuilts/sdk/tools/linux/lib/apksigner.jar"
else
    BIN="$ROOT/build-tools/${REMORA_BUILD_TOOLS:-36.0.0}"
    JAR="$ROOT/platforms/android-$API/android.jar"
    R8="$BIN/lib/d8.jar"
    APKSIGNER="$BIN/lib/apksigner.jar"
fi
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

if [ -n "${REMORA_PHONE_KEYSTORE:-}" ]; then
    # The password stays in the environment: apksigner reads it from there, never from argv.
    : "${REMORA_PHONE_KEYSTORE_PASSWORD:?REMORA_PHONE_KEYSTORE is set without its password}"
    java -jar "$APKSIGNER" sign --ks "$REMORA_PHONE_KEYSTORE" --ks-type PKCS12 \
        --ks-pass env:REMORA_PHONE_KEYSTORE_PASSWORD --out "$OUT/remora-phone.apk" "$OUT/aligned.apk"
else
    KS="$HOME/.android/debug.keystore"
    if [ ! -f "$KS" ]; then
        mkdir -p "$(dirname "$KS")"
        keytool -genkeypair -keystore "$KS" -storepass android -keypass android \
            -alias androiddebugkey -keyalg RSA -keysize 2048 -validity 10000 \
            -dname "CN=Android Debug,O=Android,C=US" >/dev/null
    fi
    java -jar "$APKSIGNER" sign --ks "$KS" --ks-pass pass:android --key-pass pass:android \
        --out "$OUT/remora-phone.apk" "$OUT/aligned.apk"
fi
echo "built: phone/out/remora-phone.apk ($VERSION_NAME, $(stat -c%s "$OUT/remora-phone.apk") bytes)"
