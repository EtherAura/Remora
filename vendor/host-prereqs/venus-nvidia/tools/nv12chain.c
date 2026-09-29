/* Reproduce the whole camera/video preview chain host-side, no guest involved:
 *   1. allocate what vtest allocates: R8 4096x192 linear image, dedicated
 *      memory, HOST_VISIBLE|CACHED, exported as a dmabuf  (the producer's blob)
 *   2. CPU-write a known NV12 pattern through the tight gralloc layout
 *      (Y=0x50 at 0 pitch 800, UV=0x20/0xE0 at 480000 pitch 800)
 *   3. import the fd the way the guest does: NV12 800x600, explicit linear
 *      layout p0[0:800] p1[480000:800], dedicated to the import image
 *   4. GPU-copy plane 0 and plane 1 into a readback buffer and compare.
 * If plane 1 reads zeros while plane 0 reads 0x50, this is the green preview
 * in 300 lines, and the variants tell us which ingredient breaks it. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

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

static void
run_case(const char *tag, int mode) /* 0 plain, 1 dedicated, 2 ALIAS, 3 VRAM, 4 R8 single-plane import,
                                       5 dedicated with plane 1 at a 64K-aligned offset (bd remora-e5x.35:
                                       alignment alone rescues a GBM allocation — does it rescue this one?) */
{
   /* --- 1. producer: R8 4096x192 linear, dedicated, exported ------------- */
   const uint64_t mod_linear = 0;
   const VkSubresourceLayout r8_layout = { .offset = 0, .rowPitch = 4096 };
   const VkImageDrmFormatModifierExplicitCreateInfoEXT r8_mod = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
      .drmFormatModifier = mod_linear,
      .drmFormatModifierPlaneCount = 1,
      .pPlaneLayouts = &r8_layout,
   };
   const VkExternalMemoryImageCreateInfo r8_ext = {
      .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
      .pNext = &r8_mod,
      .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
   };
   const VkImageCreateInfo r8_ci = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .pNext = &r8_ext,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = VK_FORMAT_R8_UNORM,
      .extent = { 4096, 192, 1 },
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
      .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
               VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   VkImage r8_img;
   CHK(vkCreateImage(dev, &r8_ci, NULL, &r8_img));
   VkMemoryRequirements r8_reqs;
   vkGetImageMemoryRequirements(dev, r8_img, &r8_reqs);

   const VkExportMemoryAllocateInfo exp = {
      .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
      .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
   };
   const VkMemoryDedicatedAllocateInfo r8_ded = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
      .pNext = &exp,
      .image = r8_img,
   };
   const VkMemoryAllocateInfo r8_ai = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .pNext = &r8_ded,
      .allocationSize = r8_reqs.size,
      .memoryTypeIndex = mem_type(
         r8_reqs.memoryTypeBits,
         mode == 3 ? (VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
                   : (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT)),
   };
   if (mode == 3) {
      VkPhysicalDeviceMemoryProperties mp;
      vkGetPhysicalDeviceMemoryProperties(phys, &mp);
      printf("  image memoryTypeBits=0x%x\n", r8_reqs.memoryTypeBits);
      for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
         if (r8_reqs.memoryTypeBits & (1u << i))
            printf("    type %u heap %u flags 0x%x\n", i, mp.memoryTypes[i].heapIndex,
                   mp.memoryTypes[i].propertyFlags);
   }
   VkDeviceMemory r8_mem;
   CHK(vkAllocateMemory(dev, &r8_ai, NULL, &r8_mem));
   CHK(vkBindImageMemory(dev, r8_img, r8_mem, 0));

   /* --- 2. CPU-write the tight NV12 pattern ------------------------------ */
   void *map;
   CHK(vkMapMemory(dev, r8_mem, 0, VK_WHOLE_SIZE, 0, &map));
   /* every aligned u32 in the buffer holds its own byte offset, so any byte
    * the GPU copies back names exactly where the driver read it from */
   unsigned *w = map;
   for (size_t i = 0; i < r8_reqs.size / 4; i++)
      w[i] = (unsigned)(i * 4) | 0x40000000u;
   vkUnmapMemory(dev, r8_mem);

   const VkMemoryGetFdInfoKHR gfd = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
      .memory = r8_mem,
      .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
   };
   PFN_vkGetMemoryFdKHR getfd =
      (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(dev, "vkGetMemoryFdKHR");
   int fd = -1;
   CHK(getfd(dev, &gfd, &fd));
   const off_t fd_size = lseek(fd, 0, SEEK_END);

   /* --- 3. consumer: NV12 with the guest's explicit layout --------------- */
   const VkSubresourceLayout nv12_layout[2] = {
      { .offset = 0, .rowPitch = 800 },
      /* mode 5: plane 1 on a 64 KiB boundary. On a GBM-on-nvidia-drm
       * allocation this alignment alone makes the import read every byte
       * correctly (gbmchain.c); here it shows whether the sysmem-export
       * defect is the same disease or a second, independent one. */
      { .offset = mode == 5 ? 524288 : 480000, .rowPitch = 800 },
   };
   const VkImageDrmFormatModifierExplicitCreateInfoEXT nv12_mod = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
      .drmFormatModifier = mod_linear,
      .drmFormatModifierPlaneCount = 2,
      .pPlaneLayouts = nv12_layout,
   };
   const VkExternalMemoryImageCreateInfo nv12_ext = {
      .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
      .pNext = &nv12_mod,
      .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
   };
   const VkImageCreateInfo nv12_ci = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .pNext = &nv12_ext,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
      .extent = { 800, 600, 1 },
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
      .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   VkImageCreateInfo consumer_ci = nv12_ci;
   if (mode == 4) {
      consumer_ci = r8_ci; /* same single-plane shape the producer used */
      consumer_ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
   }
   VkImage nv12_img;
   CHK(vkCreateImage(dev, &consumer_ci, NULL, &nv12_img));
   VkMemoryRequirements nv12_reqs;
   vkGetImageMemoryRequirements(dev, nv12_img, &nv12_reqs);

   const VkMemoryDedicatedAllocateInfo nv12_ded = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
      .image = nv12_img,
   };
   const VkImportMemoryFdInfoKHR imp = {
      .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
      .pNext = (mode == 1 || mode == 3 || mode == 5) ? (const void *)&nv12_ded : NULL,
      .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
      .fd = fd,
   };
   const VkMemoryAllocateInfo nv12_ai = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .pNext = &imp,
      .allocationSize = nv12_reqs.size,
      .memoryTypeIndex = mem_type(nv12_reqs.memoryTypeBits, 0),
   };
   VkDeviceMemory nv12_mem = VK_NULL_HANDLE;
   if (mode == 2) {
      close(fd); /* alias path never consumes the fd */
      VkResult br = vkBindImageMemory(dev, nv12_img, r8_mem, 0);
      printf("[%s] ALIAS bind to producer memory -> %d\n", tag, br);
      if (br != VK_SUCCESS)
         goto out_producer;
   } else {
      VkResult ir = vkAllocateMemory(dev, &nv12_ai, NULL, &nv12_mem);
      printf("[%s] fd_size=%lld import reqs.size=%llu alloc -> %d\n", tag,
             (long long)fd_size, (unsigned long long)nv12_reqs.size, ir);
      if (ir != VK_SUCCESS)
         goto out_producer;
      CHK(vkBindImageMemory(dev, nv12_img, nv12_mem, 0));
   }

   /* --- 4. GPU copy both planes out -------------------------------------- */
   {
      const VkBufferCreateInfo bci = {
         .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
         .size = 800 * 600 + 400 * 300 * 2,
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
      const VkCommandBufferBeginInfo bi = {
         .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      };
      CHK(vkBeginCommandBuffer(cb, &bi));
      VkImageMemoryBarrier bar = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
         .srcAccessMask = 0,
         .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
         .oldLayout = VK_IMAGE_LAYOUT_PREINITIALIZED,
         .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT,
         .dstQueueFamilyIndex = qfam,
         .image = nv12_img,
         .subresourceRange = { VK_IMAGE_ASPECT_PLANE_0_BIT | VK_IMAGE_ASPECT_PLANE_1_BIT,
                               0, 1, 0, 1 },
      };
      if (mode == 4)
         bar.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1,
                           &bar);
      VkBufferImageCopy cp[2] = {
         { .bufferOffset = 0,
           .imageSubresource = { VK_IMAGE_ASPECT_PLANE_0_BIT, 0, 0, 1 },
           .imageExtent = { 800, 600, 1 } },
         { .bufferOffset = 800 * 600,
           .imageSubresource = { VK_IMAGE_ASPECT_PLANE_1_BIT, 0, 0, 1 },
           .imageExtent = { 400, 300, 1 } },
      };
      uint32_t ncp = 2;
      if (mode == 4) {
         cp[0].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
         cp[0].imageExtent.width = 800;   /* first 800 bytes of each 4096 row */
         cp[0].imageExtent.height = 600;
         cp[0].bufferRowLength = 0;
         ncp = 1;
      }
      vkCmdCopyImageToBuffer(cb, nv12_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb, ncp,
                             cp);
      CHK(vkEndCommandBuffer(cb));
      const VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                                .commandBufferCount = 1,
                                .pCommandBuffers = &cb };
      CHK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
      CHK(vkQueueWaitIdle(queue));

      unsigned char *out;
      CHK(vkMapMemory(dev, bmem, 0, VK_WHOLE_SIZE, 0, (void **)&out));
      const unsigned *ow = (const unsigned *)out;
      printf("[%s] p0 r0c0 src@%u (zero=999999999), r1c0 src@%u, r2c0 src@%u | "
             "p1 r0c0 src@%u, r1c0 src@%u\n",
             tag, ow[0]==0?999999999:ow[0]&0x3fffffffu, ow[800/4]==0?999999999:ow[800/4]&0x3fffffffu, ow[1600/4]==0?999999999:ow[1600/4]&0x3fffffffu,
             ow[(800 * 600) / 4] == 0 ? 999999999 : ow[(800 * 600) / 4] & 0x3fffffffu, ow[(800 * 600 + 800) / 4] == 0 ? 999999999 : ow[(800 * 600 + 800) / 4] & 0x3fffffffu);
      vkUnmapMemory(dev, bmem);
      vkDestroyBuffer(dev, rb, NULL);
      vkFreeMemory(dev, bmem, NULL);
      vkFreeCommandBuffers(dev, pool, 1, &cb);
   }
   if (nv12_mem != VK_NULL_HANDLE)
      vkFreeMemory(dev, nv12_mem, NULL);
out_producer:
   vkDestroyImage(dev, nv12_img, NULL);
   vkDestroyImage(dev, r8_img, NULL);
   vkFreeMemory(dev, r8_mem, NULL);
}

int main(void)
{
   VkInstance inst;
   const VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                   .apiVersion = VK_API_VERSION_1_3 };
   const VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                      .pApplicationInfo = &app };
   if (vkCreateInstance(&ici, NULL, &inst) != VK_SUCCESS)
      return 1;
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
      return fprintf(stderr, "no NVIDIA\n"), 1;

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
      return fprintf(stderr, "no device\n"), 1;
   vkGetDeviceQueue(dev, qfam, 0, &queue);
   const VkCommandPoolCreateInfo pci = { .sType =
                                            VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                         .queueFamilyIndex = qfam };
   if (vkCreateCommandPool(dev, &pci, NULL, &pool) != VK_SUCCESS)
      return 1;

   run_case("dedicated import", 1);
   run_case("dedicated ALIGNED", 5);
   run_case("ALIAS bind     ", 2);
   run_case("R8 reimport    ", 4);

   vkDestroyCommandPool(dev, pool, NULL);
   vkDestroyDevice(dev, NULL);
   vkDestroyInstance(inst, NULL);
   return 0;
}
