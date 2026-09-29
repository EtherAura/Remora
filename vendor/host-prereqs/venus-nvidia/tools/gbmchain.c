/* bd remora-e5x.35: can a GBM-on-nvidia-drm allocation replace the Vulkan
 * sysmem export whose re-import NVIDIA corrupts (nv12chain.c)?  Three gates,
 * each printed as its own verdict so a partial failure still says which leg
 * died:
 *   1. ALLOCATE — gbm_bo_create, linear, single-plane blob geometry (the
 *      shape vtest_gpu_alloc_gpu_linear serves today: R8 4096x192).
 *   2. CPU WRITE — the guest is a container that mmap()s the dmabuf fd
 *      directly, so mmap is the path that matters; gbm_bo_map is the
 *      host-side fallback the vtest transfer protocol could serve.
 *   3. RE-IMPORT — the fd as the guest's NV12 800x600 tight layout
 *      (p0[0:800] p1[480000:800], dedicated) and as a same-shape R8 image,
 *      GPU-copy back, and compare against the self-describing pattern
 *      (every aligned u32 holds its own byte offset).
 * Build: gcc -O1 -o gbmchain gbmchain.c -lgbm -ldrm -lvulkan
 * Watch `journalctl -k -f` while it runs: the sysmem defect logs
 * intermapRegisterDmaMapping failures per import attempt. */
#include <fcntl.h>
#include <gbm.h>
#include <linux/dma-buf.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <vulkan/vulkan.h>
#include <xf86drm.h>

