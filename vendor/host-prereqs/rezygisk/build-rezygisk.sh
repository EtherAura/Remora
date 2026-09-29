#!/usr/bin/env bash
# Build ReZygisk from source with Remora's bounded-append patch, and install the
# result into the vendored module (bd remora-4ei.98).
#
# WHY THIS EXISTS. We used to ship zygisk-ptrace64 as a BINARY PATCH: patch-rezygisk.py NOPped the
# three __strcat_chk call sites whose unbounded strcat aborts the daemon under A16+ bionic FORTIFY,
# and the NOPped blob was committed as zygisk-ptrace64.patched. That worked, but it meant carrying a
# machine-edited GPL-3.0 binary and re-deriving the edit on every bump. Building from the pinned
# source with a source patch replaces a defeatable binary edit with a structural fix — bounded
# appends everywhere instead of three dead calls — and lets both the script and the blob be deleted.
#
# THE PATCH IS OURS AND STAYS OURS. It is not an upstream submission (standing direction: carry our
# own fix rather than wait on someone else's project), so this script is the whole delivery
# mechanism for it.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
FEATURE="$REPO/vendor/source-build/features/zygisk_pif"
PATCH="$FEATURE/upstream/0001-monitor-bound-the-module.prop-status-appends-fixes-a.patch"
MOD="$FEATURE/rez-mod"

WORK="${REMORA_REZYGISK_WORK:-${XDG_CACHE_HOME:-$HOME/.cache}/remora/rezygisk-src}"
URL="${REMORA_REZYGISK_URL:-https://github.com/PerformanC/ReZygisk}"
# THE PIN IS THE POINT: this is build 537's commit, the exact source of the binary we shipped before
# this script existed, so the substitution is provably like-for-like rather than a version bump
# smuggled in alongside a fix. Moving it is a deliberate act — re-verify injection on device after.
PIN="${REMORA_REZYGISK_PIN:-e42886f48eb1c9eabcc94f08a3c3af0cdbffb99e}"
NDK="${REMORA_REZYGISK_NDK:-${REMORA_MESA_NDK:?set REMORA_REZYGISK_NDK (or REMORA_MESA_NDK) to an Android NDK root, r27c}}"
# common.mk defaults to NDK 29.0.13113456; r27c is what the rest of this project uses and what the
# patch was compile-verified against, so it is passed explicitly rather than left to discovery.
API="${REMORA_REZYGISK_API:-25}"

say()  { printf '  %s\n' "$*"; }
head_() { printf '\n=== %s ===\n' "$*"; }
die()  { printf 'rezygisk-build: %s\n' "$*" >&2; exit 1; }

[ -d "$NDK" ] || die "no NDK at $NDK (set REMORA_REZYGISK_NDK)"
[ -f "$PATCH" ] || die "no patch at $PATCH"
[ -d "$MOD/bin" ] || die "no vendored module at $MOD"

head_ "1. source at $PIN"
if [ ! -d "$WORK/.git" ]; then
    mkdir -p "$WORK"
    git -C "$WORK" init -q .
    git -C "$WORK" remote add origin "$URL"
fi
git -C "$WORK" fetch -q --depth 1 origin "$PIN" || die "could not fetch $PIN from $URL"
git -C "$WORK" checkout -q --force FETCH_HEAD
# Reset to pristine BEFORE patching so a re-run does not stack the patch on itself.
git -C "$WORK" reset -q --hard FETCH_HEAD
git -C "$WORK" clean -qfd -e build
say "checked out $(git -C "$WORK" rev-parse --short HEAD)"

# SUBMODULES ARE NOT OPTIONAL and a shallow single-commit fetch does not bring them: loader/src/
# external/{plti,csoloader} are separate repos, and without them the build dies on
#   src/injector/hook.c: fatal error: 'csoloader.h' file not found
git -C "$WORK" submodule update --init --recursive --depth 1 -q || die "submodule init failed"
say "submodules: $(git -C "$WORK" submodule status | wc -l) initialised"

head_ "2. apply the bounded-append patch"
git -C "$WORK" apply "$PATCH" || die "patch did not apply to $PIN — the pin moved or the patch is stale"
say "applied $(basename "$PATCH")"

