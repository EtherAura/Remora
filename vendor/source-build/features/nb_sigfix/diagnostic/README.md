# nb_sigtrace — diagnostic only, NOT shipped

Source kept because it produced the Amazon diagnosis in bd remora-4ei.100 and re-deriving that
costs hours. It is **not** wired into any build and **not** baked into any image.

Build it exactly like the shipped module (`../build.sh` is the template — same clang, sysroot and
flags, just `sigtrace.cpp` instead of `../src/module.cpp`), then install by hand:

```
docker exec <c> sh -c 'mkdir -p /data/adb/modules/nb_sigtrace/zygisk'
docker exec -i <c> sh -c 'cat > /data/adb/modules/nb_sigtrace/zygisk/x86_64.so' < nb_sigtrace.so
# + a module.prop, then reboot the container so ReZygisk loads it
```
Remove the directory and reboot to uninstall. Always remove it when done — it hooks the signal path
of the targeted apps.

## What it does, and the one thing it got wrong

PLT-hooks two libc symbols inside `libnb.so`: `sigaction` (logs every guest handler install) and
`syscall` (intended to log guest signal *sends*).

**The `sigaction` half works and is what produced the finding.** The `syscall` half caught NOTHING,
which is itself worth recording: the AOSP berberis source emulates guest `tgkill` /
`rt_tgsigqueueinfo` / `rt_sigqueueinfo` by calling libc `syscall(234/297/129, …)`, but the SHIPPED
closed ndk_translation build does not route guest syscalls through libc's wrapper — libnb's 50
`syscall@plt` call sites are its own internal syscalls, not the guest's. Do not trust that source
table as a description of the binary.

## What it measured

- NIKKE installs handlers for signals 3,4,5,7,8,11,13,16,**24**,**30** — independently confirming
  SIGXCPU/SIGPWR (Mono GC thread suspend), which had only been seen via `berberis.tracing` before.
- Amazon, 6 trials: died 4/4 when it installed a SIGSEGV handler (crash in the SAME MILLISECOND, on
  the same thread), survived 2/2 when it never installed one.
- Refusing the SIGSEGV install (returning EINVAL) made it strictly worse — 0/8 survived — and the
  fault still landed in the same millisecond. So the install is a MARKER, not the cause: the guest
  deliberately dereferences null and relies on its own SIGSEGV handler to catch it, and berberis
  cannot deliver that fault to a guest handler whether or not the claim succeeded.
