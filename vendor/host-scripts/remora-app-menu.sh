#!/bin/bash
# remora-app-menu.sh — generate a desktop-menu folder from a running Remora device: one
# .desktop per installed app with the app's REAL icon, grouped into a launcher folder NAMED AFTER
# THE PROFILE, so multiple profiles can be integrated side by side. Icons are
# rendered ON THE DEVICE (the agent's IconDump via app_process) so even vector adaptive icons —
# which can't be extracted from an APK — come out correct.
#
# Generate/refresh: remora-app-menu.sh <adb-target> <exec-template> <profile-name> \
#                                       [<mirror-exec>] [<desktop-exec>] [<pip-exec>]
#   <exec-template>: the .desktop Exec line, with %PKG% / %NAME% substituted per app,
#                    e.g.  "/usr/bin/remora" app "Android 16" %PKG% --title "%NAME%"
#   <mirror-exec>/<desktop-exec>/<pip-exec>: optional Exec lines for the folder's plain-device
#                    entries — "Android (mirror)", "Android (desktop)" and "Android (PIP)".
# Remove:           remora-app-menu.sh --remove <profile-name>
#
# Env: ICONDUMP_DEX (fallback renderer for images whose agent predates IconDump; default:
#      icondump.dex alongside this script, which a public checkout does not have),
#      REMORA_MENU_SETTLE_TRIES / REMORA_MENU_SETTLE_SLEEP (the boot-settling retry loop)
#
# Exit codes: 0 refreshed; 3 device not settled — the EXISTING menu was kept on purpose
# (callers report a skip, not a failure; bd remora-5sd); anything else is a real failure.
set -u

APPDIR="$HOME/.local/share/applications"
ICONROOT="$HOME/.local/share/icons/remora"
DIRDIR="$HOME/.local/share/desktop-directories"
MENUDIR="$HOME/.config/menus/applications-merged"

# Profile name -> filesystem/category slug (must match appMenuSlug() in src/engine/AppMenu.cpp).
slugify() { printf '%s' "$1" | tr '[:upper:]' '[:lower:]' | sed 's/[^a-z0-9]\+/-/g; s/^-//; s/-$//'; }
esc() { printf '%s' "$1" | sed 's/[\\]/\\\\/g'; }
# For use INSIDE a sed replacement: also neutralize &, | (delimiter) and backslashes.
sedesc() { printf '%s' "$1" | sed 's/[&|\\]/\\&/g'; }

# Remove one profile's .desktop entries: matched by their category line (not by filename glob —
# a slug that prefixes another slug would over-match).
remove_entries() {
    grep -lZ "^Categories=X-Remora-$1;\$" "$APPDIR"/remora-*.desktop 2>/dev/null \
        | xargs -0r rm -f
}

# Remove a whole integration: entries + icons + the menu folder itself.
remove_profile() {
    remove_entries "$1"
    rm -rf "${ICONROOT:?}/$1"
    rm -f "$DIRDIR/remora-$1.directory" "$MENUDIR/remora-$1.menu"
}

# Drop artifacts of the retired first format — un-profiled, flat icons and a single shared folder —
# which the regenerate below never writes, so they would sit in the menu as duplicates forever.
cleanup_legacy() {
    grep -lZ '^Categories=X-Remora;$' "$APPDIR"/remora-*.desktop 2>/dev/null | xargs -0r rm -f
    find "$ICONROOT" -maxdepth 1 -type f -name '*.png' -delete 2>/dev/null
}

# The folder's plain-device entries + the folder itself. NONE of this needs the device: it is pure
# profile metadata, unlike the app list. Kept in functions so --plain can write them for a profile
# that is not running — otherwise a profile only ever gets its mirror/desktop/PIP buttons in the
# window where its container happens to be up, which is exactly when you don't need a launcher.
write_plain_entries() {
    REMORA_ICON=phone
    [ -e /usr/share/icons/hicolor/scalable/apps/io.github.EtherAura.Remora.svg ] && REMORA_ICON=io.github.EtherAura.Remora
    _plain_entry() {  # <file-suffix> <name> <comment-tail> <exec> [<wmclass>]
        [ -n "$4" ] || return 0
        cat > "$APPDIR/remora-$SLUG-0-$1.desktop" <<EOF
[Desktop Entry]
Type=Application
Version=1.0
Name=$2
GenericName=Android device
Comment=$(esc "$PROFILE") — $3
Exec=$4
Icon=$REMORA_ICON
Terminal=false
Categories=X-Remora-$SLUG;
StartupNotify=false
EOF
        [ -n "${5:-}" ] && printf 'StartupWMClass=%s\n' "$5" >> "$APPDIR/remora-$SLUG-0-$1.desktop"
        echo "  $2"
    }
    _plain_entry mirror  'Android (mirror)'  'mirror the main display (no specific app)' "$EXEC_MIRROR"
    _plain_entry desktop 'Android (desktop)' 'a separate desktop-mode display (no specific app)' "$EXEC_DESKTOP"
    # StartupWMClass is the CONSTANT PIP app-id, not a per-profile one: one class is what lets a
    # single KWin/Krohnkite rule cover every profile's PIP. The entries stay distinct through their
    # Exec (and the window title), not through the class.
    _plain_entry pip     'Android (PIP)'     'toggle a small always-on-top second mirror' "$EXEC_PIP" remora-pip
}

