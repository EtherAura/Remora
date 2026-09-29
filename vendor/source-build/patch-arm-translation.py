#!/usr/bin/env python3
"""Apply the A17 fixes to the harvested ARM-translation prebuilts.

Run against the STAGED copies during assemble-image.sh, never against the
vendored originals: the repo keeps the prebuilts exactly as harvested, and the
three fixes below are applied on the way into the image. Args:

    patch-arm-translation.py <staged-guest-linker64> <staged-libnb.so>

There is no source for either binary — arm64 translation was removed from AOSP
before Android 16 (both the A16 and A17 trees have translation_arch =
riscv64_to_x86_64 only), so these are external prebuilts and the only available
fix is a byte patch. That makes them fragile, so every patch site asserts its
expected original bytes AND each file asserts a whole-file SHA-256 first. If a
prebuilt is ever re-harvested the offsets move silently, so this MUST fail the
build rather than emit a subtly broken image. See bd remora-rve and the memories
a17-arm-0x2fa1-rootcause / a17-arm-apps-working for the full diagnosis.

All patches are idempotent: re-running on an already-patched file is a no-op.
"""

import hashlib
import sys

# --- Fix 1: guest dynamic linking (bd remora-rve) ---------------------------
#
# The translator leaves the guest thread's TLS_SLOT_BIONIC_TLS NULL, so bionic's
# __get_bionic_tls() returns 0 inside guest code. ScopedTrace's constructor reads
# the recursion guard bionic_tls.bionic_systrace_disabled unconditionally (there
# is no early-out when tracing is off), which lands as a byte load at 0x2fa1 --
# the field's offset in the ~12KB struct -- and faults. The linker runs
# ScopedTrace on dlopen paths, so EVERY guest dynamic link died; a static
# no-libc ARM binary was unaffected, which is what made this look like a
# translator-core failure for so long.
#
# These four functions are pure instrumentation, so they are neutered. Patching
# the FIRST instruction is safe: the prologue never runs, so there is no stack
# imbalance. The ScopedTrace constructor keeps its `called_ = false` store
# because the destructor checks that flag.
A64_RET = bytes.fromhex("c0035fd6")  # ret
A64_STRB_WZR_X0 = bytes.fromhex("1f000039")  # strb wzr, [x0]

LINKER_SHA256 = "29c98141e26f44f81ce6830046e6677193df327040dd5671f60b3a151c303bfc"
LINKER_PATCHES = [
    (0x4C6E4, bytes.fromhex("fd7bbea9"), A64_RET, "__dl__Z18bionic_trace_beginPKc -> ret"),
    (0x4C81C, bytes.fromhex("ff0301d1"), A64_RET, "__dl__Z16bionic_trace_endv -> ret"),
    (0x4C8D4, bytes.fromhex("fd7bbea9"), A64_STRB_WZR_X0, "ScopedTrace::ScopedTrace -> called_ = false"),
    (0x4C8D8, bytes.fromhex("f44f01a9"), A64_RET, "ScopedTrace::ScopedTrace -> return"),
    (0x4C918, bytes.fromhex("ff4301d1"), A64_RET, "ScopedTrace::~ScopedTrace -> ret"),
]

# --- Fix 2: app startup (bd remora-rve) -------------------------------------
#
# berberis::AppProcessPostInit() signals that the guest app process is ready
# (lock, set flag, notify_all) and then calls pthread_exit -- that thread is
# meant to die. On A17 host bionic's __cxa_thread_finalize then walks the C++
# thread_local destructor list and does `callq *(%rbx)` through a NULL dtor:
# guest code registers thread_local destructors via the native-bridge
# __cxa_thread_atexit_impl trampoline, and on A17 that registers a NULL host
# callback. The thread is exiting anyway, so exit it with a raw SYS_exit and
# skip the broken cleanup. `xor edi,edi` immediately before the call is left in
# place, so the thread's exit status is 0.
#
# The replacement is exactly 5 bytes, matching the call it overwrites -- nothing
# after it shifts. (Verified not to leak: thread count oscillates rather than
# growing, and RSS is stable.)
LIBNB_SHA256 = "fbadc774c989534a567e6af8fd16d2c00727b1f1d9cc778bf538d6b59ed9776d"