#define CHK(x)                                                                 \
   do {                                                                        \
      VkResult _r = (x);                                                       \
      if (_r != VK_SUCCESS) {                                                  \
         printf("FAIL %s -> %d (line %d)\n", #x, _r, __LINE__);                \
         return;                                                               \
      }                                                                        \
   } while (0)

static VkPhysicalDevice phys;
static VkDevice dev;
static VkQueue queue;
static VkCommandPool pool;
static uint32_t qfam;

static uint32_t
mem_type(uint32_t bits, VkMemoryPropertyFlags want)
{
   VkPhysicalDeviceMemoryProperties mp;
   vkGetPhysicalDeviceMemoryProperties(phys, &mp);
   for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
      if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
         return i;
   for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
      if (bits & (1u << i))
         return i;
   return 0;
}

/* import `fd` as an image and GPU-copy it back out; nplanes==2 is the guest's
 * NV12 (800x600, tight pitches at p0_off/p1_off), nplanes==1 re-reads the blob
 * at its own geometry (width x height x pitch, R8).  Every returned u32 names
 * the source offset the driver actually read, so the scan below can count
 * exactly how many bytes came from the wrong place. */
static void
run_import(const char *tag, int fd, int nplanes, uint32_t width, uint32_t height,
           uint32_t pitch, uint64_t p0_off, uint64_t p1_off)
{
   const off_t fd_size = lseek(fd, 0, SEEK_END);

   const VkSubresourceLayout layouts[2] = {
      { .offset = p0_off, .rowPitch = pitch },
      { .offset = p1_off, .rowPitch = pitch },
   };
   const VkImageDrmFormatModifierExplicitCreateInfoEXT mod = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
      .drmFormatModifier = 0, /* DRM_FORMAT_MOD_LINEAR */
      .drmFormatModifierPlaneCount = (uint32_t)nplanes,
      .pPlaneLayouts = layouts,
   };
   const VkExternalMemoryImageCreateInfo ext = {
      .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
      .pNext = &mod,
      .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
   };
   const VkImageCreateInfo ci = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .pNext = &ext,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = nplanes == 2 ? VK_FORMAT_G8_B8R8_2PLANE_420_UNORM : VK_FORMAT_R8_UNORM,
      .extent = { width, height, 1 },
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
      .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   VkImage img;
   CHK(vkCreateImage(dev, &ci, NULL, &img));
   VkMemoryRequirements reqs;
   vkGetImageMemoryRequirements(dev, img, &reqs);

   PFN_vkGetMemoryFdPropertiesKHR fdprops =
      (PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(dev, "vkGetMemoryFdPropertiesKHR");
   VkMemoryFdPropertiesKHR fp = { .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR };
   VkResult pr = fdprops(dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &fp);
   printf("[%s] fd_size=%lld import reqs.size=%llu align=%llu fd memoryTypeBits=0x%x (query -> %d)\n",
          tag, (long long)fd_size, (unsigned long long)reqs.size,
          (unsigned long long)reqs.alignment, pr == VK_SUCCESS ? fp.memoryTypeBits : 0, pr);

   /* vkAllocateMemory consumes the fd on success — import a dup */
   int import_fd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
   const VkMemoryDedicatedAllocateInfo ded = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
      .image = img,
   };
   const VkImportMemoryFdInfoKHR imp = {
      .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
      .pNext = &ded,
      .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
      .fd = import_fd,
   };
   uint32_t allowed = reqs.memoryTypeBits;
   if (pr == VK_SUCCESS && (reqs.memoryTypeBits & fp.memoryTypeBits))
      allowed = reqs.memoryTypeBits & fp.memoryTypeBits;
   const VkMemoryAllocateInfo ai = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .pNext = &imp,
      .allocationSize = reqs.size,
      .memoryTypeIndex = mem_type(allowed, 0),
   };
   VkDeviceMemory mem = VK_NULL_HANDLE;
   VkResult ir = vkAllocateMemory(dev, &ai, NULL, &mem);
   printf("[%s] import alloc -> %d\n", tag, ir);
   if (ir != VK_SUCCESS) {
      close(import_fd);
      vkDestroyImage(dev, img, NULL);
      return;
   }
   CHK(vkBindImageMemory(dev, img, mem, 0));

   /* GPU copy out.  Tight packing: plane0 rows are `width` bytes, plane1 rows
    * (NV12) are width bytes of R8G8 at buffer offset width*height. */
   const uint64_t p0_bytes = (uint64_t)width * height;
   const uint64_t out_bytes = nplanes == 2 ? p0_bytes + p0_bytes / 2 : p0_bytes;
   const VkBufferCreateInfo bci = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = out_bytes,
      .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
   };
   VkBuffer rb;
   CHK(vkCreateBuffer(dev, &bci, NULL, &rb));
   VkMemoryRequirements breqs;
   vkGetBufferMemoryRequirements(dev, rb, &breqs);
   const VkMemoryAllocateInfo bai = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = breqs.size,
      .memoryTypeIndex = mem_type(breqs.memoryTypeBits,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
   };
   VkDeviceMemory bmem;
   CHK(vkAllocateMemory(dev, &bai, NULL, &bmem));
   CHK(vkBindBufferMemory(dev, rb, bmem, 0));

   const VkCommandBufferAllocateInfo cai = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1,
   };
   VkCommandBuffer cb;
   CHK(vkAllocateCommandBuffers(dev, &cai, &cb));
   const VkCommandBufferBeginInfo bi = { .sType =
                                            VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
   CHK(vkBeginCommandBuffer(cb, &bi));
   const VkImageMemoryBarrier bar = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .srcAccessMask = 0,
      .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_PREINITIALIZED,
      .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT,
      .dstQueueFamilyIndex = qfam,
      .image = img,
      .subresourceRange = { nplanes == 2
                               ? VK_IMAGE_ASPECT_PLANE_0_BIT | VK_IMAGE_ASPECT_PLANE_1_BIT
                               : VK_IMAGE_ASPECT_COLOR_BIT,
                            0, 1, 0, 1 },
   };
   vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &bar);
   const VkBufferImageCopy cp[2] = {
      { .bufferOffset = 0,
        .imageSubresource = { nplanes == 2 ? VK_IMAGE_ASPECT_PLANE_0_BIT
                                           : VK_IMAGE_ASPECT_COLOR_BIT,
                              0, 0, 1 },
        .imageExtent = { width, height, 1 } },
      { .bufferOffset = p0_bytes,
        .imageSubresource = { VK_IMAGE_ASPECT_PLANE_1_BIT, 0, 0, 1 },
        .imageExtent = { width / 2, height / 2, 1 } },
   };
   vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb,
                          nplanes == 2 ? 2 : 1, cp);
   CHK(vkEndCommandBuffer(cb));
   const VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                             .commandBufferCount = 1,
                             .pCommandBuffers = &cb };
   CHK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
   CHK(vkQueueWaitIdle(queue));

   /* the scan: for plane p, row r, aligned column c the copied u32 must name
    * src offset  p_off + r*pitch + c.  Anything else is the defect. */
   const unsigned *out;
   CHK(vkMapMemory(dev, bmem, 0, VK_WHOLE_SIZE, 0, (void **)&out));
   for (int p = 0; p < nplanes; p++) {
      const uint32_t rows = p ? height / 2 : height;
      const uint64_t buf_base = p ? p0_bytes : 0;
      const uint64_t src_base = p ? p1_off : p0_off;
      uint64_t bad = 0, zero = 0, first_bad = 0, first_want = 0, first_got = 0;
      for (uint32_t r = 0; r < rows; r++)
         for (uint32_t c = 0; c < width; c += 4) {
            const unsigned got = out[(buf_base + (uint64_t)r * width + c) / 4];
            const unsigned want = (unsigned)(src_base + r * pitch + c) | 0x40000000u;
            if (got == want)
               continue;
            if (!got)
               zero++;
            else if (!bad) {
               first_bad = buf_base + (uint64_t)r * width + c;
               first_want = want & 0x3fffffffu;
               first_got = got & 0x3fffffffu;
            }
            if (got)
               bad++;
         }
      if (!bad && !zero)
         printf("[%s] p%d OK — every u32 came from its own offset\n", tag, p);
      else
         printf("[%s] p%d BROKEN — %llu misplaced, %llu zero (of %u); first bad "
                "buf@%llu want src@%llu got src@%llu\n",
                tag, p, (unsigned long long)bad, (unsigned long long)zero,
                rows * width / 4, (unsigned long long)first_bad,
                (unsigned long long)first_want, (unsigned long long)first_got);
   }
   vkUnmapMemory(dev, bmem);

   vkDestroyBuffer(dev, rb, NULL);
   vkFreeMemory(dev, bmem, NULL);
   vkFreeCommandBuffers(dev, pool, 1, &cb);
   vkFreeMemory(dev, mem, NULL);
   vkDestroyImage(dev, img, NULL);
}