# The menu folder: a .directory entry named after the profile + an XDG menu merge gathering this
# profile's category. One pair per profile — integrations coexist.
write_folder() {
    cat > "$DIRDIR/remora-$SLUG.directory" <<EOF
[Desktop Entry]
Type=Directory
Name=$(esc "$PROFILE")
Icon=phone
EOF
    cat > "$MENUDIR/remora-$SLUG.menu" <<EOF
<!DOCTYPE Menu PUBLIC "-//freedesktop//DTD Menu 1.0//EN" "http://www.freedesktop.org/standards/menu-spec/menu-1.0.dtd">
<Menu>
  <Name>Applications</Name>
  <Menu>
    <Name>Remora-$SLUG</Name>
    <Directory>remora-$SLUG.directory</Directory>
    <Include><And><Category>X-Remora-$SLUG</Category></And></Include>
  </Menu>
</Menu>
EOF
}

# --plain: refresh ONLY the device-level entries, leaving the app entries untouched. For a profile
# whose container is not running — the app list needs adb, these do not.
if [ "${1:-}" = "--plain" ]; then
    PROFILE="${2:?usage: remora-app-menu.sh --plain <profile> <mirror-exec> <desktop-exec> <pip-exec>}"
    EXEC_MIRROR="${3:-}"
    EXEC_DESKTOP="${4:-}"
    EXEC_PIP="${5:-}"
    SLUG=$(slugify "$PROFILE")
    mkdir -p "$APPDIR" "$DIRDIR" "$MENUDIR"
    write_plain_entries
    write_folder
    command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database "$APPDIR" 2>/dev/null
    echo "Refreshed the '$PROFILE' device entries (app list left as-is — device not queried)."
    exit 0
fi

if [ "${1:-}" = "--remove" ]; then
    PROFILE="${2:?usage: remora-app-menu.sh --remove <profile-name>}"
    SLUG=$(slugify "$PROFILE")
    remove_profile "$SLUG"
    cleanup_legacy
    command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database "$APPDIR" 2>/dev/null
    echo "Removed the '$PROFILE' app-menu folder."
    exit 0
fi

TARGET="${1:?usage: remora-app-menu.sh <adb-target> <exec-template> <profile-name>}"
EXEC_TMPL="${2:?usage: remora-app-menu.sh <adb-target> <exec-template> <profile-name>}"
PROFILE="${3:?usage: remora-app-menu.sh <adb-target> <exec-template> <profile-name>}"
EXEC_MIRROR="${4:-}"
EXEC_DESKTOP="${5:-}"
EXEC_PIP="${6:-}"
SLUG=$(slugify "$PROFILE")
[ -n "$SLUG" ] || { echo "profile name '$PROFILE' yields an empty slug"; exit 1; }
# The app list comes from the mirror client (remora mirror --list-apps); the agent answers it.
REMORA_BIN="${REMORA_BIN:-remora}"
SELFDIR="$(cd "$(dirname "$0")" && pwd)"
ICONDUMP_DEX="${ICONDUMP_DEX:-$SELFDIR/icondump.dex}"
ICONDIR="$ICONROOT/$SLUG"
mkdir -p "$APPDIR" "$ICONDIR" "$DIRDIR" "$MENUDIR"
cleanup_legacy

