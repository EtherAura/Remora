// gralloc.remora — mapping and CPU access (bd remora-28ix.4 R2b).
//
// A buffer is shared memory, so "registering" it in a process means mapping that memory in, and
// locking it means handing back the pointer. There is no cache maintenance to do: the memory is
// ordinary host RAM shared between processes, not device memory behind a bus.

#define LOG_TAG "gralloc.remora"

#include <errno.h>
#include <pthread.h>
#include <string.h>
#include <sys/mman.h>

#include <log/log.h>

#include "remora_gralloc.h"

int remoraMapBuffer(gralloc_module_t const * /*module*/, private_handle_t *hnd) {
    void *mapped = mmap(nullptr, hnd->size, PROT_READ | PROT_WRITE, MAP_SHARED, hnd->fd, 0);
    if (mapped == MAP_FAILED) {
        ALOGE("could not map buffer of %d bytes: %s", hnd->size, strerror(errno));
        return -errno;
    }
    hnd->base = uint64_t(uintptr_t(mapped)) + hnd->offset;
    return 0;
}

int remoraUnmapBuffer(gralloc_module_t const * /*module*/, private_handle_t *hnd) {
    void *base = reinterpret_cast<void *>(uintptr_t(hnd->base) - hnd->offset);
    if (munmap(base, hnd->size) < 0) {
        ALOGE("could not unmap buffer at %p: %s", base, strerror(errno));
        return -errno;
    }
    hnd->base = 0;
    return 0;
}

int gralloc_register_buffer(gralloc_module_t const *module, buffer_handle_t handle) {
    if (private_handle_t::validate(handle) < 0) {
        ALOGE("register: invalid handle");
        return -EINVAL;
    }

    // The ALLOCATING process already mapped this buffer at gralloc_alloc time and must not map it
    // twice. Test that with pid, NOT with base. base sits inside the handle's INTS — see the field
    // order in remora_gralloc.h, where sNumInts() derives its count from sizeof(private_handle_t)
    // and so counts base — which means base is transported VERBATIM across binder and a consumer
    // receives the ALLOCATOR's address in it, never zero. Reading a non-zero base as "mine,
    // already mapped" therefore made every consumer skip its own mmap and then write through a
    // pointer that is mapped only in the allocating process: SIGSEGV/SEGV_MAPERR on write, which
    // is what crash-looped every UI RenderThread in guest mode (bd remora-28ix.4.1).
    // pid is the inherited ABI's own answer to "did I allocate this", which is why the field is
    // there and why the legacy allocator tested it.
    private_handle_t *hnd = const_cast<private_handle_t *>(
        reinterpret_cast<private_handle_t const *>(handle));
    if (hnd->pid == getpid()) return 0;

    // Whatever base arrived belongs to some other process; remoraMapBuffer overwrites it with the
    // address that is valid here.
    return remoraMapBuffer(module, hnd);
}

int gralloc_unregister_buffer(gralloc_module_t const *module, buffer_handle_t handle) {
    if (private_handle_t::validate(handle) < 0) {
        ALOGE("unregister: invalid handle");
        return -EINVAL;
    }

    // The mirror of register, and keyed the same way: the allocating process holds its mapping
    // until gralloc_free, which is the path that unmaps it and closes the fd. Unmapping it here
    // would leave that process locking a freed address. Only a process that took a mapping in
    // gralloc_register_buffer gives one back here.
    private_handle_t *hnd = const_cast<private_handle_t *>(
        reinterpret_cast<private_handle_t const *>(handle));
    if (hnd->pid == getpid()) return 0;
    if (hnd->base != 0) return remoraUnmapBuffer(module, hnd);
    return 0;
}

int gralloc_lock(gralloc_module_t const * /*module*/, buffer_handle_t handle, int /*usage*/,
                 int /*l*/, int /*t*/, int /*w*/, int /*h*/, void **vaddr) {
    // The rectangle is ignored on purpose: the whole buffer is CPU-visible shared memory, so
    // there is nothing to partially map or flush, and callers expect the base pointer.
    if (private_handle_t::validate(handle) < 0) {
        ALOGE("lock: invalid handle");
        return -EINVAL;
    }

    private_handle_t *hnd = const_cast<private_handle_t *>(
        reinterpret_cast<private_handle_t const *>(handle));
    if (hnd->base == 0) {
        ALOGE("lock: buffer is not mapped in this process");
        return -EINVAL;
    }
    if (vaddr != nullptr) *vaddr = reinterpret_cast<void *>(uintptr_t(hnd->base));
    return 0;
}

int gralloc_unlock(gralloc_module_t const * /*module*/, buffer_handle_t handle) {
    // Nothing to flush — see gralloc_lock. Still validates, so a bad handle is refused rather
    // than silently accepted.
    if (private_handle_t::validate(handle) < 0) {
        ALOGE("unlock: invalid handle");
        return -EINVAL;
    }
    return 0;
}
