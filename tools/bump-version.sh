#!/usr/bin/env bash
# Bump Remora's version everywhere it is written, in one step, so a release cannot go out with the
# PKGBUILD on one version and the AppStream metainfo on another.
#
#   tools/bump-version.sh 2026.11.0
#
# CalVer, YYYY.MM.PATCH: the year and two-digit month of the release, then a patch number that
# starts at 0. Local edits only — no commit, no tag, no push:
#
#   VERSION                                            overwritten; CMake reads it
#   packaging/PKGBUILD                                 pkgver= set, pkgrel= back to 1
#   packaging/io.github.EtherAura.Remora.metainfo.xml  <release version= date=/> prepended
#   CHANGELOG.md                                       "## [<version>] - <date>" opened under
#                                                      [Unreleased], so what was unreleased is now
#                                                      this release; the compare links follow
#
# The date is today's, in UTC. release.yml refuses a tag these files disagree with, and publishes
# the CHANGELOG section as the release notes — so write it before tagging. The PKGBUILD's
# sha256sums stays 'SKIP': release.yml stamps the real sum into the copy it attaches to the release.
set -euo pipefail

usage() {
    echo "usage: $0 <YYYY.MM.PATCH>   e.g. $0 2026.11.0" >&2
    exit 1
}
[ "$#" -eq 1 ] || usage
NEW="$1"
[[ "$NEW" =~ ^[0-9]{4}\.(0[1-9]|1[0-2])\.[0-9]+$ ]] || {
    echo "bump-version: '$NEW' is not YYYY.MM.PATCH (two-digit month, e.g. 2026.09.0)" >&2
    exit 1
}

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"
METAINFO=packaging/io.github.EtherAura.Remora.metainfo.xml
for f in VERSION packaging/PKGBUILD "$METAINFO" CHANGELOG.md; do
    [ -f "$f" ] || { echo "bump-version: $ROOT/$f is missing" >&2; exit 1; }
done

OLD=$(tr -d '[:space:]' < VERSION)
if [ "$OLD" = "$NEW" ]; then
    echo "VERSION is already $NEW — nothing to do."
    exit 0
fi
# Versions only move forward: a lower one would sort as older to pacman, dpkg and AppStream alike.
if [ "$(printf '%s\n%s\n' "$OLD" "$NEW" | sort -V | tail -n 1)" != "$NEW" ]; then
    echo "bump-version: $NEW is not newer than the current $OLD" >&2
    exit 1
fi
TODAY=$(date -u +%Y-%m-%d)
echo "Bumping $OLD -> $NEW ($TODAY)"

printf '%s\n' "$NEW" > VERSION
sed -i -E -e "s/^pkgver=.*/pkgver=${NEW}/" -e "s/^pkgrel=.*/pkgrel=1/" packaging/PKGBUILD

python3 - "$METAINFO" CHANGELOG.md "$OLD" "$NEW" "$TODAY" <<'PY'
import re
import sys

metainfo, changelog, old, new, today = sys.argv[1:]

# Metainfo: the newest release goes first, which is also what release.yml reads.
with open(metainfo, encoding="utf-8") as f:
    xml = f.read()
if re.search(rf'<release\s+version="{re.escape(new)}"', xml):
    print(f"  {metainfo}: <release version=\"{new}\"> already present")
else:
    indent = re.search(r"^( *)<release\b", xml, re.M)
    indent = indent.group(1) if indent else "    "
    xml, n = re.subn(r"(<releases>[^\n]*\n)",
                     rf'\1{indent}<release version="{new}" date="{today}"/>\n', xml, count=1)
    if n != 1:
        sys.exit(f"bump-version: no <releases> element in {metainfo}")
    with open(metainfo, "w", encoding="utf-8") as f:
        f.write(xml)

# CHANGELOG: the new heading goes directly under [Unreleased], so the entries collected there
# become this release's; a fresh, empty [Unreleased] stays on top.
with open(changelog, encoding="utf-8") as f:
    md = f.read()
repo = "https://github.com/EtherAura/Remora"
if re.search(rf"^## \[{re.escape(new)}\]", md, re.M):
    print(f"  {changelog}: ## [{new}] already present")
else:
    heading = f"## [{new}] - {today}"
    md, n = re.subn(r"^## \[Unreleased\][^\n]*\n", rf"\g<0>\n{heading}\n", md, count=1, flags=re.M)
    if n != 1:
        md, n = re.subn(r"^## \[", f"{heading}\n\n## [", md, count=1, flags=re.M)
    if n != 1:
        sys.exit(f"bump-version: no '## [' heading in {changelog} to put {new} above")
    md = re.sub(r"^\[Unreleased\]: .*$", f"[Unreleased]: {repo}/compare/v{new}...HEAD",
                md, count=1, flags=re.M)
    link = f"[{new}]: {repo}/compare/v{old}...v{new}"
    md, n = re.subn(r"^(\[Unreleased\]: [^\n]*\n)", rf"\1{link}\n", md, count=1, flags=re.M)
    if n != 1:
        md = md.rstrip("\n") + f"\n\n{link}\n"
    with open(changelog, "w", encoding="utf-8") as f:
        f.write(md)
PY

cat <<EOF

Version bumped to $NEW. Changed:
  VERSION
  packaging/PKGBUILD
  $METAINFO
  CHANGELOG.md

Write the release notes under "## [$NEW] - $TODAY" in CHANGELOG.md (release.yml publishes that
section as the release body, and fails if it is empty), optionally a <description> for the new
<release> in the metainfo, then review with git diff, commit, and tag:
  git tag v$NEW
EOF
