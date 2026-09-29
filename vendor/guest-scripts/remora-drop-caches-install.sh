#!/bin/bash
# One-time root install of the post-load cache-drop helper on a docker host. Idempotent. Remora's
# image push and archive-load flows call it best-effort after a multi-GB `docker load` (bd
# remora-8xq); without it they skip the drop silently. Run it on the host that receives images:
#   sudo bash remora-drop-caches-install.sh [<user>]     (default: the user who ran sudo)
set -eu
cd "$(dirname "$0")"
U="${1:-${SUDO_USER:?run with sudo, or name the user Remora connects as}}"
install -m 0755 remora-drop-caches /usr/local/sbin/remora-drop-caches
RULE=$(mktemp)
printf '# Let Remora free the page cache after a multi-GB docker load (remora-drop-caches).\n%s ALL=(root) NOPASSWD: /usr/local/sbin/remora-drop-caches\n' "$U" > "$RULE"
visudo -c -f "$RULE" >/dev/null
install -m 0440 "$RULE" /etc/sudoers.d/remora-drop-caches
rm -f "$RULE"
echo "remora-drop-caches installed, sudoers rule active for $U"