echo "Enumerating apps on $TARGET…"
# --list-apps output: " * Name  pkg" (system) / " - Name  pkg" (user). pkg is the last field.
enumerate() {
    RAW=$("$REMORA_BIN" mirror -s "$TARGET" --list-apps 2>/dev/null)
    APPS=$(printf '%s\n' "$RAW" \
      | awk '/^[[:space:]]*[-*][[:space:]]/ { pkg=$NF; $NF="";
             sub(/^[[:space:]]*[-*][[:space:]]+/,""); sub(/[[:space:]]+$/,"");
             if (pkg ~ /\./ && length($0)) print pkg"\t"$0 }')
    NAPPS=$(printf '%s\n' "$APPS" | grep -c .)
}
# Count this profile's current entries FIRST: an empty enumeration is a hard failure only when
# there is no menu to protect — with one, it is the settling condition below in its most extreme
# form and takes that path instead of failing outright (bd remora-5sd).
EXISTING=0
for f in "$APPDIR"/remora-*.desktop; do
    [ -f "$f" ] || continue
    grep -q "^Categories=X-Remora-$SLUG;\$" "$f" 2>/dev/null \
        && grep -q "^X-Remora-App=" "$f" 2>/dev/null && EXISTING=$((EXISTING + 1))
done

enumerate
[ -n "$APPS" ] || [ "$EXISTING" -gt 3 ] \
    || { echo "No apps found (is the device connected?)"; exit 1; }

# Boot-settling guard: right after sys.boot_completed the device can report a near-empty app
# list (intent resolution still warming) — regenerating from it would wipe the menu. If the list
# shrank to under half of the current entries, retry a while; still degenerate → keep the menu
# and exit 3, the "kept, not failed" code. This fires on cold deploys, where the step runs right
# after boot — reporting it as a failure trained the eye to ignore a step state that fails on
# every single run (bd remora-5sd), when keeping the menu is the guard doing its job.
SETTLE_TRIES="${REMORA_MENU_SETTLE_TRIES:-8}"
SETTLE_SLEEP="${REMORA_MENU_SETTLE_SLEEP:-8}"
if [ "$EXISTING" -gt 3 ]; then
    tries=0
    while [ $((NAPPS * 2)) -lt "$EXISTING" ] && [ "$tries" -lt "$SETTLE_TRIES" ]; do
        echo "  device reports only $NAPPS of $EXISTING apps (still booting?) — retrying…"
        sleep "$SETTLE_SLEEP"
        enumerate
        tries=$((tries + 1))
    done
    if [ $((NAPPS * 2)) -lt "$EXISTING" ]; then
        echo "Device still reports only $NAPPS of $EXISTING apps — keeping the existing menu."
        echo "(Refresh again once the device has settled.)"
        exit 3
    fi
fi
PKGS=$(printf '%s\n' "$APPS" | cut -f1)
echo "Found $NAPPS apps. Rendering icons on device…"

# Render every icon in one app_process pass (Android composites adaptive/vector icons correctly).
#
# NOTHING IS DELETED BEFORE THE RENDER SUCCEEDS. This used to open with `rm -f $ICONDIR/*.png`,
# which destroyed a perfectly good icon set to make room for one that might never arrive: a render
# that fails (device mid-boot, a framework restart, app_process refusing) then left the directory
# EMPTY, every entry fell back to Icon=phone, and nothing retried until the next refresh — so one
# unlucky moment genericised the menu for good. Measured, all 33 entries, and the damage
# reached BEYOND Remora: the desktop's task manager matches a window to a .desktop entry by name
# when the window class does not resolve, so a Chromium PWA sharing a name with an Android app
# (YouTube) inherited the generic phone icon too. Same rule the settle check already follows for
# the menu itself (exit 3): keep what works rather than replace it with nothing.
# The renderer is IconDump, which ships inside the agent jar of any image built with the
# mirror_agent feature. An image built before it moved there lacks the class, so fall back to the
# standalone dex when one is available; with neither, the entries stay generic (said below).
render_icons() {  # <classpath> <class>
    # shellcheck disable=SC2086
    adb -s "$TARGET" shell "CLASSPATH=$1 app_process /system/bin $2 /data/local/tmp/remora-icons $(printf '%s ' $PKGS)" </dev/null 2>/dev/null \
        | grep -c '^OK '
}
AGENT_JAR=/system_ext/framework/remora-agent.jar
adb -s "$TARGET" shell 'mkdir -p /data/local/tmp/remora-icons; rm -f /data/local/tmp/remora-icons/*.png' </dev/null 2>/dev/null
ok=$(render_icons "$AGENT_JAR" com.remora.agent.IconDump)
if [ "${ok:-0}" -eq 0 ] && [ -f "$ICONDUMP_DEX" ]; then
    adb -s "$TARGET" shell 'mkdir -p /data/local/tmp/remora-icondump' </dev/null 2>/dev/null
    adb -s "$TARGET" push "$ICONDUMP_DEX" /data/local/tmp/remora-icondump/classes.dex </dev/null >/dev/null 2>&1
    ok=$(render_icons /data/local/tmp/remora-icondump/classes.dex com.remora.IconDump)
