#!/bin/bash
# gen-lineage-backport.sh — derive an EXPERIMENTAL LineageOS platform backport as a patch series
# (bd remora-cbd). Args: <tree> <project-path> <lineage-branch> [out-dir]
#
# WHY THIS EXISTS. The A17 image is AOSP 17 + the LineageOS 23.2 *app* suite, because LineageOS
# cut their lineage-24.0 branches early as pure AOSP-17 tracking branches and have not
# forward-ported their platform work yet. Measured on packages/apps/Settings@lineage-24.0: zero
# files matching "lineage" and zero LineageOS-authored commits. So the platform ships AOSP UI and
# nothing reads the ro.lineage.* props we set. See memory a17-is-aosp-not-lineage.
#
# This script extracts the part that is actually LineageOS's — their own commits on the previous
# release branch — as a rebasable series, so the backport is REPRODUCIBLE rather than a pile of
# hand-carried diffs that nobody can regenerate.
#
# THE KEY FILTER: everything in the range EXCEPT Google-authored commits. The raw delta between
# lineage-23.2 and our lineage-24.0 HEAD is ~135 commits for Settings; ~38 are AOSP-16-era commits
# from google.com/android.com that we emphatically do NOT want on an AOSP-17 tree — cherry-picking
# them would drag the platform backwards. The remaining ~97 are the community's work.
#
# Do NOT "simplify" this to --author='lineageos\.org'. That was the first attempt and it caught
# only 18 of the 97: LineageOS contributors overwhelmingly author from gmail and personal domains
# (measured: 50 gmail.com, 18 lineageos.org, 5 mortimer.me.uk, 3 pm.me, 3 cyngn.com...). Worse, it
# silently dropped the one commit that matters most — 7c1861cde9f "Settings: Add LineageOS entries
# into device info", which is what puts the LineageOS version row in About phone.
#
# Usage:
#   gen-lineage-backport.sh <lineage-24-tree> packages/apps/Settings lineage-23.2
#
# The matching SourcePatch entry leaves its 'patches' list EMPTY, which means "apply every
# NNNN-*.patch in this directory, in sorted order" — so regenerating the series never requires
# touching C++. Enable it in the GUI: Image page > source patches > "LineageOS platform backport".
#
# EXPECT CONFLICTS. These commits were written against AOSP 16. The build applies them with
# `git am --keep-cr` falling back to `git apply --3way`, which resolves the easy cases and leaves
# the rest. This is the ongoing merge burden that comes with option (b) — it is the same work
# LineageOS themselves do each cycle, and it does not go away until upstream forward-ports.
set -eu

TREE="${1:-}"; PROJ="${2:-}"; BRANCH="${3:-}"
[ -n "$TREE" ] && [ -n "$PROJ" ] && [ -n "$BRANCH" ] || {
  echo "usage: gen-lineage-backport.sh <tree> <project-path> <lineage-branch> [out-dir]" >&2
  exit 2
}
SELF="$(cd "$(dirname "$0")" && pwd)"
OUT="${4:-$SELF/../source-patches/lineage-backport-$(echo "$PROJ" | tr '/' '-')}"
D="$TREE/$PROJ"
[ -d "$D/.git" ] || { echo "not a git project: $D" >&2; exit 1; }

echo "=== fetching $BRANCH for $PROJ ==="
git -C "$D" fetch github "$BRANCH" --depth=2000 -q 2>/dev/null \
  || git -C "$D" fetch origin "$BRANCH" --depth=2000 -q 2>/dev/null \
  || { echo "could not fetch $BRANCH" >&2; exit 1; }

REF=$(git -C "$D" rev-parse --verify -q "github/$BRANCH" || git -C "$D" rev-parse --verify -q "origin/$BRANCH")
BASE=$(git -C "$D" merge-base "$REF" HEAD)
TOTAL=$(git -C "$D" rev-list --count --no-merges "$BASE..$REF")

rm -rf "$OUT"; mkdir -p "$OUT"
# Negative-lookahead author filter (needs --perl-regexp): keep the community's commits, drop the
# AOSP-16 history that is already present, or deliberately absent, on our newer base.
git -C "$D" format-patch --no-merges --no-signature -o "$OUT" --perl-regexp \
    --author='^(?!.*@(google|android)\.com)' "$BASE..$REF" >/dev/null

