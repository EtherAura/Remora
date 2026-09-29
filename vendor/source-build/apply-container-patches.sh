#!/bin/sh
# Apply the upstream project's public, version-pinned container-compat patches so the source build boots as a
# container (the MANDATORY container_compat_patches).
# Args: <tree> <patchset-dir> [project-path...]
#
# The optional project paths RESTRICT the pass to those projects. Remora's incremental patch state
# force-syncs only the projects whose own patches changed, and a targeted sync drops that project's
# container-compat commits along with them — so those have to be re-applied, but only there. The
# projects that were not synced still carry theirs, and re-running over them would rewrite files
# Soong has already analysed as well as exercise an already-applied path this script has never taken
# (today a full sync always precedes it, so every project is pristine).
set -e
TREE="$1"; PD="$2"
# Usage check BEFORE the shift: `shift 2` with fewer than two arguments fails, and under `set -e`
# that would exit silently instead of printing how to call this.
[ -d "$TREE" ] && [ -d "$PD" ] || { echo "usage: apply-container-patches.sh <tree> <patchset> [project...]"; exit 2; }
shift 2
ONLY="$*"
# `if`, not `[ -n "$ONLY" ] && echo …`: an empty ONLY would make the && list return 1 and `set -e`
# would abort the script before applying anything.
if [ -n "$ONLY" ]; then echo "=== container-compat: restricted to $ONLY ==="; fi

# APPLIED-CONTENT STAMPS (bd remora-h457). Each visited project records the sha256 of every patch
# it was given; the verify pass at the bottom re-reads them. This exists because a restricted pass
# leaves most projects untouched on the assumption that what they already carry is current — and
# when a patch FILE is edited without its project being re-synced, that assumption is silently
# false and the image quietly lacks the change.
#
# Why a stamp rather than asking git: measured on the real A17 tree, 4 of 35 patches fail
# `git apply --reverse --check` while being fully present (later commits move their context), and
# --3way only narrows that to 2. Both numbers are false positives, so any git-apply predicate would
# either fail correct builds or need a fudge factor. Hashing the input is exact and cannot drift.
STATE="$TREE/.remora-patch-state.d"
mkdir -p "$STATE" 2>/dev/null || true
# Key by path-with-underscores; only this script reads these, so it need not match the C++ slug.
stamp_file() { echo "$STATE/cp-$(echo "$1" | tr '/' '_')"; }
# Relative names, so the stamp is stable if the patchset is mounted at a different path.
stamp_now() { ( cd "$PD/$1" && sha256sum *.patch ); }

