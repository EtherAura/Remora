# Vendored script provenance

The files below entered Remora from a private packaging overlay of container-operation scripts
that predates Remora and was never published. Each row records the name and the `sha256` of the
bytes as they arrived, so the origin of a line can be checked against what was actually received.

Formal licence attribution for upstream projects lives in `NOTICE`, which is the authoritative
record; this file only maps vendored files to their origin. The third-party **binary** payloads a
build stages into an image are a separate question, covered by `PAYLOADS.md`.

| file | name as received | sha256 as received | since then |
|---|---|---|---|
| `container-scripts/remora-netfix.sh` | `netfix.sh`, under its pre-rename prefix | `ef558dfcf1e085aeccaecdda47285bcd193722af1872af37b47b616b59d154a8` | **Modified by Remora** and renamed at the identity cutover (bd remora-28ix.4): it now defers to init's `remora_fwd` service when that is mounted, supervises and checks the forwarder, locates `fwd` beside itself, and disables Restricted Networking Mode |
| `native/fwd.c` | `fwd.c` | `d37c72cdcf25482a48ace74eec42ff59f7002b814100166a4bcf2365600762b6` | **Modified by Remora**: waits for its port instead of exiting on `EADDRINUSE`, retries `select` on `EINTR`, and writes in full rather than dropping a session on a short write (bd remora-yn4) |

Both files are now ordinary maintained source: their current bytes are Remora's, and the hash above
identifies the starting point, not the present file. `native/uinput-kbd.c` came from the same
overlay and was rewritten in-tree (bd remora-gf7), so it is no longer recorded here at all.
