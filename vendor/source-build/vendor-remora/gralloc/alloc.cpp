// gralloc.remora — allocation and the HAL module entry point (bd remora-28ix.4 R2b).

#define LOG_TAG "gralloc.remora"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cutils/ashmem.h>
#include <log/log.h>

#include "remora_gralloc.h"

// Declared in mapper.cpp — the module-level operations.
extern int gralloc_register_buffer(gralloc_module_t const *module, buffer_handle_t handle);
extern int gralloc_unregister_buffer(gralloc_module_t const *module, buffer_handle_t handle);
extern int gralloc_lock(gralloc_module_t const *module, buffer_handle_t handle, int usage, int l,
                        int t, int w, int h, void **vaddr);
extern int gralloc_unlock(gralloc_module_t const *module, buffer_handle_t handle);

namespace {

size_t pageAlign(size_t x) {
    const size_t pageSize = size_t(getpagesize());
    return (x + (pageSize - 1)) & ~(pageSize - 1);
}

// Bytes per pixel for the formats this allocator is asked for. The list is deliberately explicit:
// an unknown format returning a wrong stride produces a buffer that is too small, which surfaces
// as corruption or a crash inside whatever drew into it rather than as an allocation failure.
int bytesPerPixel(int format) {
    switch (format) {
        case HAL_PIXEL_FORMAT_RGBA_8888:
        case HAL_PIXEL_FORMAT_RGBX_8888:
        case HAL_PIXEL_FORMAT_BGRA_8888:
            return 4;
        case HAL_PIXEL_FORMAT_RGB_888:
            return 3;
        case HAL_PIXEL_FORMAT_RGB_565:
        case HAL_PIXEL_FORMAT_RAW16:
            return 2;
        case HAL_PIXEL_FORMAT_BLOB:
        case HAL_PIXEL_FORMAT_Y8:
            return 1;
        default:
            return 0;  // handled by the caller: 0 means "not a simple linear format"
    }
}

int allocShared(alloc_device_t * /*dev*/, size_t size, int /*usage*/, buffer_handle_t *pHandle) {
    size = pageAlign(size);

    // ashmem_create_region, not memfd directly: libcutils picks memfd itself when the container
    // has no /dev/ashmem (which is the common case here — see sys.use_memfd in remora.common.rc
    // and post-fs-data.remora.sh), so this one call is correct on both kinds of host.
    const int fd = ashmem_create_region("remora-gralloc", size);
    if (fd < 0) {
        ALOGE("could not allocate %zu bytes of shared memory: %s", size, strerror(errno));
        return -errno;
    }

    private_handle_t *hnd = new private_handle_t(fd, int(size), 0);
    gralloc_module_t *module = reinterpret_cast<gralloc_module_t *>(
        reinterpret_cast<private_module_t *>(0));  // unused by remoraMapBuffer
    const int err = remoraMapBuffer(module, hnd);
    if (err < 0) {
        close(fd);
        delete hnd;
        return err;
    }

    *pHandle = hnd;
    return 0;
}

int gralloc_alloc(alloc_device_t *dev, int w, int h, int format, int usage,
                  buffer_handle_t *pHandle, int *pStride) {
    if (!pHandle || !pStride || w <= 0 || h <= 0) return -EINVAL;

    size_t size = 0;
    int stride = 0;

    if (format == HAL_PIXEL_FORMAT_YV12) {
        // YV12 is planar and its planes are alignment-constrained: the luma stride rounds to 16
        // and each chroma plane's to 8, per the format's own contract in graphics.h. Getting this
        // wrong under-allocates and the camera/video path scribbles past the end.
        const int alignedW = (w + 15) & ~15;
        const int chromaStride = (alignedW / 2 + 7) & ~7;
        stride = alignedW;
        size = size_t(alignedW) * size_t(h) + size_t(chromaStride) * size_t(h);
    } else if (format == HAL_PIXEL_FORMAT_BLOB) {
        // A BLOB is a byte array whose "width" IS its length — no stride arithmetic applies.
        stride = w;
        size = size_t(w) * size_t(h);
    } else {
        const int bpp = bytesPerPixel(format);
        if (bpp == 0) {
            ALOGE("unsupported pixel format 0x%x", format);
            return -EINVAL;
        }
        // 32-byte row alignment: enough for the SIMD paths in the software renderers that are the
        // only consumers on this path, and what the allocator this replaces effectively produced.
        const size_t bytesPerRow = (size_t(w) * size_t(bpp) + 31) & ~size_t(31);
        size = bytesPerRow * size_t(h);
        stride = int(bytesPerRow / size_t(bpp));
    }

    const int err = allocShared(dev, size, usage, pHandle);
    if (err < 0) return err;

    *pStride = stride;
    return 0;
}

int gralloc_free(alloc_device_t * /*dev*/, buffer_handle_t handle) {
    if (private_handle_t::validate(handle) < 0) {
        ALOGE("free: invalid handle");
        return -EINVAL;
    }

    private_handle_t const *hnd = reinterpret_cast<private_handle_t const *>(handle);
    private_handle_t *mutableHnd = const_cast<private_handle_t *>(hnd);
    if (mutableHnd->base != 0) remoraUnmapBuffer(nullptr, mutableHnd);
    if (hnd->fd >= 0) close(hnd->fd);
    delete mutableHnd;
    return 0;
}

int gralloc_close(struct hw_device_t *dev) {
    delete reinterpret_cast<alloc_device_t *>(dev);
    return 0;
}

int gralloc_device_open(const hw_module_t *module, const char *name, hw_device_t **device) {
    // GRALLOC_HARDWARE_FB0 is the framebuffer device, which this module deliberately does not
    // implement (see remora_gralloc.h): a container has no /dev/graphics/fb0, and composition
    // goes through the composer HAL. Refusing is the honest answer; pretending would hand back a
    // device whose post() has nowhere to write.
    if (strcmp(name, GRALLOC_HARDWARE_GPU0) != 0) {
        ALOGE("no such device: %s (this module provides " GRALLOC_HARDWARE_GPU0 " only)", name);
        return -EINVAL;
    }

    alloc_device_t *dev = new alloc_device_t;
    memset(dev, 0, sizeof(*dev));
    dev->common.tag = HARDWARE_DEVICE_TAG;
    dev->common.version = 0;
    dev->common.module = const_cast<hw_module_t *>(module);
    dev->common.close = gralloc_close;
    dev->alloc = gralloc_alloc;
    dev->free = gralloc_free;

    *device = &dev->common;
    return 0;
}

struct hw_module_methods_t gralloc_module_methods = {
    .open = gralloc_device_open,
};

}  // namespace

// The symbol the loader looks for. hw_get_module resolves gralloc.<ro.hardware.gralloc>.so and
// then this struct by name, so both must line up with what gpu_config.sh sets.
struct private_module_t HAL_MODULE_INFO_SYM = {
    .base =
        {
            .common =
                {
                    .tag = HARDWARE_MODULE_TAG,
                    .version_major = 1,
                    .version_minor = 0,
                    .id = GRALLOC_HARDWARE_MODULE_ID,
                    .name = "Remora graphics memory allocator",
                    .author = "Remora",
                    .methods = &gralloc_module_methods,
                    .dso = nullptr,
                    .reserved = {0},
                },
            .registerBuffer = gralloc_register_buffer,
            .unregisterBuffer = gralloc_unregister_buffer,
            .lock = gralloc_lock,
            .unlock = gralloc_unlock,
            .perform = nullptr,
            .lock_ycbcr = nullptr,
            .lockAsync = nullptr,
            .unlockAsync = nullptr,
            .lockAsync_ycbcr = nullptr,
            .reserved_proc = {nullptr},
        },
    .framebuffer = nullptr,
    .flags = 0,
    .numBuffers = 0,
    .bufferMask = 0,
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .currentBuffer = nullptr,
    .pmem_master = -1,
};
