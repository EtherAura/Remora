// nb_sigfix Zygisk module (bd remora-4ei.99) — keep TikTok alive under berberis ARM translation.
//
// TikTok's ByteDance NPTH watchdog sigqueues signal 64 (SIGRTMAX) to its own process. berberis
// then runs the guest handler through ProcessGuestSignal -> RunGuestCall -> the virtual guest
// call frame, and the guest call corrupts that frame (runtime_primitives/arm64/
// virtual_guest_call_frame.cc:42 CHECK, guest SP off by exactly 864 bytes). Deterministic: every
// launch. berberis' arm64 translator core is a closed prebuilt (bd remora-4ei.100), so we cannot
// fix the corruption; we prevent the trigger instead.
//
// berberis installs its host-side handler for a guest signal by calling libc sigaction() (verified:
// /system/lib64/libnb.so imports `sigaction`, UND from LIBC). We PLT-hook that call in libnb only,
// and when the target signal's handler is being installed we pin the HOST disposition to SIG_IGN.
// The kernel then discards signal 64 (an ignored signal is never delivered and, unlike a blocked
// one, is not queued), so ProcessGuestSignal never runs the guest handler and the frame is never
// entered. We return 0, so the guest believes its handler is installed and the app proceeds; it
// loses only NPTH's ANR self-reporting.
//
// Scoped to TikTok by process name so no other app's real-time signal handling is touched.
//
// ============================================================================================
// SECOND, UNRELATED WORKAROUND IN THE SAME MODULE: AMAZON'S CPU AFFINITY (bd remora-4ei.100).
//
// Amazon (com.amazon.mShop.android.shopping) builds SEVERAL React Native contexts concurrently,
// and getRuntimeScheduler() on one thread can read a CatalystInstance another thread has not
// finished constructing — a null hybrid pointer, SIGSEGV at address 0 in
// libreactnativejni.so's JNI dispatch, on thread create_react_co. That is a latent race in the
// APP; real ARM hardware happens to hide it and translation does not.
//
// It is not a berberis signal bug and there is nothing to hook. What fixes it is less
// parallelism, measured as a clean dose-response against the container's cpuset:
//        2 cpus 8/8 alive    4 cpus 8/8    8 cpus 6/8    16 cpus 1-3/8    24 cpus 3/8
// So this pins AMAZON ONLY to a few cores. The container keeps all 24 for the encoder, the games
// and everything else — the earlier alternative, restricting the whole container, would have
// slowed the entire device to fix one app.
//
// WHY IT SHARES nb_sigfix's MODULE ID rather than shipping as a second module: the id is already
// wired into install.sh, verify-image.sh, the deploy drift check and the on-device module path,
// and a rename churns all four for a cosmetic gain. The name is now narrower than what the module
// does — that is a known wart, recorded here and in REMORA.md rather than papered over.
// ============================================================================================

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <sched.h>
#include <unistd.h>
#include <errno.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/system_properties.h>
#include <android/log.h>
#include <jni.h>

#include "zygisk.hpp"

using zygisk::Api;
using zygisk::AppSpecializeArgs;
using zygisk::ServerSpecializeArgs;

#define TAG "nb_sigfix"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)

static const char* kTargetProc = "com.zhiliaoapp.musically";  // TikTok
static const int   kTargetSig   = 64;                          // SIGRTMAX (NPTH watchdog)

// Amazon, and how many cpus to leave it. 4 is the largest count that measured 8/8; 8 already
// slipped to 6/8. Override at runtime with `setprop remora.affinity.cpus N` (0 disables).
static const char* kAffinityProc = "com.amazon.mShop.android.shopping";
static const int   kAffinityCpus = 4;

// --- the interposed sigaction ------------------------------------------------------------------
using sigaction_t = int (*)(int, const struct sigaction*, struct sigaction*);
static sigaction_t g_orig_sigaction = nullptr;

static int hooked_sigaction(int signum, const struct sigaction* act, struct sigaction* oldact) {
    if (act && signum == kTargetSig) {
        struct sigaction ign = *act;         // preserve mask/size, overwrite disposition only
        ign.sa_flags &= ~SA_SIGINFO;
        ign.sa_handler = SIG_IGN;
        LOGI("pinning signal %d to SIG_IGN (berberis handler install intercepted); "
             "TikTok keeps running, NPTH ANR trace suppressed", signum);
        return g_orig_sigaction ? g_orig_sigaction(signum, &ign, oldact)
                                : sigaction(signum, &ign, oldact);
    }
    return g_orig_sigaction ? g_orig_sigaction(signum, act, oldact)
                            : sigaction(signum, act, oldact);
}