static int
vk_init(void)
{
   VkInstance inst;
   const VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                   .apiVersion = VK_API_VERSION_1_3 };
   const VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                      .pApplicationInfo = &app };
   if (vkCreateInstance(&ici, NULL, &inst) != VK_SUCCESS)
      return -1;
   uint32_t n = 0;
   vkEnumeratePhysicalDevices(inst, &n, NULL);
   VkPhysicalDevice devs[8];
   n = n > 8 ? 8 : n;
   vkEnumeratePhysicalDevices(inst, &n, devs);
   for (uint32_t i = 0; i < n; i++) {
      VkPhysicalDeviceProperties pr;
      vkGetPhysicalDeviceProperties(devs[i], &pr);
      if (strstr(pr.deviceName, "NVIDIA"))
         phys = devs[i];
   }
   if (!phys)
      return fprintf(stderr, "no NVIDIA\n"), -1;

   uint32_t qn = 0;
   vkGetPhysicalDeviceQueueFamilyProperties(phys, &qn, NULL);
   VkQueueFamilyProperties qp[8];
   qn = qn > 8 ? 8 : qn;
   vkGetPhysicalDeviceQueueFamilyProperties(phys, &qn, qp);
   for (uint32_t i = 0; i < qn; i++)
      if (qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
         qfam = i;
         break;
      }

   const char *exts[] = { "VK_EXT_image_drm_format_modifier",
                          "VK_KHR_sampler_ycbcr_conversion",
                          "VK_KHR_external_memory",
                          "VK_KHR_external_memory_fd",
                          "VK_EXT_external_memory_dma_buf",
                          "VK_EXT_queue_family_foreign",
                          "VK_KHR_image_format_list" };
   VkPhysicalDeviceSamplerYcbcrConversionFeatures ycbcr = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES,
      .samplerYcbcrConversion = VK_TRUE,
   };
   float prio = 1.0f;
   const VkDeviceQueueCreateInfo q = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                       .queueFamilyIndex = qfam,
                                       .queueCount = 1,
                                       .pQueuePriorities = &prio };
   const VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                    .pNext = &ycbcr,
                                    .queueCreateInfoCount = 1,
                                    .pQueueCreateInfos = &q,
                                    .enabledExtensionCount = 7,
                                    .ppEnabledExtensionNames = exts };
   if (vkCreateDevice(phys, &dci, NULL, &dev) != VK_SUCCESS)
      return fprintf(stderr, "no device\n"), -1;
   vkGetDeviceQueue(dev, qfam, 0, &queue);
   const VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                         .queueFamilyIndex = qfam };
   if (vkCreateCommandPool(dev, &pci, NULL, &pool) != VK_SUCCESS)
      return -1;
   return 0;
}

