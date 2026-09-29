// gralloc.remora — the software-path buffer allocator (bd remora-28ix.4 R2b).
//
// WHEN THIS IS USED: guest mode only. With a host GPU passed through, gpu_config.sh selects a
// minigbm variant (gbm / minigbm_intel / minigbm_gbm_mesa) and this module is never loaded. Guest
// mode has no render node at all — SwiftShader or ANGLE draws on the CPU — so buffers are plain
// shared memory and that is all this allocates.
//
// NO FRAMEBUFFER PATH, deliberately. The legacy gralloc this replaces carried an fb device that
// mmaps /dev/graphics/fb0 and page-flips. A container has no framebuffer device, so that path
// could only ever fail; every allocation here is shared memory, including one tagged
// GRALLOC_USAGE_HW_FB, which SurfaceFlinger then composites through the composer HAL as usual.
//
// THE HANDLE LAYOUT IS AN ABI, NOT AN IMPLEMENTATION DETAIL. The composer receives buffers
// allocated here, and a prebuilt composer has no source to rebuild against the header. Anything it
// reads out of the handle — the fd, the size, the offset — must sit exactly where the previous
// allocator put it, so this struct keeps the inherited field order, types and magic verbatim.
// Rewriting the code is safe; rewriting the layout is not. Do not reorder these fields.

#pragma once

#include <errno.h>
#include <hardware/gralloc.h>
#include <pthread.h>
#include <stdint.h>
#include <unistd.h>

#include <cutils/native_handle.h>

struct private_handle_t;

struct private_module_t {
    gralloc_module_t base;

    private_handle_t *framebuffer;  // always null here — kept so the struct matches the ABI
    uint32_t flags;
    uint32_t numBuffers;
    uint32_t bufferMask;
    pthread_mutex_t lock;
    buffer_handle_t currentBuffer;
    int pmem_master;
};

#ifdef __cplusplus
struct private_handle_t : public native_handle {
#else
struct private_handle_t {
    struct native_handle nativeHandle;
#endif

    enum {
        PRIV_FLAGS_FRAMEBUFFER = 0x00000001,
    };

    // file-descriptors — must be first, and numFds must count them: the framework dups handles
    // generically by walking this region.
    int fd;
    // ints
    int magic;
    int flags;
    int size;
    int offset;

    uint64_t base __attribute__((aligned(8)));
    int pid;

#ifdef __cplusplus
    static const int sNumFds = 1;
    static const int sMagic = 0x3141592;

    static inline int sNumInts() {
        return (((sizeof(private_handle_t) - sizeof(native_handle_t)) / sizeof(int)) - sNumFds);
    }

    private_handle_t(int fd, int size, int flags)
        : fd(fd), magic(sMagic), flags(flags), size(size), offset(0), base(0), pid(getpid()) {
        version = sizeof(native_handle);
        numInts = sNumInts();
        numFds = sNumFds;
    }

    ~private_handle_t() { magic = 0; }

    // Every entry point validates before dereferencing: a handle arriving here came across a
    // binder boundary from another process, so "it is the right shape" is not something to assume.
    static int validate(const native_handle *h) {
        const private_handle_t *hnd = reinterpret_cast<const private_handle_t *>(h);
        if (!h || h->version != sizeof(native_handle) || h->numInts != sNumInts() ||
            h->numFds != sNumFds || hnd->magic != sMagic) {
            return -EINVAL;
        }
        return 0;
    }
#endif
};

int remoraMapBuffer(gralloc_module_t const *module, private_handle_t *hnd);
int remoraUnmapBuffer(gralloc_module_t const *module, private_handle_t *hnd);
