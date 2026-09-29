#!/bin/sh
# Stage Remora's OWN tree projects — device/remora (R1) and vendor/remora (R2) — into a source
# tree (bd remora-28ix.4).
#
# Args: <tree> <vendored-device-remora-dir> [<vendored-vendor-remora-dir>]
#
# WHY STAGED RATHER THAN REPO PROJECTS. These are Remora-authored source, so no upstream manifest
# can supply them, and the one thing a manifest needs is a fetchable remote. device/remora briefly
# had one pointing at a directory on a single machine, which made the A17 build unreproducible
# anywhere else and would have shipped that path to every reader of the public tree. Vendored with
# Remora and staged, they need no remote at all — the same treatment (and reasoning) as c2-va.
#
# WHY ITS OWN SCRIPT, RUN BEFORE THE PATCH PASS. stage-features.sh deliberately runs AFTER the
# patch reset/apply, so the mk lines it appends survive the reset. A tree project cannot live
# there: the patch pass would reset/clean the project first, and a copy landing afterwards would
# overwrite whatever the patches did — the three device/remora entries (sensors, boot animation,
# hide-stock-updater) would silently never take effect.
#
# THREE THINGS THIS MUST GET RIGHT, each of which silently produced a wrong image when missed:
#   1. A GIT REPO WITH A COMMIT. The patch pass runs `git init` on a project that has none, then
#      `git reset --hard` + `git clean -fdq`. On a fresh repo with no HEAD the reset is a no-op
#      and EVERY staged file is untracked — so clean would delete the entire project, and the
#      build would then fail to lunch. Committing the pristine copy makes the reset restore
#      exactly this content, which is precisely the contract patches expect.
#   2. IDEMPOTENCE. Re-copying on every build would wipe the patched state each time. Staging is
#      therefore keyed on a stamp of the vendored content and is a no-op while that is unchanged.
#   3. MARK INVALIDATION. When the content DOES change, the copy is replaced pristine — and the
#      patch pass's per-project mark file must go with it, or that pass is skipped (it is guarded
#      by `[ -f <mark> ] ||`) and the image quietly builds without the enabled patches.
set -e
TREE="$1"
SRC="$2"
VENDOR_SRC="${3:-$(dirname "$2")/vendor-remora}"
[ -d "$TREE" ] && [ -d "$SRC" ] || { echo "usage: stage-device-tree.sh <tree> <device-remora-dir> [vendor-remora-dir]"; exit 2; }

# LineageOS's default wallpapers, taken from LineageOS's OWN synced tree rather than vendored.
# They are third-party artwork: committing 5 MB of someone else's PNGs is precisely what the
# release rule against binaries in the tree exists to prevent, and it would trip the CI payloads
# gate. The manifest already syncs vendor/lineage and those files are byte-identical to the ones
# that were vendored, so copying them here costs nothing and keeps the provenance honest. Only the
# wallpapers travel: lineage.mk deliberately does NOT inherit vendor/lineage/overlay/common
# wholesale, whose peripheral package overlays risk aapt2 conflicts.
#
# CALLED BEFORE THE COMMIT, and that is the whole correctness of it. Copied afterwards these files
# are UNTRACKED, and the patch pass that follows runs `git clean -fdq` — which deleted all seven,
# silently, leaving the image with AOSP's wallpaper. Staging said it had copied them and it had;
# they simply did not survive the next step. Tracked, the reset/clean restores them instead.
#
# Best-effort. A tree synced without vendor/lineage (source_kind=aosp, or a partial sync) simply
# gets AOSP's wallpaper — cosmetic, not a broken build, so it must not fail here.
add_lineage_art() {
    _dest="$1"
    _lovl="$TREE/vendor/lineage/overlay/common/frameworks/base/core/res/res"
    if [ ! -d "$_lovl" ]; then
        echo "  (no vendor/lineage in this tree — AOSP wallpaper stands)"
        return 0
    fi
    _n=0
    for _d in hdpi nodpi xhdpi xxhdpi xxxhdpi sw600dp-nodpi sw720dp-nodpi; do
        _src="$_lovl/drawable-$_d/default_wallpaper.png"
        [ -f "$_src" ] || continue
        _dst="$_dest/lineage-overlay/frameworks/base/core/res/res/drawable-$_d"
        mkdir -p "$_dst" && cp -a "$_src" "$_dst/" && _n=$((_n + 1))
    done
    echo "  + $_n LineageOS wallpaper(s) from vendor/lineage"
}

# stage_project <vendored-src> <tree-relative-path> <patch-mark-slug>
# Returns 0 if it staged, 1 if it was already current (so callers can skip follow-up work).
stage_project() {
    _src="$1"; _rel="$2"; _slug="$3"
    _dest="$TREE/$_rel"
    _stamp="$_dest/.remora-staged-from"
    # Content stamp: every file's path + hash, so an edit, an addition or a deletion all change it.
    _want=$(cd "$_src" && find . -type f ! -name '.remora-staged-from' -exec sha256sum {} + \
            | sort | sha256sum | cut -d' ' -f1)

    if [ -f "$_stamp" ] && [ "$(cat "$_stamp" 2>/dev/null)" = "$_want" ]; then
        echo "$_rel already staged at this revision — leaving it (patched state intact)"
        return 1
    fi

    echo "=== staging $_rel (Remora-owned) ==="
    rm -rf "$_dest"
    mkdir -p "$(dirname "$_dest")"
    cp -a "$_src" "$_dest"
    printf '%s\n' "$_want" > "$_stamp"

    # Anything that augments the staged copy must land BEFORE the commit — see add_lineage_art.
    # Written as an if rather than `[ ... ] && ...`: under set -e a failing test as the last
    # command of a function returns non-zero, and this hook must never decide the exit status.
    if [ "$_rel" = "device/remora" ]; then
        add_lineage_art "$_dest"
    fi

    # The pristine baseline as a commit, so the patch pass has something to reset to. Identity is
    # supplied inline: this may run before any global git identity exists on a build host.
    rm -rf "$_dest/.git"
    git -C "$_dest" init -q
    git -C "$_dest" add -A
    git -C "$_dest" -c user.name=Remora -c user.email=remora@localhost \
        commit -q -m "$_rel staged from vendor/source-build/$(basename "$_src")"

    # The content changed, so anything the patch pass recorded about this project is stale.
    rm -f "$TREE/.remora-incremental.d/$_slug" "$TREE/.remora-patch-state.d/$_slug"
    echo "$_rel staged ($(find "$_dest" -type f | wc -l) files); patch marks cleared"
    return 0
}

stage_project "$SRC" "device/remora" "device-remora" || true

# vendor/remora (R2): the container's init behaviour, GPU bring-up and boot-property namespace.
# Staged even when device/remora was already current — they version independently.
if [ -d "$VENDOR_SRC" ]; then
    stage_project "$VENDOR_SRC" "vendor/remora" "vendor-remora" || true
else
    echo "  (no vendored vendor/remora at $VENDOR_SRC — skipping)"
fi
