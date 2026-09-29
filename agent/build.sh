#!/bin/bash
# Build the Remora agent dex against the in-tree platform framework (docs/MIRROR_AGENT.md).
# The codecprobe pattern (bd remora-e5x.27): javac against the FULL framework.jar — hidden APIs
# are plain calls — then d8 to dex. No SDK, no gradle, no image rebuild; iterate on the live
# container by restarting the service with the fresh dex.
#
# Usage: agent/build.sh [<android tree>]   (default $REMORA_SOURCE_TREE) — a tree that has been built
set -euo pipefail
SELF="$(cd "$(dirname "$0")" && pwd)"
TREE="${1:-${REMORA_SOURCE_TREE:?usage: agent/build.sh <android tree>}}"
FRAMEWORK="$TREE/out/soong/.intermediates/frameworks/base/framework/android_common/turbine-combined/framework.jar"
D8="$TREE/out/host/linux-x86/bin/d8"
[ -f "$FRAMEWORK" ] || { echo "framework.jar not found: $FRAMEWORK" >&2; exit 1; }
[ -x "$D8" ] || { echo "d8 not found: $D8" >&2; exit 1; }

OUT="$SELF/out"
rm -rf "$OUT" && mkdir -p "$OUT/classes"
find "$SELF/src" -name '*.java' > "$OUT/sources.txt"
# Plain -cp, exactly the codecprobe recipe: the host JDK compiles against the full framework
# jar and d8 downlevels the bytecode. No bootclasspath games — modern javac refuses them.
javac -cp "$FRAMEWORK" -d "$OUT/classes" @"$OUT/sources.txt"
[ -d "$OUT/classes/com" ] || { echo "javac produced nothing" >&2; exit 1; }
# Unquoted on purpose: one argument per class file, and Java package paths carry no whitespace.
# shellcheck disable=SC2046
"$D8" --min-api 26 --output "$OUT" $(find "$OUT/classes" -name '*.class')
mv "$OUT/classes.dex" "$OUT/agent.dex"
# The deployable copy lands in vendor/, which is what the engine mounts — the same road every
# other device-side artefact travels. agent/out/ is scratch (gitignored); this is the one that
# ships. Written IN PLACE (cat >, not install/mv): a running container bind-mounts this FILE,
# which pins its inode — install unlinks and recreates, so the container would keep serving the
# old dex forever while the host file looks fresh (found the hard way in bd remora-28ix.3.3's
# smoke test). In-place truncate+write reaches the mounted inode.
DEX="$SELF/../vendor/agent/agent.dex"
mkdir -p "$(dirname "$DEX")"
[ -f "$DEX" ] || { touch "$DEX" && chmod 644 "$DEX"; }
cat "$OUT/agent.dex" > "$DEX"
echo "built: vendor/agent/agent.dex ($(stat -c%s "$OUT/agent.dex") bytes)"