# Remora-authored fixups live OUTSIDE the generated directory (which is wiped above) and are
# copied in afterwards, numbered 9001+ so they sort last and land on top of the upstream series.
# They exist because some upstream patches cannot apply to the newer base — e.g. Android.bp
# dependency hunks whose context AOSP has since changed. Keep them in git; the generated series
# is disposable, these are not.
FIX="$SELF/lineage-backport-fixups/$(echo "$PROJ" | tr '/' '-')"
if [ -d "$FIX" ] && ls "$FIX"/*.patch >/dev/null 2>&1; then
  cp "$FIX"/*.patch "$OUT"/
  echo "=== carried $(ls "$FIX"/*.patch | wc -l) Remora fixup(s) from $FIX ==="
fi

# EXCLUSIONS. A patch that FAILS to apply is loud — the apply loop warns and the tree is left
# clean. The dangerous case is the opposite: a patch that applies PERFECTLY while the platform
# code it calls is absent, leaving a tree that is internally inconsistent rather than merely
# missing a feature. That compiles for 45 minutes and then dies somewhere unrelated-looking.
#
# This series is cut from a LineageOS branch (see UPSTREAM: lineage-23.2 for Settings) and
# replayed onto a newer AOSP base carrying almost no LineageOS framework — there is no generated
# frameworks/base backport series at all, only a single hand-written deps fixup. So any patch here
# that calls a Lineage framework API has no provider by construction. Measured instance:
# 0009-Add-toggle-to-enable-ADB-root applies cleanly, calls android.adb.ADBRootService, and that
# class exists in no repo in the tree, in no vendored patch, and in no ref of our frameworks/base.
# It failed the build at 41,623/141,741 targets (bd remora-4ei.32).
#
# EXCLUDE is one shell glob per line, matched against the patch FILENAME; '#' comments and blank
# lines ignored. It lives with the fixups, outside the generated directory, for the same reason
# they do: the generated series is disposable, this is not.
# Drops are ECHOED individually and recorded in UPSTREAM. A silent exclusion would be the same
# class of invisible failure this exists to prevent.
EXCL="$FIX/EXCLUDE"
DROPPED=0
if [ -f "$EXCL" ]; then
  while IFS= read -r pat; do
    case "$pat" in ''|'#'*) continue ;; esac
    for f in "$OUT"/$pat; do
      [ -e "$f" ] || continue
      echo "=== EXCLUDED $(basename "$f")  (matched '$pat')"
      rm -f "$f"
      DROPPED=$((DROPPED + 1))
    done
  done < "$EXCL"
  [ "$DROPPED" -gt 0 ] && echo "=== $DROPPED patch(es) excluded per $EXCL ==="
  # Ship the list WITH the series. The apply loop re-checks it at apply time, because the build
  # stages patches from the installed vendor tree and install(DIRECTORY) copies without pruning —
  # so a patch deleted here can still be present, and applied, on an already-installed host.
  cp "$EXCL" "$OUT/EXCLUDE"
fi

# Record exactly what this series was cut from, so drift is detectable later (see --check).
# base_sha is the UPSTREAM revision the series was cut against, so read the manifest ref rather
# than HEAD: repo pins each project at refs/remotes/m/<branch> and our series lands on top of it.
# Regenerating on a tree that already has the series applied would otherwise record a backport
# commit as the base, and check-lineage-backport.sh could never report drift honestly again.
MREF=$(git -C "$D" for-each-ref --format='%(refname)' 'refs/remotes/m/*' 2>/dev/null | head -1)
[ -n "$MREF" ] || echo "WARNING: no refs/remotes/m/* in $D — recording base_sha from HEAD" >&2
{
  echo "project=$PROJ"
  echo "branch=$BRANCH"
  echo "upstream_ref=$REF"
  echo "upstream_sha=$(git -C "$D" rev-parse "$REF")"
  echo "merge_base=$BASE"
  echo "base_sha=$(git -C "$D" rev-parse "${MREF:-HEAD}")"
  echo "generated_patches=$(ls "$OUT"/[0-8]*.patch 2>/dev/null | wc -l)"
  echo "excluded_patches=$DROPPED"
} > "$OUT/UPSTREAM"

N=$(ls "$OUT"/*.patch 2>/dev/null | wc -l)
if [ "$N" -eq 0 ]; then
  echo "no community commits in $BASE..$REF — nothing to backport"
  rmdir "$OUT" 2>/dev/null || true
  exit 0
fi

echo "=== $N community patches (of $TOTAL total commits in range) -> $OUT ==="
echo
echo "Leave the SourcePatch entry's 'patches' list EMPTY — an empty list means"
echo "\"apply every NNNN-*.patch in the directory, in sorted order\", so regenerating this"
echo "series does not require editing C++."