/* allocate the vtest blob shape (R8 4096x192) at the given use flags, write
 * the pattern, and run the import matrix over it.  The NV12 aligned variant
 * puts plane 1 on a 64 KiB boundary (524288) to separate "offsets must be
 * 64K-aligned" from "the backing store must be 64K-contiguous" — the kernel's
 * complaint (`non-contig 4KB pages for 64kB mapping`) is about the latter. */
static void
blob_case(struct gbm_device *gbm, uint32_t use, const char *tag)
{
   struct gbm_bo *bo = gbm_bo_create(gbm, 4096, 192, GBM_FORMAT_R8, use);
   if (!bo) {
      printf("=== %s: ALLOCATE FAILED (use=0x%x)\n", tag, use);
      return;
   }
   const uint32_t stride = gbm_bo_get_stride(bo);
   int fd = gbm_bo_get_fd(bo);
   if (fd < 0) {
      printf("=== %s: no fd\n", tag);
      gbm_bo_destroy(bo);
      return;
   }
   const off_t fd_size = lseek(fd, 0, SEEK_END);
   printf("=== %s: stride=%u modifier=0x%llx fd_size=%lld\n", tag, stride,
          (unsigned long long)gbm_bo_get_modifier(bo), (long long)fd_size);

   /* the production shape: vtest sends the fd to the guest and drops every
    * other handle, so the fd must be the allocation's sole owner.  Destroying
    * the bo BEFORE any use proves the dmabuf reference keeps the memory (and
    * its contents) alive on this driver. */
   gbm_bo_destroy(bo);
   bo = NULL;

   void *cpu = mmap(NULL, (size_t)fd_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
   printf("  mmap(dmabuf) -> %s\n", cpu == MAP_FAILED ? "FAILED" : "OK");
   if (cpu == MAP_FAILED) {
      close(fd);
      return;
   }
   struct dma_buf_sync sync = { .flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE };
   ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
   unsigned *wp = cpu;
   for (size_t i = 0; i < (size_t)fd_size / 4; i++)
      wp[i] = (unsigned)(i * 4) | 0x40000000u;
   sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE;
   ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);

   run_import("NV12 tight  ", fd, 2, 800, 600, 800, 0, 480000);
   run_import("NV12 aligned", fd, 2, 800, 600, 800, 0, 524288);
   run_import("blob R8 self", fd, 1, stride, 192, stride, 0, 0);

   munmap(cpu, (size_t)fd_size);
   close(fd);
}

/* let the nvidia backend lay out NV12 itself: whatever offsets and strides it
 * picks are by definition what its own import path expects to see. */
