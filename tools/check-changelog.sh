#!/usr/bin/env bash
# Does a change that users can notice come with its CHANGELOG.md entry?
#
#   tools/check-changelog.sh <base> [<head>]        head defaults to HEAD
#
# Run by .github/workflows/changelog.yml on every pull request and every push to main, and by hand
# before opening a pull request:
#
#   tools/check-changelog.sh origin/main
#
# What users can notice is what ships to them: the desktop side (src/), the in-image agent
# (agent/), and everything deployed to a host or built into an image (vendor/). Tests, the docs,
# the workflows, packaging metadata and these tools are not checked.
#
# A change that touches those paths passes when it also adds at least one line to CHANGELOG.md.
# One that is internal (a refactor, a comment, a rename nobody outside can see) says so instead:
#
#   pull request   the `no-changelog` label
#   commit         a line reading exactly [no-changelog] in the commit message
#
# The marker counts only alone on its line, and only for the commit that carries it: a message that
# merely mentions it opts nothing out, and a marked commit beside unmarked work does not cover that
# work. With CHANGELOG_EXEMPT=1 in the environment (how the workflow passes the label on) the whole
# range is exempt.
set -euo pipefail

BASE="${1:?usage: check-changelog.sh <base> [<head>]}"
HEAD="${2:-HEAD}"

note() { printf '%s\n' "check-changelog: $*"; }

if [ "${CHANGELOG_EXEMPT:-0}" = "1" ]; then
    note "exempt (the no-changelog label)"
    exit 0
fi

RANGE_BASE=$(git merge-base "$BASE" "$HEAD")

# The commits that have to answer for themselves: every one in the range without the marker.
unmarked=()
while IFS= read -r sha; do
    [ -n "$sha" ] || continue
    if git log -1 --format=%B "$sha" | grep -qE '^[[:space:]]*\[no-changelog\][[:space:]]*$'; then
        note "$(git log -1 --format='%h %s' "$sha"): carries [no-changelog], exempt"
    else
        unmarked+=("$sha")
    fi
done < <(git rev-list "${RANGE_BASE}..${HEAD}")

if [ "${#unmarked[@]}" -eq 0 ]; then
    note "nothing to check: the range is empty or every commit carries [no-changelog]"
    exit 0
fi

# What each of them changed against its FIRST parent. For a merge that is what the merge brought
# to this branch; diffing against every parent would also list everything main gained since the
# merged branch was cut, and no edit to the change under review could ever answer for that.
changed=$(for sha in "${unmarked[@]}"; do
    if git rev-parse -q --verify "${sha}^1" >/dev/null 2>&1; then
        git diff --name-only "${sha}^1" "$sha"
    else
        git diff-tree -r --no-commit-id --name-only --root "$sha"
    fi
done | sort -u)

visible=$(printf '%s\n' "$changed" | grep -E '^(src|agent|vendor)/' || true)
if [ -z "$visible" ]; then
    note "nothing under src/, agent/ or vendor/ changed: no entry needed"
    exit 0
fi

added=0
if printf '%s\n' "$changed" | grep -qx 'CHANGELOG.md'; then
    # Added lines with something on them. Indented ones count: the continuation lines of a wrapped
    # bullet are how an existing entry grows.
    added=$(git diff "$RANGE_BASE" "$HEAD" -- CHANGELOG.md \
        | awk '/^\+\+\+ / { in_file = 1; next } in_file && /^\+[^+]/ { print }' \
        | grep -cE '^\+[[:space:]]*[^[:space:]]' || true)
fi

if [ "$added" -gt 0 ]; then
    note "CHANGELOG.md gains $added line(s)"
    exit 0
fi

{
    note "these change what users get, and CHANGELOG.md gains nothing:"
    printf '%s\n' "$visible" | sed 's/^/    /'
    note "add an entry under [Unreleased], written for users. If nobody outside could notice the"
    note "change, give the pull request the no-changelog label, or put [no-changelog] alone on a"
    note "line of the commit message."
} >&2
exit 1
