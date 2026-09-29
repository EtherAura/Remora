#!/bin/sh
# remora-arm-fetch.sh <container> <package> — fetch a Play app's ARM64 build and sideload it into a
# Remora container as arm64, so it runs under the native bridge.
#
# Why this exists: some Play apps ship a BROKEN x86_64 split. The worst case is Flutter apps whose
# x86_64 config split omits libflutter.so/libapp.so — Play prefers the native x86_64 split, delivers
# the gutted one, and the app crashes on launch. The arm64 split is complete. Play can't be made to
# deliver arm64 to an x86_64 host: every Play ABI decision — live split delivery and the device
# config uploaded at checkin — reads the NATIVE ro.product.cpu.abilist. Spoofing the Java
# Build.SUPPORTED_ABIS inside GmsCore/Play changes nothing; PLT-hooking the native property read is
# refused by the ptrace-based ReZygisk; and reordering the abilist device-wide makes everything run
# translated and crash-loops zygote when arm64-only. All three were built and measured. So instead
# we pull the arm64 build OURSELVES from Google's servers and install it forcing --abi arm64-v8a.
#
# Auth: reuses the container's OWN Google login — the AAS master token is read straight out of the
# device's accounts_ce.db, so there's no browser OAuth step and no stored credential. apkeep's default
# device profile is arm64, so it fetches the arm64 splits.
#
# Requires on the host: apkeep (`cargo install apkeep`) and sqlite3. The container must be logged into
# a Google account. This is the "host-side Play downloader" — Aurora's mechanism, Remora-native, no
# Aurora app on the device.
set -e
C="$1"; PKG="$2"
[ -n "$C" ] && [ -n "$PKG" ] || { echo "usage: remora-arm-fetch.sh <container> <package>|--update-all"; exit 2; }