head_ "3. build (NDK $(basename "$NDK"), API $API)"
# BUILD FROM THE TOP LEVEL, not `make -C loader`. The loader link needs libplti.a and libcsoloader.a,
# which only the top-level targets produce — going straight at the loader compiles every object and
# then fails with "no such file or directory: .../libplti.a". Note ARCH does NOT restrict the build
# to one ABI; all four are built regardless.
#
# `make release` also runs a module-packaging step that fails in this environment. That is packaging,
# not compilation, and every binary we install below is produced before it — so the failure is
# tolerated here and the GATE below is what decides whether the build was good.
make -C "$WORK" release NDK_PATH="$NDK" API_LEVEL="$API" >/dev/null 2>&1 || \
    say "top-level make returned non-zero (module packaging); checking artifacts instead"

L="$WORK/build/obj/release/loader"
Z="$WORK/build/obj/release/zygiskd"
# 64-BIT ONLY, and that is a finding rather than a shortcut. The x86 (32-bit) loader does not link
# with NDK r27c:
#   ld.lld: error: undefined symbol: _GLOBAL_OFFSET_TABLE_   (linking libzygisk.so)
# upstream's common.mk expects NDK 29.0.13113456, and this is the kind of 32-bit PIC/relocation
# breakage a toolchain gap produces. It does not matter here: the FORTIFY abort this patch fixes is
# 64-BIT ONLY — the 32-bit daemon has always run unpatched and serves requests fine on A17 — so the
# 32-bit half stays vendored, exactly as it is today. Both halves therefore remain internally
# consistent: the 64-bit set comes wholly from this build, the 32-bit set wholly from the upstream
# CI build of the SAME commit.
for f in "$L/x86_64/stripped/libzygisk_ptrace.so" "$L/x86_64/stripped/libzygisk.so" \
         "$Z/x86_64/zygiskd"; do
    [ -f "$f" ] || die "build did not produce $f"
done
say "64-bit artifacts present (32-bit stays vendored — see comment)"

head_ "4. GATE: is the FORTIFY abort actually gone?"
# CALL SITES, NOT SYMBOLS. __strcat_chk legitimately survives in the binary: spawn_daemon appends two
# bytes to a PATH_MAX buffer holding a 13-byte literal, which is provably safe and deliberately left
# alone. A symbol-level check reads that as a failed patch. Only call-site counts separate them — the
# same trap recorded on this bead when the patch was first verified.
NM="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-objdump"
P64="$L/x86_64/stripped/libzygisk_ptrace.so"
cat_sites=$("$NM" -d "$P64" 2>/dev/null | grep -cE 'call.*<__strcat_chk' || true)
lcat_sites=$("$NM" -d "$P64" 2>/dev/null | grep -cE 'call.*<__strlcat_chk' || true)
say "zygisk-ptrace64: __strcat_chk $cat_sites, __strlcat_chk $lcat_sites"
[ "$cat_sites" = "1" ] || die "expected exactly 1 __strcat_chk call site (the safe spawn_daemon one), got $cat_sites — the patch did not take"
[ "$lcat_sites" -ge 1 ] || die "no __strlcat_chk call sites — the patch did not take"

head_ "5. install into $MOD"
install -m755 "$L/x86_64/stripped/libzygisk_ptrace.so" "$MOD/bin/zygisk-ptrace64"
install -m755 "$Z/x86_64/zygiskd"                      "$MOD/bin/zygiskd64"
install -m644 "$L/x86_64/stripped/libzygisk.so"        "$MOD/lib64/libzygisk.so"
# THE WHOLE 64-BIT SET, not just the ptracer. zygiskd and zygisk-ptrace talk to each other over a
# private protocol, so replacing one and keeping the other would pair two artifacts nothing
# guarantees agree. They agreed before because they came from one upstream CI build; they agree now
# because they come from one local build of that same commit. bin/zygiskd32, bin/zygisk-ptrace32 and
# lib/libzygisk.so are deliberately NOT touched — same reasoning, other bitness.
say "installed 3 x86_64 binaries from $(git -C "$WORK" rev-parse --short HEAD)"

printf '\nrezygisk-build: OK — module updated. Re-verify Play-spoof injection on device.\n'
