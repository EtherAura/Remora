// bd remora-e5x.36 isolation: reproduce c2-va's vaCreateSurfaces(DRM_PRIME_2) import on the host,
// against the same iHD driver and the same buffer shape minigbm hands it (a LINEAR R8 blob whose
// planar geometry lives only in the descriptor). Varies one thing at a time.
#include <stdlib.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <gbm.h>
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>
#include <libdrm/drm_fourcc.h>

static int drm_fd;
static VADisplay dpy;
static struct gbm_device *gbm;

// Allocate exactly like minigbm's spoofed-format path: R8, width = total bytes, height = 1, linear.
static int alloc_blob(size_t total, uint64_t *mod_out)
{
    // Desktop gbm caps BO width, so allocate the same BYTE COUNT as a 2-D linear R8 buffer.
    // Only the fd and its size matter here; the descriptor carries its own offsets and pitches.
    uint32_t w = 4096, h = (uint32_t)((total + w - 1) / w);
    struct gbm_bo *bo = gbm_bo_create(gbm, w, h, GBM_FORMAT_R8,
                                      GBM_BO_USE_LINEAR | GBM_BO_USE_RENDERING);
    if (!bo) { printf("    gbm_bo_create(R8 %ux%u) FAILED\n", w, h); return -1; }
    *mod_out = gbm_bo_get_modifier(bo);
    return gbm_bo_get_fd(bo);
}

static void try_import(const char *label, uint32_t fourcc, uint32_t w, uint32_t h,
                       uint32_t rtfmt, int planar, uint32_t pitchY, uint32_t offY,
                       uint32_t pitchUV, uint32_t offUV, size_t total, int split_layers)
{
    uint64_t mod = 0;
    int fd = alloc_blob(total, &mod);
    if (fd < 0) return;

    VADRMPRIMESurfaceDescriptor d;
    memset(&d, 0, sizeof(d));
    d.fourcc = fourcc;
    d.width = w;
    d.height = h;
    d.num_objects = 1;
    d.objects[0].fd = fd;
    d.objects[0].size = (uint32_t)total;
    d.objects[0].drm_format_modifier = mod;

    if (!planar) {
        d.num_layers = 1;
        d.layers[0].drm_format = DRM_FORMAT_XRGB8888;
        d.layers[0].num_planes = 1;
        d.layers[0].object_index[0] = 0;
        d.layers[0].offset[0] = offY;
        d.layers[0].pitch[0] = pitchY;
    } else if (split_layers) {
        d.num_layers = 2;
        d.layers[0].drm_format = DRM_FORMAT_R8;
        d.layers[0].num_planes = 1;
        d.layers[0].object_index[0] = 0;
        d.layers[0].offset[0] = offY;
        d.layers[0].pitch[0] = pitchY;
        d.layers[1].drm_format = DRM_FORMAT_GR88;
        d.layers[1].num_planes = 1;
        d.layers[1].object_index[0] = 0;
        d.layers[1].offset[0] = offUV;
        d.layers[1].pitch[0] = pitchUV;
    } else {
        d.num_layers = 1;
        d.layers[0].drm_format = DRM_FORMAT_NV12;
        d.layers[0].num_planes = 2;
        d.layers[0].object_index[0] = 0;
        d.layers[0].offset[0] = offY;
        d.layers[0].pitch[0] = pitchY;
        d.layers[0].object_index[1] = 0;
        d.layers[0].offset[1] = offUV;
        d.layers[0].pitch[1] = pitchUV;
    }

    VASurfaceAttrib attr[2];
    memset(attr, 0, sizeof(attr));
    attr[0].type = VASurfaceAttribMemoryType;
    attr[0].flags = VA_SURFACE_ATTRIB_SETTABLE;
    attr[0].value.type = VAGenericValueTypeInteger;
    attr[0].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2;
    attr[1].type = VASurfaceAttribExternalBufferDescriptor;
    attr[1].flags = VA_SURFACE_ATTRIB_SETTABLE;
    attr[1].value.type = VAGenericValueTypePointer;
    attr[1].value.value.p = &d;

    VASurfaceID s = VA_INVALID_SURFACE;
    VAStatus st = vaCreateSurfaces(dpy, rtfmt, w, h, &s, 1, attr, 2);
    printf("  %-46s %s%s\n", label,
           st == VA_STATUS_SUCCESS ? "OK" : "FAIL: ",
           st == VA_STATUS_SUCCESS ? "" : vaErrorStr(st));
    if (st == VA_STATUS_SUCCESS) vaDestroySurfaces(dpy, &s, 1);
    close(fd);
}

int main(void)
{
    drm_fd = open("/dev/dri/renderD129", O_RDWR);
    if (drm_fd < 0) { perror("open renderD129"); return 1; }
    gbm = gbm_create_device(drm_fd);
    if (!gbm) { printf("gbm_create_device failed\n"); return 1; }
    setenv("LIBVA_DRIVER_NAME", "iHD", 1);
    dpy = vaGetDisplayDRM(drm_fd);
    int maj, min;
    if (vaInitialize(dpy, &maj, &min) != VA_STATUS_SUCCESS) { printf("vaInitialize failed\n"); return 1; }
    printf("VA-API %d.%d, driver: %s\n\n", maj, min, vaQueryVendorString(dpy));

    // The working control from the device: XRGB 1920x1088, one plane, pitch 4*w.
    try_import("XRGB 1920x1088 single-layer (device control)", VA_FOURCC_RGBX, 1920, 1088,
               VA_RT_FORMAT_RGB32, 0, 7680, 0, 0, 0, 8355840, 0);

    // NV12 at a NORMAL size, tight offsets -- is planarity or SIZE the problem?
    try_import("NV12 1920x1088 single-layer 2-plane, tight",  VA_FOURCC_NV12, 1920, 1088,
               VA_RT_FORMAT_YUV420, 1, 1920, 0, 1920, 1920*1088, 1920*1088*3/2, 0);
    try_import("NV12 1920x1088 split-layer, tight",           VA_FOURCC_NV12, 1920, 1088,
               VA_RT_FORMAT_YUV420, 1, 1920, 0, 1920, 1920*1088, 1920*1088*3/2, 1);

    // NIKKE's actual geometry, with the offsets the device really produced.
    try_import("NV12 1920x1920 single-layer 2-plane, dev off", VA_FOURCC_NV12, 1920, 1920,
               VA_RT_FORMAT_YUV420, 1, 1920, 0, 1920, 3932160, 5775360, 0);
    try_import("NV12 1920x1920 split-layer, dev off",          VA_FOURCC_NV12, 1920, 1920,
               VA_RT_FORMAT_YUV420, 1, 1920, 0, 1920, 3932160, 5775360, 1);
    try_import("NV12 1920x1920 split-layer, TIGHT off",        VA_FOURCC_NV12, 1920, 1920,
               VA_RT_FORMAT_YUV420, 1, 1920, 0, 1920, 1920*1920, 1920*1920*3/2, 1);
    return 0;
}
