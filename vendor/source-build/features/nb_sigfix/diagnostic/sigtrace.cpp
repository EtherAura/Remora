// nb_sigtrace — DIAGNOSTIC Zygisk module (bd remora-4ei.100).
//
// Amazon and NIKKE die in berberis' guest-signal delivery, and both crashes VANISH under
// berberis.tracing — because that tracing writes its line inside ProcessGuestSignal, i.e. INSIDE
// the racy delivery window it is trying to observe. So the triggering signal has never been
// identified for either app.
//
// This observes the SEND side instead, which is a different code path from the racy delivery
// window. berberis emulates the guest's signal-sending syscalls with RAW libc syscall() calls
// (kernel_api/arm64/gen_syscall_emulation_arm64_to_x86_64-inl.h: guest __NR_tgkill -> syscall(234),
// __NR_rt_tgsigqueueinfo -> syscall(297), __NR_rt_sigqueueinfo -> syscall(129)), and libnb.so
// imports libc `syscall` (UND, 50 call sites). So one PLT hook on `syscall` sees every signal the
// guest sends, without touching the delivery path at all.
//
// Deliberately cheap: a switch on the syscall number (2-4 compares) for the >99% of calls that are
// not signal sends, a relaxed atomic increment for the rest, and BOUNDED logging — the first 40 of
// each signal then every 100th — so a chatty app cannot flood logcat and perturb what we measure.
//
// DIAGNOSTIC ONLY. Not baked into any image; installed to /data by hand and removed after use.

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <errno.h>
#include <android/log.h>
#include <jni.h>

#include "zygisk.hpp"

using zygisk::Api;
using zygisk::AppSpecializeArgs;
using zygisk::ServerSpecializeArgs;

#define TAG "nb_sigtrace"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)

// Processes to observe. Both are the racy berberis crashes; TikTok is deterministic and already
// fixed by nb_sigfix, so it is deliberately not here.
static const char* kTargets[] = {
    "com.amazon.mShop.android.shopping",
    "com.proximabeta.nikke",
};

// x86_64 syscall numbers that deliver a signal, and where the signal number sits in each.
//   kill(pid, sig)                         -> sig = arg2
//   rt_sigqueueinfo(tgid, sig, uinfo)      -> sig = arg2
//   tgkill(tgid, tid, sig)                 -> sig = arg3
//   rt_tgsigqueueinfo(tgid, tid, sig, uinfo) -> sig = arg3
enum { SYS_KILL = 62, SYS_RT_SIGQUEUEINFO = 129, SYS_TGKILL = 234, SYS_RT_TGSIGQUEUEINFO = 297 };

static unsigned int g_count[65];

// libc syscall() is variadic; on x86_64 SysV, declaring 7 fixed longs matches how bionic's stub
// reads its arguments (six registers + one stack slot), so forwarding is exact.
using syscall_t = long (*)(long, long, long, long, long, long, long);
static syscall_t g_orig_syscall = nullptr;

static long hooked_syscall(long n, long a1, long a2, long a3, long a4, long a5, long a6) {
    int sig = -1;
    const char* which = nullptr;
    long tgid = a1, tid_arg = -1;
    switch (n) {
        case SYS_KILL:              sig = (int)a2; which = "kill";              break;
        case SYS_RT_SIGQUEUEINFO:   sig = (int)a2; which = "rt_sigqueueinfo";   break;
        case SYS_TGKILL:            sig = (int)a3; which = "tgkill";   tid_arg = a2; break;
        case SYS_RT_TGSIGQUEUEINFO: sig = (int)a3; which = "rt_tgsigqueueinfo"; tid_arg = a2; break;
        default: break;
    }
    if (which != nullptr && sig > 0 && sig < 65) {
        unsigned int c = __atomic_add_fetch(&g_count[sig], 1, __ATOMIC_RELAXED);
        if (c <= 40 || (c % 100) == 0) {
            LOGI("guest SEND sig=%d (%s) n=%u from_tid=%d -> tgid=%ld tid=%ld",
                 sig, which, c, gettid(), tgid, tid_arg);
        }
    }
    return g_orig_syscall(n, a1, a2, a3, a4, a5, a6);
}

