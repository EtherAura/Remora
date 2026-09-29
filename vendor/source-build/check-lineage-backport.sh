#!/bin/bash
# check-lineage-backport.sh — maintenance status for the experimental LineageOS platform backport
# (bd remora-cbd). Read-only: fetches refs, touches nothing else.
#
# The backport is a temporary measure. It exists only because LineageOS cut their lineage-24.0
# branches as pure AOSP-17 tracking branches and have not forward-ported their platform work yet.
# Carrying it costs a re-generate + conflict pass after every upstream sync, so the single most
# useful thing this script reports is WHEN YOU CAN THROW IT AWAY.
#
# Usage:  check-lineage-backport.sh <tree> [project-path ...]
#         (default project: packages/apps/Settings)
#
# Reports, per project:
#   1. RETIREMENT — has lineage-24.0 become real? Counts community commits and lineage-touching
#      files on the branch we actually build. Both non-zero means upstream has forward-ported and
#      the backport should be deleted, not maintained.
#   2. UPSTREAM DRIFT — new commits on the source branch since the series was cut. These are
#      backport candidates you do not have yet.
#   3. BASE DRIFT — has our own checkout moved since the series was cut? If so the series was
#      rebased under you and the conflict profile has changed; re-run gen-lineage-backport.sh.
set -u
TREE="${1:-}"; shift || true
[ -n "$TREE" ] || { echo "usage: check-lineage-backport.sh <tree> [project-path ...]" >&2; exit 2; }
PROJECTS=${*:-packages/apps/Settings}
SELF="$(cd "$(dirname "$0")" && pwd)"

for PROJ in $PROJECTS; do
  D="$TREE/$PROJ"
  OUT="$SELF/../source-patches/lineage-backport-$(echo "$PROJ" | tr '/' '-')"
  echo "=================================================================="
  echo "$PROJ"
  echo "=================================================================="
  [ -d "$D/.git" ] || { echo "  not a git project: $D"; continue; }

  # ---- 1. can we retire it? -------------------------------------------------
  # Count lineage-NAMED TRACKED FILES. Do not be tempted to use an author filter: on a pristine
  # AOSP tracking branch the last 300 commits still contain hundreds of non-Google authors
  # (external AOSP contributors), so that metric reads ~300 and means nothing. File presence is
  # unambiguous — LineageOS's platform work always lands Lineage-named sources, and a pure AOSP
  # branch has exactly zero.
  #
  # COUNT ON THE MANIFEST REF, NEVER ON HEAD. This read HEAD and inverted its own
  # verdict the moment the backport was applied: HEAD is exactly where our series lands, so the
  # check was measuring OUR forward-port and calling it upstream's. Measured on the live A17 tree
  # that day — HEAD 7 lineage files ("RETIRE"), m/lineage-24.0 0 ("KEEP") — i.e. it advised
  # deleting the backport precisely because the backport was working. repo checks every project out
  # at the manifest revision and aliases it refs/remotes/m/<branch>, which is pre-patch by
  # construction and is the only ref here that speaks for upstream.
  mref=$(cd "$D" && git for-each-ref --format='%(refname)' 'refs/remotes/m/*' 2>/dev/null | head -1)
  head_files=$(cd "$D" && git ls-tree -r --name-only HEAD 2>/dev/null | grep -ci lineage)
  echo "  RETIREMENT CHECK (is lineage-24.0 real yet?)"
  if [ -z "$mref" ]; then
    # No fallback to HEAD on purpose — that is the bug above. An honest "cannot tell" is the only
    # safe answer, since the wrong one here is "delete a series that took a conflict pass to build".
    echo "    no refs/remotes/m/* in this project — cannot identify the upstream ref"
    echo "    => UNKNOWN: not a repo-managed checkout? Verify by hand before retiring anything."
  else
    files=$(cd "$D" && git ls-tree -r --name-only "$mref" 2>/dev/null | grep -ci lineage)
    echo "    lineage-named files on ${mref#refs/remotes/}: $files   (our HEAD: $head_files)"
    if [ "$files" -gt 0 ]; then
      echo "    => RETIRE: upstream has forward-ported. Delete the backport rather than maintain it."
    else
      echo "    => KEEP: upstream is still pristine AOSP here."
      [ "$head_files" -gt 0 ] && echo "       (HEAD's $head_files are the backport itself, applied — expected)"
    fi
  fi

  [ -f "$OUT/UPSTREAM" ] || { echo "  (no series generated yet — run gen-lineage-backport.sh)"; continue; }
  # UPSTREAM defines these four. Cleared first so a file missing one cannot inherit the previous
  # project's value.
  branch='' upstream_sha='' base_sha='' generated_patches=''
  # shellcheck disable=SC1090
  . "$OUT/UPSTREAM"

  # ---- 2. upstream drift ----------------------------------------------------
  git -C "$D" fetch github "$branch" --depth=2000 -q 2>/dev/null || true
  ref=$(git -C "$D" rev-parse --verify -q "github/$branch" || echo "")
  echo "  UPSTREAM DRIFT ($branch)"
  if [ -n "$ref" ] && [ "$ref" != "$upstream_sha" ]; then
    n=$(git -C "$D" rev-list --count --no-merges "$upstream_sha..$ref" 2>/dev/null || echo '?')
    echo "    moved: $upstream_sha -> $ref  ($n new commits)"
    echo "    => re-run gen-lineage-backport.sh to pick them up"
  else
    echo "    unchanged since the series was cut ($upstream_sha)"
  fi

  # ---- 3. base drift --------------------------------------------------------
  # Compare the MANIFEST ref, not HEAD — same trap as the retirement check above. base_sha is the
  # upstream revision the series was cut against, and our series lands on HEAD, so a HEAD
  # comparison starts reporting "the base moved, regenerate" the moment the backport is applied
  # and never stops. Advice that can only be satisfied by un-applying the series is noise, and
  # noise next to a real signal is how the real one gets ignored.
  echo "  BASE DRIFT (upstream revision this checkout is pinned to)"
  if [ -z "$mref" ]; then
    echo "    no refs/remotes/m/* — cannot determine the pinned revision"
  else
    now=$(git -C "$D" rev-parse "$mref")
    if [ "$now" != "$base_sha" ]; then
      echo "    moved: $base_sha -> $now"
      echo "    => the series was cut against a different base; regenerate and re-check conflicts"
    else
      echo "    unchanged since the series was cut"
    fi
  fi

  echo "  SERIES: $generated_patches upstream patches + $(ls "$OUT"/9*.patch 2>/dev/null | wc -l) Remora fixup(s)"
done
