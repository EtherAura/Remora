# Carried source patches for ReZygisk

Patches written against [PerformanC/ReZygisk](https://github.com/PerformanC/ReZygisk) that Remora
**carries itself**, as it does its other patch series: they are applied at build time, and nothing
here waits on another project.

`vendor/host-prereqs/rezygisk/build-rezygisk.sh` applies these to the pinned commit and builds the
64-bit binaries into `rez-mod/`. The directory name is historical.

## `0001-monitor-bound-the-module.prop-status-appends-*.patch`

bd `remora-4ei.98`. Replaces the unbounded `strcat` appends in `loader/src/ptracer/monitor.c` with
`strlcat`.

**Why we care.** Under Android's FORTIFY the unbounded appends abort `zygisk-ptrace64` on a
cosmetic status banner, and the injector dies with it. `patch-rezygisk.py` used to work around this
by binary-patching a vendored GPL-3.0 dependency on every bump. Applying the fix at build time from
source deleted both that script and the NOPped `zygisk-ptrace64.patched` blob, and restored
ReZygisk's own status banner, which the NOPs blanked.

**Base.** `e42886f48eb1c9eabcc94f08a3c3af0cdbffb99e` — the upstream `main` commit Remora pins, the
same one CI build 537 came from. So the patch applies to exactly what we ship.

**Two buffers, not one.** The bead only named `pre_section`/`post_section`, but `status_text` in
the same status writer has the identical defect and is the more serious of the two:

| buffer | size | appended from | worst case |
|---|---|---|---|
| `pre_section` / `post_section` | `char[1024]` each | `module.prop`, line by line | local file, ~1 KB is enough |
| `status_text` | `char[256]` | includes `daemon_error_info` | **a heap string whose length arrived as a `uint32_t` over the monitor socket, unbounded** |

Under FORTIFY both abort. Without it, the `status_text` one is a stack buffer overflow.

**The overflow is cumulative, not one long line.** Every line of `module.prop` is concatenated onto
whichever section it falls in, so what matters is the total size of the pre- and post-description
blocks. `reproduce-fortify-abort.c` mirrors `prepare_environment()`'s loop exactly and shows it
outside Android:

```
gcc -O2 -D_FORTIFY_SOURCE=2 -o fortify-check reproduce-fortify-abort.c
./fortify-check          # strcat  -> aborts at line 18, exit 134 (SIGABRT)
./fortify-check fixed    # strlcat -> survives 60 lines, holds 1023 of 3360 bytes, exit 0
```

Sixty ordinary short lines, none anywhere near 1024 bytes; it dies the moment the running total
would pass 1023.

**Trade accepted.** `strlcat` turns the abort into truncation of a cosmetic banner. Sizing the
buffers from the file length would avoid truncation entirely, but that is a larger change and is
not needed to fix the crash.

**It survives a bump.** The patch also applies with `git apply --check` to a later upstream `main`
(`e10115a`), where the defect is still present verbatim — `static char pre_section[1024]` /
`post_section[1024]`, and the same unbounded appends. Moving the pin should not need a rebase;
`build-rezygisk.sh` refuses loudly if it ever does.

**Compiled, which it had not been.** `reproduce-fortify-abort.c` above proves the mechanism under
glibc, but the patch itself had only ever been read. Built with NDK r27c at API 25 under the
loader's own release flags (`-std=c18 -D_GNU_SOURCE -Wall -Wextra -O3 -D_FORTIFY_SOURCE=2`):

| target | result | `__strcat_chk` call sites | `__strlcat_chk` |
|---|---|---|---|
| `aarch64-linux-android25` | 0 warnings | 1 | 16 |
| `x86_64-linux-android25` | 0 warnings | 1 | 16 |
| `armv7a-linux-androideabi25` | 0 warnings | 1 | 16 |

Unpatched, the same object has **17** `__strcat_chk` call sites and no `__strlcat_chk`. The one
that survives is the `strcat(daemon_name, ...)` in `spawn_daemon()`, which the patch deliberately
leaves alone: it appends two bytes to a `PATH_MAX` buffer holding a 13-byte literal and cannot
overflow. Counting *call sites* rather than undefined symbols matters here — `__strcat_chk` still
appears in `nm -u` output after the fix, because of that one safe caller, so the symbol alone
would read as a failed patch.

`__strlcat_chk` is bionic's fortified `strlcat`. It aborts only when the caller lies about the
destination size; every call here passes `sizeof` a real array, so the behaviour is truncation.