# --update-all: update the ARM64-INJECTED apps, and only those.
#
# arm64-primary + shell-installed = an app this script put there. Re-fetching IS their update
# path: the shell's update-ownership claim (below) blocks Play's silent auto-update, so new
# versions cannot arrive any other way.
#
# EVERY OTHER APP IS DELIBERATELY LEFT TO PLAY. They are Play-owned x86_64 installs, and Play
# updates them on the device by itself — sweeping them here did no work Play was not already
# doing. It also could not work: fetching an x86_64 build means asking apkeep for
# -o device=google_kiwi_x86_64, the only x86_64 profile gpapi 6.1.0 ships, and measured
# over three packages that profile returns ZERO files for every one of them while the
# default (arm64) profile downloads the full split set for all three — and apkeep exits 0 either
# way (bd remora-rvf). The profile describes a Cuttlefish emulator ('HPE device',
# hardware=cutf_cvm) that Play appears to refuse delivery to. So the x86_64 arm of this sweep
# failed for ~32 of 33 apps on its first use and reported the whole run as a failure.
# DO NOT RE-ADD IT without first proving a device profile Play will actually serve x86_64 to.
#
# Sequential; one app failing doesn't stop the sweep.
if [ "$PKG" = "--update-all" ]; then
  echo "scanning $C for third-party apps…"
  ARMS=$(docker exec "$C" sh -c 'for p in $(pm list packages -3 | sed s/^package://); do
    dumpsys package $p 2>/dev/null | grep -q primaryCpuAbi=arm64-v8a || continue
    dumpsys package $p 2>/dev/null | grep -qE "installerPackageName=(com.android.shell|null)" && echo $p
  done')
  NALL=$(docker exec "$C" sh -c 'pm list packages -3 | sed s/^package://' | grep -c . || true)
  # arm64 apps PLAY owns. The selector above deliberately excludes them (the rvf scoping), but they
  # are not the same thing as the x86_64 residual and must not be counted into it: an arm64 app on
  # Play's update ownership is the exposed class in bd remora-4ei.91 — Play may push the x86_64
  # split over it at any update, and the arm guard only repairs that AFTER the app is broken.
  # Folding them into "Play-owned x86_64 installs" told the operator the one thing that is not
  # true about them, and hid the only state this sweep could have surfaced for free.
  PLAYARM=$(docker exec "$C" sh -c 'for p in $(pm list packages -3 | sed s/^package://); do
    dumpsys package $p 2>/dev/null | grep -q primaryCpuAbi=arm64-v8a || continue
    dumpsys package $p 2>/dev/null | grep -qE "installerPackageName=(com.android.shell|null)" || echo $p
  done')
  NPLAYARM=$(echo "$PLAYARM" | grep -c . || true)
  # Named once, here, so both exit paths say the same thing.
  playarm_note() {
    [ "$NPLAYARM" -gt 0 ] || return 0
    echo "  NOTE: $NPLAYARM arm64 app(s) are on PLAY's update ownership:$(echo " $PLAYARM" | tr '\n' ' ')" >&2
    echo "  Play can replace those with the x86_64 split at any update; the arm guard repairs them" >&2
    echo "  on the next Connect, so they are broken in between. To claim one for the shell now, use" >&2
    echo "  Import / export… → Install ARM app from Google Play (uninstall -k keeps the data)." >&2
  }
  if [ -z "$ARMS" ]; then
    echo "no arm64-injected apps in $C — nothing to update here."
    echo "  ($((NALL - NPLAYARM)) third-party app(s) are Play-owned x86_64 installs; Play updates those on the device.)"
    playarm_note
    exit 0
  fi
  echo "arm64-injected:" $ARMS
  rc=0
  ok=0; bad=0; failed=""
  for p in $ARMS; do
    echo "=== $p ==="
    sh "$0" "$C" "$p" && ok=$((ok + 1)) \
      || { echo "WARN: update failed for $p" >&2; bad=$((bad + 1)); failed="$failed $p"; rc=1; }
  done
  # A per-app WARN scrolled past says nothing about the SHAPE of the outcome, and the shape is the
  # whole diagnosis: "1 of 33" is a broken request, "32 of 33" is one odd app. The sweep reporting
  # only a bare exit code is what left bd remora-rvf with no failing stage at all. The skipped
  # count is named too, so "nothing happened to my other apps" is answered before it is asked.
  echo "update sweep: $ok updated, $bad failed$([ -n "$failed" ] && echo " —$failed"); \
$((NALL - ok - bad - NPLAYARM)) Play-owned x86_64 app(s) left to Play's own updater"
  playarm_note
  exit $rc
fi
command -v sqlite3 >/dev/null || { echo "need sqlite3 on the host"; exit 1; }
APKEEP="$(command -v apkeep || echo "$HOME/.cargo/bin/apkeep")"
[ -x "$APKEEP" ] || { echo "need apkeep (cargo install apkeep)"; exit 1; }

WORK=$(mktemp -d)
# Cleanup must survive its own failures. shred exits non-zero for any of these files that was
# never created (the .pb is only fetched on some paths), and with `set -e` that aborted the trap
# BEFORE rm -rf — leaking the work dir, and worse, making a SUCCESSFUL run exit 1 because the
# trap's status became the script's. The caller reads that exit code to decide whether the fetch
# worked, so a good fetch was being reported as a failure.
cleanup() {
    rc=$?
    shred -u "$WORK/acc.db" "$WORK/acc.db-wal" "$WORK/gads.pb" 2>/dev/null || :
    rm -rf "$WORK"
    exit "$rc"
}
trap cleanup EXIT

# --- 1. pull the Google account + AAS master token out of the container -----------------------------
# The account name comes from dumpsys: no database copy, and it is stable across GMS versions.
EMAIL=$(docker exec "$C" sh -c 'dumpsys account 2>/dev/null' \
        | sed -n 's/.*Account {name=\([^,]*\), *type=com\.google}.*/\1/p' | head -1)

# The aas_et/… master token has moved between GMS versions, so try both homes.
#
# Older GMS keeps it in accounts_ce.db (credential-encrypted storage, but plain sqlite to root) in
# the com.google account's "password" column. That database is in WAL Mode: copying the .db WITHOUT
# its -wal sidecar yields a file whose tables all read back missing ("no such table: accounts"),
# which looks exactly like "not logged in" — so bring the sidecar along. The container's own sqlite3
# is broken, hence parsing on the host.
docker exec "$C" sh -c 'cat /data/system_ce/0/accounts_ce.db'     > "$WORK/acc.db"     2>/dev/null
docker exec "$C" sh -c 'cat /data/system_ce/0/accounts_ce.db-wal' > "$WORK/acc.db-wal" 2>/dev/null
[ -n "$EMAIL" ] || EMAIL=$(sqlite3 "$WORK/acc.db" \
    "SELECT name FROM accounts WHERE type='com.google' LIMIT 1;" 2>/dev/null)
TOKEN=$(sqlite3 "$WORK/acc.db" \
    "SELECT password FROM accounts WHERE type='com.google' AND password LIKE 'aas_et/%' LIMIT 1;" 2>/dev/null)

# Newer GMS (the A17-era build) leaves accounts.password empty and stores the master token in a
# protobuf datastore instead. It is not worth a protobuf parser: the token is a plain string with a
# distinctive prefix, so scrape it. -a because the file is binary.
if [ -z "$TOKEN" ]; then
    docker exec "$C" sh -c \
        'cat /data/data/com.google.android.gms/files/authaccount/shared/GoogleAccountDataStore.pb' \
        > "$WORK/gads.pb" 2>/dev/null
    TOKEN=$(LC_ALL=C grep -ao 'aas_et/[A-Za-z0-9_/+=-]*' "$WORK/gads.pb" 2>/dev/null | head -1)
fi

# The master token is long-lived but Google can start refusing it outright ("Invalid payload" at
# login) while the account on the device is perfectly healthy — and nothing we can do from here
# renews it. So prefer the token GMS ALREADY KEEPS FRESH: the Play-scoped oauth2 token that lives
# in the same accounts_ce.db, under authtokens, for
# https://www.googleapis.com/auth/googleplay. apkeep takes it directly via --auth-token.
#
# It is short-lived (a ya29.* bearer, ~1h), which is exactly why this is read at every fetch rather
# than cached anywhere: GMS re-mints it whenever the device does anything with Play, so on a device
# that is actually in use there is a valid one sitting there. The master token stays as the
# fallback for the case where no Play-scoped token has been minted yet (bd remora-e5x.31).
AUTHTOKEN=$(sqlite3 "$WORK/acc.db" \
    "SELECT authtoken FROM authtokens WHERE type LIKE '%auth/googleplay%' LIMIT 1;" 2>/dev/null)

if [ -z "$EMAIL" ] || { [ -z "$TOKEN" ] && [ -z "$AUTHTOKEN" ]; }; then
    echo "no Google account with a usable Play token in $C" >&2
    echo "  account found : ${EMAIL:-<none>}" >&2
    echo "  master token  : ${TOKEN:+found}${TOKEN:-not found}" >&2
    echo "  play token    : ${AUTHTOKEN:+found}${AUTHTOKEN:-not found}" >&2
    echo "  Sign in to Google on the device (Settings > Passwords & accounts), then retry." >&2
    echo "  If the account is signed in but no token is found, GMS may have moved it again:" >&2
    echo "  find it with  docker exec $C sh -c 'grep -rl aas_et /data/system_ce /data/data/com.google.android.gms 2>/dev/null'" >&2
    exit 1
fi
echo "account: $EMAIL"

# --- 2. download the split set from Google ----------------------------------------------------------
# apkeep's DEFAULT device profile is arm64, which is exactly what this script wants — it is the
# only profile measured to deliver anything at all (see the --update-all note above).
#
# BUT THE PROFILE ALSO FIXES THE DENSITY, and Play selects the config.<density> split from it. The
# default px_9a is 420 dpi (xxhdpi); this container is whatever dpi= says in remorarc, 320 (xhdpi)
# on the A17 instance. Measured on com.zhiliaoapp.musically: px_9a delivers
# config.xxhdpi, px_tablet delivers config.xhdpi, everything else about the two sets identical.
# Wrong-bucket resources still WORK — Android rescales them — so this was never a correctness bug,
# just every app carrying art for a screen this device does not have.
#
# Only arm64-v8a-ONLY profiles are listed. That is deliberate and load-bearing: a profile that also
# advertises x86_64 lets Play offer the x86_64 split set, which is the exact delivery this whole
# script exists to avoid (see the header). Densities are the gpapi 6.1 device.properties values.
#
# NOT A GENERAL SPLIT GAP: the profile changes WHICH density split arrives, never whether config or
# locale splits arrive at all. Same measurement, both profiles: TikTok 8 APKs including config.en,
# Super Mario Run 3 APKs with no density or locale split in either — that app genuinely ships none
# (a Unity title), so a 3-APK install of it is COMPLETE, not truncated (bd remora-rve.2).
DENSITY=$(docker exec "$C" sh -c 'getprop ro.sf.lcd_density' 2>/dev/null | tr -dc '0-9')
if [ -n "${REMORA_APKEEP_DEVICE:-}" ]; then
  DEV="$REMORA_APKEEP_DEVICE"
else
  # Bucket boundaries are the midpoints between Android's xhdpi/xxhdpi/xxxhdpi anchors (320/480/640).
  case "${DENSITY:-0}" in
    ''|0)          DEV=px_9a     ;;   # unreadable — keep apkeep's own default
    *) if   [ "$DENSITY" -lt 400 ]; then DEV=px_tablet   # 320, xhdpi
       elif [ "$DENSITY" -lt 560 ]; then DEV=px_9a       # 420, xxhdpi
       else                              DEV=sm_s25u     # 560, xxxhdpi
       fi ;;
  esac