cd "$PD"
AVAIL=0   # projects in the patchset that carry patches
WANTED=0  # ...of those, the ones THIS pass was asked to visit (== AVAIL when unrestricted)
DONE=0    # projects actually visited
FAILED=   # "<proj>/<patch>" for every patch that could not be applied at all
# AOSP project paths never contain whitespace, and a find | while pipe would run the counters
# below in a subshell and lose them.
# shellcheck disable=SC2044
for proj in $(find * -type d); do
  ls "$PD/$proj"/*.patch >/dev/null 2>&1 || continue
  AVAIL=$((AVAIL + 1))
  if [ -n "$ONLY" ]; then
    keep=
    # `if`, not `[ … ] && keep=1`: under `set -e` a false test as the last command of a loop body
    # aborts the whole script.
    for o in $ONLY; do
      if [ "$o" = "$proj" ]; then keep=1; fi
    done
    [ -n "$keep" ] || continue
  fi
  WANTED=$((WANTED + 1))
  [ -d "$TREE/$proj/.git" ] || { echo "skip (not in tree): $proj"; continue; }
  DONE=$((DONE + 1))
  PROJ_BAD=
  echo "=== container-compat: $proj ==="
  if ! git -C "$TREE/$proj" am --keep-cr --3way "$PD/$proj"/*.patch; then
    git -C "$TREE/$proj" am --abort 2>/dev/null || true
    # `git apply --3way` WRITES CONFLICT MARKERS even when it exits non-zero, and it leaves
    # UNMERGED INDEX ENTRIES. That matters for the cleanup: `git checkout -- .` refuses to touch
    # unmerged paths, so it does NOT undo the damage — measured on frameworks/av, which was left
    # with 2 poisoned files. `git reset --hard` does clean it, but would also discard patches
    # applied earlier in the same project, since these are working-tree edits with no commit
    # behind them. So commit each successful fallback apply, exactly as the backport path does,
    # and only then is reset --hard safe.
    for p in "$PD/$proj"/*.patch; do
      if git -C "$TREE/$proj" apply --3way "$p" 2>/dev/null; then
        git -C "$TREE/$proj" add -A 2>/dev/null || true
        git -C "$TREE/$proj" -c user.email=remora@local -c user.name=Remora \
            commit -qm "container-compat: $(basename "$p" .patch)" 2>/dev/null || true
        continue
      fi
      if git -C "$TREE/$proj" apply --reverse --check "$p" 2>/dev/null; then
        git -C "$TREE/$proj" reset --hard -q 2>/dev/null || true
        git -C "$TREE/$proj" clean -fdq 2>/dev/null || true
        echo "  (already applied) $(basename "$p")"; continue
      fi
      git -C "$TREE/$proj" reset --hard -q 2>/dev/null || true
      git -C "$TREE/$proj" clean -fdq 2>/dev/null || true
      echo "  ***WARN could not apply $(basename "$p") — SKIPPED (tree left clean)"
      PROJ_BAD=1
      FAILED="$FAILED $proj/$(basename "$p")"
    done
  fi
  # Stamp only a project that took ALL of its patches. Stamping a partial apply would record the
  # current inputs as satisfied and make the verify pass below endorse the very gap it exists to
  # catch — the warning above is already the weakest link, and it must not be laundered into a
  # clean record.
  if [ -n "$PROJ_BAD" ]; then
    rm -f "$(stamp_file "$proj")" 2>/dev/null || true
  else
    stamp_now "$proj" > "$(stamp_file "$proj")" 2>/dev/null || true
  fi
done
# HARD-FAIL A PASS THAT DID NOTHING (bd remora-4ei.82). These patches are what make the built image
# boot as a container, and they carry our own fixes too — system/core 0002 (the memfd sign-in fix)
# among them. A pass that visits ZERO projects while the patchset holds some produced an image
# SILENTLY missing all of it, and said nothing: measured on builds 5 and 6, which reported
# "patch inputs changed — full reset, sync and apply" and then applied 0 of 24. Build 6 went on to
# print "source build OK". Build 5 only failed at all because one missing patch happened to break a
# compile 20 minutes later; had the missing set only changed runtime behaviour, a regressed image
# would have shipped looking perfect.
#
# The trigger there was an upstream repo sync failure that the chain swallowed (bd remora-4ei.81),
# but the guard is deliberately on the OUTCOME rather than that one cause, so any future reason for
# an empty pass stops the build instead of shipping.
#
# GUARD ON WHAT THIS PASS WAS ASKED TO DO, NOT ON THE WHOLE PATCHSET. A RESTRICTED pass ($ONLY, used
# when only some projects drifted) legitimately visits zero of them when the named projects carry no
# container-compat patches at all — external/minigbm is the case that exposed this: a build whose
# only drift was minigbm printed "0 of 22 project(s) visited" and refused, though nothing was wrong
# and nothing was missing. Comparing DONE against WANTED rather than AVAIL fixes that without
# weakening anything: when the pass is UNRESTRICTED, WANTED == AVAIL by construction, so the
# empty-pass detection bd remora-4ei.82 added is untouched for exactly the case it was written for.
echo "=== container-compat: $DONE of $AVAIL project(s) visited ($WANTED targeted) ==="
if [ "$WANTED" -gt 0 ] && [ "$DONE" -eq 0 ]; then
  echo "container-compat: applied NOTHING — $WANTED targeted project(s) carry patches and none was visited." >&2
  echo "  The tree is missing every container-compat patch, including the ones the image needs to" >&2
  echo "  boot as a container. Refusing to build. Usual cause: the repo sync before this step" >&2
  echo "  failed, so the projects are absent or unreadable — check the sync output above." >&2
  exit 1
fi
# VERIFY WHAT THE PASS DID NOT TOUCH (bd remora-h457). The loop above only visits the projects it
# was told to; every other project keeps whatever a previous build left in it. That is correct when
# the patches have not changed and silently wrong when they have — the failure being a green build
# whose image lacks the edit you just made, which is how the binder_alloc rename reached an image
# still naming the old path.
#
# So: compare each project's CURRENT patch hashes against the stamp written when it was last
# applied. Visited projects were stamped moments ago and pass trivially; the check is really about
# the skipped ones. A missing stamp is "cannot verify", not "bad" — the first build after this
# change has no stamps for anything, and a full pass writes them all.
# A patch that could not be applied at all leaves the project missing container-compat behaviour,
# which is the same silently-wrong-image outcome this pass exists to prevent — so it refuses too,
# rather than warning and building on (bd remora-h457). Collected across all projects first, so one
# run reports every broken patch instead of stopping at the first.
if [ -n "$FAILED" ]; then
  echo "container-compat: PATCH(ES) COULD NOT BE APPLIED:$FAILED" >&2
  echo "  The affected project(s) are missing container-compat changes the image needs, and the" >&2
  echo "  warnings above are the only sign. Refusing to build on a tree in that state." >&2
  echo "  Usual cause: the project was not pristine when this ran — it still carried an older" >&2
  echo "  form of the patch, so neither a clean apply nor the already-applied check succeeded." >&2
  echo "  Remedy: force a full sync:" >&2
  echo "    rm -f $TREE/.remora-patch-state $TREE/.remora-incremental" >&2
  exit 1
fi
DRIFT=
UNVERIFIED=0
# shellcheck disable=SC2044  # as above
for proj in $(find * -type d); do
  ls "$PD/$proj"/*.patch >/dev/null 2>&1 || continue
  [ -d "$TREE/$proj/.git" ] || continue
  sf="$(stamp_file "$proj")"
  if [ -f "$sf" ]; then
    if [ "$(stamp_now "$proj")" != "$(cat "$sf")" ]; then DRIFT="$DRIFT $proj"; fi
  else
    UNVERIFIED=$((UNVERIFIED + 1))
  fi
done
if [ "$UNVERIFIED" -gt 0 ]; then
  echo "=== container-compat: $UNVERIFIED project(s) have no applied-content stamp yet (first run after bd remora-h457; a full pass records them) ==="
fi
if [ -n "$DRIFT" ]; then
  echo "container-compat: PATCH FILES CHANGED BUT THE TREE STILL CARRIES THE OLD ONES." >&2
  echo "  Drifted project(s):$DRIFT" >&2
  echo "  These were not re-synced this run, so this pass did not revisit them, and what they" >&2
  echo "  carry is a PREVIOUS version of the patch. Building now would produce an image that" >&2
  echo "  looks correct and silently lacks your edit — refusing." >&2
  echo "  Remedy: force a full sync so the projects come back pristine and every patch is" >&2
  echo "  re-applied, by removing the incremental skip mark:" >&2
  echo "    rm -f $TREE/.remora-patch-state $TREE/.remora-incremental" >&2
  exit 1
fi
echo "=== container-compat patches applied ==="