// Find /system/lib64/libnb.so's dev+inode from /proc/self/maps (identifies the ELF for pltHook).
static bool find_libnb(dev_t* out_dev, ino_t* out_ino) {
    FILE* f = fopen("/proc/self/maps", "re");
    if (!f) return false;
    char line[512];
    bool found = false;
    while (fgets(line, sizeof(line), f)) {
        if (!strstr(line, "/libnb.so")) continue;
        // format: start-end perms offset MAJ:MIN inode path
        unsigned long lo, hi, off;
        unsigned int maj, min;
        unsigned long ino;
        char perms[8];
        if (sscanf(line, "%lx-%lx %7s %lx %x:%x %lu",
                   &lo, &hi, perms, &off, &maj, &min, &ino) == 7) {
            *out_dev = makedev(maj, min);
            *out_ino = (ino_t)ino;
            found = true;
            break;
        }
    }
    fclose(f);
    return found;
}

class NbSigfix : public zygisk::ModuleBase {
public:
    void onLoad(Api* api, JNIEnv* env) override {
        api_ = api;
        env_ = env;
    }

    void preAppSpecialize(AppSpecializeArgs* args) override {
        // Decide here which target this is, while we can still read nice_name cheaply.
        is_target_ = false;
        is_affinity_target_ = false;
        if (args && args->nice_name) {
            const char* name = env_->GetStringUTFChars(args->nice_name, nullptr);
            if (name) {
                is_target_ = (strcmp(name, kTargetProc) == 0);
                is_affinity_target_ = (strcmp(name, kAffinityProc) == 0);
                env_->ReleaseStringUTFChars(args->nice_name, name);
            }
        }
        if (!is_target_ && !is_affinity_target_) {
            // Neither target: ask Zygisk to unload us from this process entirely.
            api_->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
        }
    }

    void postAppSpecialize(const AppSpecializeArgs*) override {
        if (is_affinity_target_) applyAffinity();
        if (!is_target_) return;
        dev_t dev; ino_t ino;
        if (!find_libnb(&dev, &ino)) {
            LOGW("libnb.so not mapped in %s at specialize; no hook installed "
                 "(app may load the bridge later)", kTargetProc);
            return;
        }
        // libnb (BuildId 2810e5b4...) imports plain `sigaction` as UND from LIBC — verified with
        // readelf. Registering a symbol that is absent from the target's PLT makes pltHookCommit
        // report failure even though other hooks applied, so we register only what libnb imports.
        api_->pltHookRegister(dev, ino, "sigaction", (void*)hooked_sigaction, (void**)&g_orig_sigaction);
        if (api_->pltHookCommit()) {
            LOGI("PLT hook committed on libnb.so (dev=%lu ino=%lu) for %s: signal %d neutralised",
                 (unsigned long)dev, (unsigned long)ino, kTargetProc, kTargetSig);
        } else {
            LOGW("pltHookCommit failed for %s", kTargetProc);
        }
    }

private:
    // Pin Amazon to a few cores so React Native's concurrent startup stops racing itself
    // (bd remora-4ei.100). Set on the main thread at specialize, BEFORE the app creates any of its
    // own threads, because a new thread starts with its creator's affinity mask — one call here
    // covers the process without chasing threads.
    // MEASURED rather than assumed, because the inheritance is not total: on a running Amazon,
    // 403 of 407 threads carried 0-3 and 4 did not. Android moves some threads between cpuset
    // cgroups (top-app/foreground/background all exist here), which re-widens those few. It does
    // not matter — 10/10 launches survived with the pin versus 3/8 without — but do not write
    // "every thread" in a comment here, because four of them are not.
    void applyAffinity() {
        int n = kAffinityCpus;
        char buf[PROP_VALUE_MAX] = {0};
        if (__system_property_get("remora.affinity.cpus", buf) > 0) {
            int v = atoi(buf);
            if (v > 0 && v <= CPU_SETSIZE) n = v;
        }
        if (n <= 0) return;                       // explicitly disabled
        long online = sysconf(_SC_NPROCESSORS_ONLN);
        if (online > 0 && n >= online) {
            LOGI("affinity not applied for %s: asked for %d of %ld cpus, which is not a restriction",
                 kAffinityProc, n, online);
            return;
        }
        cpu_set_t set;
        CPU_ZERO(&set);
        for (int i = 0; i < n; ++i) CPU_SET(i, &set);
        if (sched_setaffinity(0, sizeof(set), &set) == 0) {
            LOGI("pinned %s to %d of %ld cpus — narrows the React Native startup race "
                 "(measured 8/8 alive at 4 cpus vs 3/8 at 24)", kAffinityProc, n, online);
        } else {
            LOGW("sched_setaffinity failed for %s (errno %d) — app runs unpinned and may still "
                 "lose its startup race", kAffinityProc, errno);
        }
    }

    Api* api_ = nullptr;
    JNIEnv* env_ = nullptr;
    bool is_target_ = false;
    bool is_affinity_target_ = false;
};

REGISTER_ZYGISK_MODULE(NbSigfix)