fi
if [ "${ok:-0}" -gt 0 ]; then
    # Stage the pull, then promote. `adb pull <dir> <dest>` lays the files out differently
    # depending on whether <dest> already exists: missing -> flat in dest/, existing -> nested in
    # dest/remora-icons/. A staging dir that outlived one run (interrupt, or two syncs racing —
    # the GUI refreshes the Apps page on every visit) therefore made every LATER run nest, promote
    # nothing, and fail to rmdir a non-empty dir, so the menu stayed generic for good. Clear it
    # first and promote by search, so neither layout nor a leftover can strand the icons again.
    rm -rf "$ICONDIR/pull"
    adb -s "$TARGET" pull /data/local/tmp/remora-icons "$ICONDIR/pull" </dev/null >/dev/null 2>&1
    # A zero-byte PNG is a failed render, not an icon — it would satisfy the -s test below and
    # show as a blank tile, which reads as broken rather than as generic.
    rendered=$(find "$ICONDIR/pull" -name '*.png' -size +0c 2>/dev/null | wc -l)
    if [ "$rendered" -gt 0 ]; then
        find "$ICONDIR/pull" -name '*.png' -size +0c -exec mv {} "$ICONDIR" \; 2>/dev/null
        # Prune only now, and only what the live app list no longer contains: an app removed from
        # the device should lose its icon, but an app the render happened to MISS this round keeps
        # the icon it already had. Pruning on a failed render is exactly the bug above.
        for old in "$ICONDIR"/*.png; do
            [ -e "$old" ] || continue
            oldpkg=$(basename "$old" .png)
            printf '%s\n' $PKGS | grep -qxF "$oldpkg" || rm -f "$old"
        done
        echo "  rendered $rendered icon(s)"
    else
        echo "  (icon render produced nothing — KEEPING the icons already on disk; entries without"
        echo "   one stay generic until a render succeeds. Device mid-boot or framework restarting?)"
    fi
    rm -rf "$ICONDIR/pull"
else
    echo "  (icon render produced nothing — KEEPING the icons already on disk. Either the device is"
    echo "   mid-boot, or it has no renderer: the image predates IconDump in the agent and there is"
    echo "   no icondump.dex at $ICONDUMP_DEX. Entries without an icon stay generic.)"
fi

# Regenerate this profile's .desktop entries from scratch (drops uninstalled apps). Entries only —
# the icon dir holds the icons just rendered above.
remove_entries "$SLUG"
printf '%s\n' "$APPS" | while IFS=$'\t' read -r pkg name; do
    [ -n "$pkg" ] || continue
    if [ -s "$ICONDIR/$pkg.png" ]; then icon="$ICONDIR/$pkg.png"; tag=icon; else icon=phone; tag=generic; fi
    exec_line=$(printf '%s' "$EXEC_TMPL" | sed "s|%PKG%|$pkg|g; s|%NAME%|$(sedesc "$name")|g")
    # StartupWMClass matches the app-id/WM_CLASS remora sets on the mirror window, so the
    # desktop pairs the window with THIS entry — the app's own name + icon in the taskbar.
    cat > "$APPDIR/remora-$SLUG-$pkg.desktop" <<EOF
[Desktop Entry]
Type=Application
Version=1.0
Name=$(esc "$name")
GenericName=Android app
Comment=$(esc "$name") — $(esc "$PROFILE") ($pkg)
Exec=$exec_line
Icon=$icon
Terminal=false
Categories=X-Remora-$SLUG;
StartupNotify=false
StartupWMClass=remora-$SLUG-$pkg
X-Remora-App=$pkg
EOF
    printf '  %-28s %s  [%s]\n' "$name" "$pkg" "$tag"
done

# Folder-level entries for the plain device (no specific app): mirror, desktop mode and the PIP
# toggle. Prefixed "0-" so they sort to the top of the folder.
write_plain_entries
write_folder

command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database "$APPDIR" 2>/dev/null
echo "Generated the '$PROFILE' app-menu folder ($(printf '%s\n' "$PKGS" | grep -c .) apps)."