static void
nv12_direct_case(struct gbm_device *gbm, uint32_t use, const char *tag)
{
   struct gbm_bo *bo = gbm_bo_create(gbm, 800, 600, GBM_FORMAT_NV12, use);
   if (!bo) {
      printf("=== %s: ALLOCATE FAILED (use=0x%x)\n", tag, use);
      return;
   }
   const int planes = gbm_bo_get_plane_count(bo);
   const uint32_t p0_pitch = gbm_bo_get_stride_for_plane(bo, 0);
   const uint32_t p1_pitch = planes > 1 ? gbm_bo_get_stride_for_plane(bo, 1) : 0;
   const uint64_t p0_off = gbm_bo_get_offset(bo, 0);
   const uint64_t p1_off = planes > 1 ? gbm_bo_get_offset(bo, 1) : 0;
   int fd = gbm_bo_get_fd(bo);
   if (fd < 0) {
      printf("=== %s: no fd\n", tag);
      gbm_bo_destroy(bo);
      return;
   }
   const off_t fd_size = lseek(fd, 0, SEEK_END);
   printf("=== %s: planes=%d p0[%llu:%u] p1[%llu:%u] modifier=0x%llx fd_size=%lld\n",
          tag, planes, (unsigned long long)p0_off, p0_pitch,
          (unsigned long long)p1_off, p1_pitch,
          (unsigned long long)gbm_bo_get_modifier(bo), (long long)fd_size);

   void *cpu = mmap(NULL, (size_t)fd_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
   printf("  mmap(dmabuf) -> %s\n", cpu == MAP_FAILED ? "FAILED" : "OK");
   if (cpu == MAP_FAILED || planes != 2 || p0_pitch != p1_pitch) {
      if (cpu != MAP_FAILED)
         munmap(cpu, (size_t)fd_size);
      close(fd);
      gbm_bo_destroy(bo);
      return;
   }
   struct dma_buf_sync sync = { .flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE };
   ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
   unsigned *wp = cpu;
   for (size_t i = 0; i < (size_t)fd_size / 4; i++)
      wp[i] = (unsigned)(i * 4) | 0x40000000u;
   sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE;
   ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);

   run_import("NV12 own-layout", fd, 2, 800, 600, p0_pitch, p0_off, p1_off);

   munmap(cpu, (size_t)fd_size);
   close(fd);
   gbm_bo_destroy(bo);
}

int
main(void)
{
   /* --- the nvidia-drm node ---------------------------------------------- */
   int drm_fd = -1;
   char node[32] = "";
   for (int i = 128; i < 136 && drm_fd < 0; i++) {
      snprintf(node, sizeof(node), "/dev/dri/renderD%d", i);
      int fd = open(node, O_RDWR | O_CLOEXEC);
      if (fd < 0)
         continue;
      drmVersionPtr v = drmGetVersion(fd);
      if (v && !strcmp(v->name, "nvidia-drm"))
         drm_fd = fd;
      else
         close(fd);
      if (v)
         drmFreeVersion(v);
   }
   if (drm_fd < 0)
      return fprintf(stderr, "no nvidia-drm render node\n"), 1;

   struct gbm_device *gbm = gbm_create_device(drm_fd);
   if (!gbm)
      return fprintf(stderr, "gbm_create_device failed on %s\n", node), 1;
   printf("gbm backend \"%s\" on %s\n", gbm_device_get_backend_name(gbm), node);

   /* --- gate 1: what will it allocate linear? ---------------------------- */
   const struct { uint32_t fmt; const char *name; uint32_t bpp; } fmts[] = {
      { GBM_FORMAT_R8, "R8", 1 },
      { GBM_FORMAT_GR88, "GR88", 2 },
      { GBM_FORMAT_NV12, "NV12", 1 },
      { GBM_FORMAT_ARGB8888, "ARGB8888", 4 },
      { GBM_FORMAT_XRGB8888, "XRGB8888", 4 },
   };
   for (unsigned i = 0; i < sizeof(fmts) / sizeof(fmts[0]); i++)
      printf("format %-8s linear:%d linear+scanout:%d\n", fmts[i].name,
             gbm_device_is_format_supported(gbm, fmts[i].fmt, GBM_BO_USE_LINEAR),
             gbm_device_is_format_supported(gbm, fmts[i].fmt,
                                            GBM_BO_USE_LINEAR | GBM_BO_USE_SCANOUT));

   if (vk_init())
      return 1;

   /* the blob shape at each allocation class, then the driver's own NV12
    * layout — the matrix separates use-flags from offset alignment from
    * backing-store contiguity. */
   blob_case(gbm, GBM_BO_USE_LINEAR, "blob LINEAR");
   blob_case(gbm, GBM_BO_USE_LINEAR | GBM_BO_USE_SCANOUT, "blob LINEAR|SCANOUT");
   nv12_direct_case(gbm, GBM_BO_USE_LINEAR, "gbm NV12 LINEAR");
   nv12_direct_case(gbm, GBM_BO_USE_LINEAR | GBM_BO_USE_SCANOUT,
                    "gbm NV12 LINEAR|SCANOUT");

   gbm_device_destroy(gbm);
   close(drm_fd);
   return 0;
}
