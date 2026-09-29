# nb_sigfix — berberis app workarounds (bd remora-4ei.99 / remora-4ei.100)

**The name is narrower than what this module does.** It carries two unrelated workarounds: the
TikTok signal fix it was named for, and Amazon's CPU affinity pin. The module *id* stays
`nb_sigfix` because it is already wired into `install.sh`, `verify-image.sh`, the deploy drift
check and the on-device module path, and renaming churns four places for a cosmetic gain. Recorded
here rather than papered over.

## Amazon: CPU affinity pin

Amazon builds several React Native contexts concurrently, and `getRuntimeScheduler()` on one thread
can read a `CatalystInstance` another thread has not finished constructing — a null hybrid pointer,
SIGSEGV at address 0 in `libreactnativejni.so`'s JNI dispatch, on thread `create_react_co`. That is
a latent race in the **app**; real ARM hardware hides it, translation does not. Nothing to hook —
what fixes it is less parallelism, measured as a dose-response against the container cpuset:

| cpus | 2 | 4 | 8 | 16 | 24 |
|---|---|---|---|---|---|
| survives | 8/8 | 8/8 | 6/8 | 1–3/8 | 3/8 |

So the module pins **Amazon only** (`sched_setaffinity` in `postAppSpecialize`, before the app makes
its threads) to 4 cpus, leaving the container's other 20 for the encoder, games and everything else.
Restricting the whole container would have slowed the device to fix one app.

Result: **10/10 launches survive**, against 3/8 unpinned on the same 24-core host. Override with
`setprop remora.affinity.cpus N` (`0` disables); the module also declines to act if N is not
actually a restriction. Note the pin is inherited but not universally — measured 403 of 407 threads
carried the mask, the remaining 4 re-widened by Android's cpuset cgroups. It works regardless.

## TikTok: signal fix

A Zygisk module that keeps **TikTok** (`com.zhiliaoapp.musically`) alive under berberis arm64→x86_64
translation. Remora-authored; source in `src/`, reproducible into `zygisk/x86_64.so` with `build.sh`.

## The bug it works around

TikTok's ByteDance NPTH watchdog `sigqueue`s signal **64 (SIGRTMAX)** to its own process. berberis
then runs the guest handler through `ProcessGuestSignal → RunGuestCall → ScopedVirtualGuestCallFrame`,
and the guest call corrupts that frame — `runtime_primitives/arm64/virtual_guest_call_frame.cc:42`
`CHECK_EQ(stack_pointer_, cpu_->x[29])` fails, guest SP off by **exactly 864 bytes**, on every launch.
The arm64 translator core is a closed A16-vintage prebuilt (`libnb.so`, BuildId `2810e5b4…`) that
cannot be rebuilt (AOSP ships only the riscv64 build config for A17), so the corruption itself is
unfixable — we remove the **trigger** instead.

## How it works

berberis installs its host-side handler for a guest signal by calling libc `sigaction()` (verified:
`libnb.so` imports `sigaction` UND from LIBC). The module PLT-hooks that one call **in libnb only**,
scoped to the TikTok process by `nice_name`. When the guest tries to install a handler for signal 64
we pin the host disposition to `SIG_IGN`: the kernel then discards signal 64 (an ignored signal is
never delivered and, unlike a blocked one, is not queued), so `ProcessGuestSignal` never runs the
guest handler and the frame is never entered. We return 0 so the guest believes its handler is
installed; TikTok proceeds, losing only NPTH's ANR self-reporting.

Scoped to TikTok so no other app's real-time signal handling is touched; in every other process the
module asks Zygisk to `DLCLOSE` it immediately.

## Why not the other two failing apps

Amazon and NIKKE are **racy** signal-delivery crashes (they vanish under berberis tracing), and their
triggers are load-bearing runtime signals (Hermes / Mono GC), not a disposable watchdog — so the same
SIG_IGN trick is neither safe nor confirmed for them. They get the partial `accurate-sigsegv`
mitigation from the berberis rc instead. TikTok's is the only member of the family that is
deterministic and safely neutralisable. See bd remora-4ei.100.

## Rebuild

```
vendor/source-build/features/nb_sigfix/build.sh <aosp-tree>
```

The tree argument is required (or `REMORA_SOURCE_TREE`): it is the AOSP tree Remora builds, which
supplies the clang, `out/soong/ndk/sysroot` (present after a build) and `jni.h`. x86_64 only — the
target runs 64-bit under berberis.

You rarely need to run it by hand: `assemble-image.sh` rebuilds the module from `src/` against the
tree it has just built. The public tree carries no built `.so`, so there that rebuild is the only
source of one — if it fails, the module is left out and the assembly says so.

## Shipping

Baked by `assemble-image.sh` under `/system/etc/remora-spoof/nb_sigfix` (only when ARM translation
AND the ReZygisk stack are both in the image — the module needs `libnb` to hook and Zygisk to load),
installed to `/data/adb/modules/nb_sigfix` at first boot by `install.sh`, and checked by
`verify-image.sh`.