fi
echo "fetching $PKG (arm64, device profile $DEV for ${DENSITY:-unknown} dpi) from Google Play…"
OPTS="split_apk=1,device=$DEV"
mkdir -p "$WORK/dl"   # apkeep requires the output dir to already exist
# Capture apkeep's output instead of discarding it. It used to go to /dev/null, which made every
# failure indistinguishable from every other: the script would exit non-zero with nothing to say,
# the caller reported "ARM install failed — see the Log pane", and the Log pane held only the two
# progress lines above. Diagnosing a rejected Play token meant re-running apkeep by hand.
# --auth-token (the Play-scoped bearer) when we have one, -t (the aas_et master token) otherwise.
if [ -n "$AUTHTOKEN" ]; then
  set -- --auth-token "$AUTHTOKEN"
else
  set -- -t "$TOKEN"
fi
# A LOGIN REJECTION IS OFTEN TRANSIENT, so retry it rather than believing the first one. Measured
# a sweep fetch failed with "Could not log in … Invalid payload" at 19:57, eleven
# minutes after an identical fetch succeeded, and the SAME token (fingerprint unchanged, re-read
# from the device between attempts) downloaded fine minutes later. Nothing about the account had
# changed. Treating the first rejection as final is what sent the user to re-authenticate a login
# that was never broken — and it fails a whole unattended sweep on one blip.
# Only this error class is retried: a delivery that simply is not there must still fail fast.
FETCH_TRIES="${REMORA_FETCH_TRIES:-3}"
FETCH_DELAY="${REMORA_FETCH_DELAY:-5}"
attempt=1
delay="$FETCH_DELAY"
while :; do
  if "$APKEEP" -a "$PKG" -d google-play -e "$EMAIL" "$@" -o "$OPTS" --accept-tos \
          "$WORK/dl" >"$WORK/apkeep.log" 2>&1; then
    break
  fi
  if grep -qE "Could not log in|Invalid payload|credentials" "$WORK/apkeep.log" 2>/dev/null \
     && [ "$attempt" -lt "$FETCH_TRIES" ]; then
    echo "  Play rejected the token (attempt $attempt of $FETCH_TRIES) — retrying in ${delay}s…"
    sleep "$delay"
    delay=$((delay * 3))
    attempt=$((attempt + 1))
    continue
  fi
  echo "apkeep could not fetch $PKG:" >&2
  sed 's/^/    /' "$WORK/apkeep.log" >&2
  if grep -qE "Could not log in|Invalid payload|credentials" "$WORK/apkeep.log" 2>/dev/null; then
    echo "  -> Google Play rejected the device's login token $FETCH_TRIES times." >&2
    echo "     These rejections are usually transient and clear by themselves; if this one does" >&2
    echo "     not, open the Play Store on the device and check the account is still signed in." >&2
    echo "     The token is re-read from the device at each fetch, so nothing here needs changing" >&2
    echo "     once it is healthy again." >&2
  fi
  exit 1