# --- Fix 3: the guest-SP exit assertion (bd remora-rtr1) --------------------
#
# berberis::ExecuteGuestCall saves the guest SP before entering guest code and,
# on return, asserts it came back byte-identical -- "Guest call didn't restore
# sp: expected %p, actual %p" -- and aborts the WHOLE process when it does not.
# NIKKE's arm64 code returns from a guest thread entry with SP exactly 16 bytes
# low, so every launch died ~45-60 s in, from RunGuestThread ->
# GuestCall::RunResInt64 -> ExecuteGuestCall, on a thread that was exiting
# anyway. Four launches, four identical aborts; reproduced with the c2-va HAL
# stopped, so it is not codec-related.
#
# The saved value feeds nothing but this comparison (the sole reference to the
# format string is the abort path it guards), so neutering the branch lets the
# guest thread unwind and exit normally. CAVEAT: for a host->guest call that is
# NOT a thread entry, a short return now leaves ThreadState's SP low by that
# much for the rest of the thread's life -- a bounded guest-stack leak rather
# than an abort. That trade is what makes the game playable; if a guest thread
# is ever seen exhausting its stack, this is the first thing to revisit.
X86_NOP2 = bytes.fromhex("6690")  # xchg ax,ax -- 2-byte nop

LIBNB_PATCHES = [
    (
        0x1345AF,
        bytes.fromhex("e83c883200"),  # callq pthread_exit@plt
        bytes.fromhex("6a3c580f05"),  # push 0x3c ; pop rax ; syscall  (SYS_exit)
        "AppProcessPostInit: pthread_exit -> raw SYS_exit",
    ),
    (
        0x35551E,
        bytes.fromhex("753b"),  # jne -> "didn't restore sp" abort
        X86_NOP2,
        "ExecuteGuestCall: drop the guest-SP restore assertion",
    ),
]


def patch(path, expect_sha256, patches):
    with open(path, "rb") as f:
        data = bytearray(f.read())

    actual = hashlib.sha256(data).hexdigest()
    already = all(data[o : o + len(new)] == new for o, _old, new, _d in patches)
    if already:
        print(f"    {path}: already patched, nothing to do")
        return

    if actual != expect_sha256:
        sys.exit(
            f"patch-arm-translation: {path} is not the prebuilt these patches were\n"
            f"  written against (sha256 {actual}, expected {expect_sha256}).\n"
            f"  The byte offsets below are only valid for that exact binary, so\n"
            f"  patching blind would produce a broken image. Re-derive the offsets\n"
            f"  (disassemble for ScopedTrace / AppProcessPostInit) before shipping."
        )

    for off, old, new, desc in patches:
        assert len(old) == len(new), desc
        found = bytes(data[off : off + len(old)])
        if found != old:
            sys.exit(
                f"patch-arm-translation: {path} @ 0x{off:x}: expected {old.hex()}, "
                f"found {found.hex()} ({desc})"
            )
        data[off : off + len(new)] = new
        print(f"    0x{off:08x}  {desc}")

    with open(path, "wb") as f:
        f.write(bytes(data))


def main():
    if len(sys.argv) != 3:
        sys.exit("usage: patch-arm-translation.py <guest-linker64> <libnb.so>")
    print("=== ndk_translation: applying A17 ARM-translation fixes ===")
    patch(sys.argv[1], LINKER_SHA256, LINKER_PATCHES)
    patch(sys.argv[2], LIBNB_SHA256, LIBNB_PATCHES)


if __name__ == "__main__":
    main()
