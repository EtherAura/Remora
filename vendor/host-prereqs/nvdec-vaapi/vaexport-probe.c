/* Does this host's NVIDIA VA driver export NVDEC surfaces as dma_bufs?
 * The keystone question for bd remora-e5x.3's host-decode design: ffmpeg's hwmap
 * path returns ENOSYS without (apparently) calling the driver, so ask libva
 * directly, both layer flavours. */
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>

static const char *st(VAStatus s) { return vaErrorStr(s); }

int main(void)
{
   int fd = open("/dev/dri/renderD128", O_RDWR);
   if (fd < 0) { perror("open"); return 1; }
   VADisplay dpy = vaGetDisplayDRM(fd);
   int maj, min;
   VAStatus vs = vaInitialize(dpy, &maj, &min);
   printf("vaInitialize: %s (VA %d.%d, vendor: %s)\n", st(vs), maj, min,
          vs == VA_STATUS_SUCCESS ? vaQueryVendorString(dpy) : "?");
   if (vs != VA_STATUS_SUCCESS) return 1;

   VASurfaceAttrib attr = {
      .type = VASurfaceAttribPixelFormat,
      .flags = VA_SURFACE_ATTRIB_SETTABLE,
      .value = { .type = VAGenericValueTypeInteger, .value.i = VA_FOURCC_NV12 },
   };
   VASurfaceID surf;
   vs = vaCreateSurfaces(dpy, VA_RT_FORMAT_YUV420, 1280, 720, &surf, 1, &attr, 1);
   printf("vaCreateSurfaces: %s\n", st(vs));
   if (vs != VA_STATUS_SUCCESS) return 1;

   struct { const char *name; uint32_t flags; } runs[] = {
      { "SEPARATE_LAYERS|READ_ONLY",
        VA_EXPORT_SURFACE_SEPARATE_LAYERS | VA_EXPORT_SURFACE_READ_ONLY },
      { "COMPOSED_LAYERS|READ_ONLY",
        VA_EXPORT_SURFACE_COMPOSED_LAYERS | VA_EXPORT_SURFACE_READ_ONLY },
      { "SEPARATE_LAYERS|READ_WRITE",
        VA_EXPORT_SURFACE_SEPARATE_LAYERS | VA_EXPORT_SURFACE_READ_WRITE },
   };
   for (unsigned i = 0; i < sizeof(runs) / sizeof(runs[0]); i++) {
      VADRMPRIMESurfaceDescriptor d = { 0 };
      vs = vaExportSurfaceHandle(dpy, surf, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                                 runs[i].flags, &d);
      printf("vaExportSurfaceHandle[%s]: %s", runs[i].name, st(vs));
      if (vs == VA_STATUS_SUCCESS) {
         printf("  fourcc=%.4s %ux%u objects=%u layers=%u",
                (char *)&d.fourcc, d.width, d.height, d.num_objects, d.num_layers);
         for (unsigned o = 0; o < d.num_objects; o++) {
            printf(" [obj%u fd=%d size=%u mod=0x%lx]", o, d.objects[o].fd,
                   d.objects[o].size, (unsigned long)d.objects[o].drm_format_modifier);
            close(d.objects[o].fd);
         }
      }
      printf("\n");
   }
   vaDestroySurfaces(dpy, &surf, 1);
   vaTerminate(dpy);
   close(fd);
   return 0;
}