done
DIR="$WORK/dl/$PKG"
# apkeep 1.0.0 EXITS 0 WHEN PLAY DELIVERS NOTHING AT ALL — it prints "Downloading <pkg>…", omits
# its "downloaded successfully!" line, writes no file, and reports success. So an empty directory
# is the only evidence, and it means something categorically different from "this app has no split
# for the ABI we asked about": nothing arrived, for any ABI (bd remora-rvf). Separated because the
# per-app message sent the last diagnosis at the app instead of at the request.
if [ -z "$(ls -A "$DIR" 2>/dev/null)" ]; then
  echo "Google Play delivered NOTHING for $PKG (apkeep reported success and wrote no file)." >&2
  echo "  -> Nothing arrived for ANY ABI, so this is the request or the account, not the app's" >&2
  echo "     split set. Check the device's Play login, and whether $PKG is available to it." >&2
  exit 1
fi
ARM=$(ls "$DIR"/*arm64_v8a*.apk 2>/dev/null | head -1)
# Reached only when SOMETHING arrived, so this really is about the app's split set.
[ -f "$ARM" ] || { echo "no arm64 split delivered for $PKG (arm-incompatible app?)"; exit 1; }

# --- 3. streamed install session (no bind-mount needed) ---------------------------------------------
# Stream each APK straight into the PackageInstaller session over docker exec stdin.
OLD=$(docker exec "$C" sh -c "dumpsys package $PKG 2>/dev/null | grep -m1 versionName | sed 's/.*versionName=//;s/ .*//'")
# --update-ownership (A14+): the shell claims update ownership at this initial install, so Play
# can no longer SILENTLY auto-update the package back to its broken x86_64 split set (non-owner
# updates need explicit user consent; Play's auto-updater skips such apps). Ownership is only
# claimable at initial install — which this is, thanks to the uninstall below — and shell
# sessions (our re-fetches, the sonar-guard repairs) are exempt from the enforcement.
SID=$(docker exec "$C" sh -c "pm install-create --abi arm64-v8a -r --update-ownership" | grep -oE '\[[0-9]+\]' | tr -d '[]')
[ -n "$SID" ] || { echo "could not create install session"; exit 1; }
# Keep the app's identity across the reinstall: -k preserves /data/data (login state, settings),
# and the granted runtime permissions are snapshotted here and re-granted after the install (a
# full uninstall resets them). The uninstall itself is what makes the next install "initial",
# which is the only moment update ownership can be claimed.
PERMS=$(docker exec "$C" sh -c "dumpsys package $PKG 2>/dev/null" | sed -n 's/^ *\([a-zA-Z0-9_.]*\): granted=true.*/\1/p' | sort -u)
docker exec "$C" sh -c "pm uninstall -k $PKG" >/dev/null 2>&1 || true
n=0
for apk in "$DIR"/*.apk; do
  n=$((n+1))
  sz=$(stat -c%s "$apk")
  docker exec -i "$C" sh -c "pm install-write -S $sz $SID split$n -" < "$apk" >/dev/null
done
echo "committing $n splits…"
docker exec "$C" sh -c "pm install-commit $SID"

# --- 4. re-grant + report ---------------------------------------------------------------------------
for perm in $PERMS; do
  docker exec "$C" sh -c "pm grant $PKG $perm" >/dev/null 2>&1 || true  # non-runtime perms no-op
done
ABI=$(docker exec "$C" sh -c "dumpsys package $PKG 2>/dev/null | grep -m1 primaryCpuAbi | sed 's/.*primaryCpuAbi=//;s/ .*//'")
NEW=$(docker exec "$C" sh -c "dumpsys package $PKG 2>/dev/null | grep -m1 versionName | sed 's/.*versionName=//;s/ .*//'")
OWNER=$(docker exec "$C" sh -c "dumpsys package $PKG 2>/dev/null | grep -m1 updateOwnerPackageName | sed 's/.*=//'")
if [ -z "$OLD" ]; then echo "installed $PKG $NEW  primaryCpuAbi=$ABI"
elif [ "$OLD" = "$NEW" ]; then echo "reinstalled $PKG $NEW (already current)  primaryCpuAbi=$ABI"
else echo "updated $PKG $OLD -> $NEW  primaryCpuAbi=$ABI"; fi
if [ "$OWNER" = "com.android.shell" ]; then
  echo "update ownership: com.android.shell — Play cannot silently auto-update this app;"
  echo "  new versions come from re-running this script (Remora: 'Update ARM apps')"
else
  echo "WARN: update ownership not recorded — Play may still silently auto-update $PKG." >&2
  echo "  Expected on images built before the shell_update_owner patch: stock Android drops a" >&2
  echo "  claim when app data was kept (pm uninstall -k keeps the PackageSetting). Rebuild+push" >&2
  echo "  the image, then re-run this check to claim ownership without losing app data. Until" >&2
  echo "  then the sonar-guard watcher keeps repairing Play strikes within a minute." >&2
fi