// Also record which signals the guest installs handlers for — rare, so free, and it tells us the
// app's signal vocabulary (which of the sends above are even deliverable to guest code).
using sigaction_t = int (*)(int, const struct sigaction*, struct sigaction*);
static sigaction_t g_orig_sigaction = nullptr;

// EXPERIMENT (bd remora-4ei.100): measured 6/6 — Amazon dies in the SAME MILLISECOND, on the same
// thread, that the guest installs a SIGSEGV handler, and survives exactly when it never installs
// one. berberis cannot route that first fault to a freshly-claimed guest handler, so it takes the
// fatal path. We cannot fix libnb, so refuse the claim and let the app take its no-handler
// fallback — the same "remove the trigger" shape as nb_sigfix does for TikTok's SIGRTMAX.
// Returning FAILURE (not a fake success) is deliberate: a fake success leaves the app believing it
// has a handler, so it would still take whatever null-fault path the handler exists to catch.
static int hooked_sigaction(int signum, const struct sigaction* act, struct sigaction* oldact) {
    if (act != nullptr) {
        LOGI("guest INSTALLS handler for sig=%d (tid=%d)", signum, gettid());
        if (signum == SIGSEGV) {
            LOGW("refusing guest SIGSEGV handler install -> EINVAL (berberis cannot route the "
                 "first fault after a claim; see bd remora-4ei.100)");
            errno = EINVAL;
            return -1;
        }
    }
    return g_orig_sigaction ? g_orig_sigaction(signum, act, oldact)
                            : sigaction(signum, act, oldact);
}

static bool find_libnb(dev_t* out_dev, ino_t* out_ino) {
    FILE* f = fopen("/proc/self/maps", "re");
    if (f == nullptr) return false;
    char line[512];
    bool found = false;
    while (fgets(line, sizeof(line), f) != nullptr) {
        if (strstr(line, "/libnb.so") == nullptr) continue;
        unsigned long lo, hi, off, ino;
        unsigned int maj, min;
        char perms[8];
        if (sscanf(line, "%lx-%lx %7s %lx %x:%x %lu", &lo, &hi, perms, &off, &maj, &min, &ino) == 7) {
            *out_dev = makedev(maj, min);
            *out_ino = (ino_t)ino;
            found = true;
            break;
        }
    }
    fclose(f);
    return found;
}

class NbSigTrace : public zygisk::ModuleBase {
public:
    void onLoad(Api* api, JNIEnv* env) override { api_ = api; env_ = env; }

    void preAppSpecialize(AppSpecializeArgs* args) override {
        is_target_ = false;
        if (args != nullptr && args->nice_name != nullptr) {
            const char* name = env_->GetStringUTFChars(args->nice_name, nullptr);
            if (name != nullptr) {
                for (const char* t : kTargets) {
                    if (strcmp(name, t) == 0) { is_target_ = true; break; }
                }
                env_->ReleaseStringUTFChars(args->nice_name, name);
            }
        }
        if (!is_target_) api_->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
    }

    void postAppSpecialize(const AppSpecializeArgs*) override {
        if (!is_target_) return;
        dev_t dev; ino_t ino;
        if (!find_libnb(&dev, &ino)) { LOGW("libnb.so not mapped; no trace installed"); return; }
        api_->pltHookRegister(dev, ino, "syscall",   (void*)hooked_syscall,   (void**)&g_orig_syscall);
        api_->pltHookRegister(dev, ino, "sigaction", (void*)hooked_sigaction, (void**)&g_orig_sigaction);
        if (api_->pltHookCommit()) {
            LOGI("tracing guest signal SENDS via libnb syscall() (dev=%lu ino=%lu)",
                 (unsigned long)dev, (unsigned long)ino);
        } else {
            LOGW("pltHookCommit failed");
        }
    }

private:
    Api* api_ = nullptr;
    JNIEnv* env_ = nullptr;
    bool is_target_ = false;
};

REGISTER_ZYGISK_MODULE(NbSigTrace)
