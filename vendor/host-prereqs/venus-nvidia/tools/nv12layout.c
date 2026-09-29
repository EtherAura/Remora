/* Create linear NV12 on NVIDIA with the EXACT explicit plane layout the guest
 * camera preview uses (p0[0:800] p1[480000:800], 800x600, modifier 0x0), then
 * ask vkGetImageSubresourceLayout what layout the driver ACTUALLY uses per
 * plane. If the answer differs from what the create supplied, the driver
 * normalized the layout and the sampler walks different offsets than the
 * producer wrote — which is the green-preview mechanism. */
#include <stdio.h>
#include <string.h>
#include <vulkan/vulkan.h>

static VkPhysicalDevice pick_nvidia(VkInstance inst)
{
   uint32_t n = 0;
   vkEnumeratePhysicalDevices(inst, &n, NULL);
   VkPhysicalDevice devs[8];
   n = n > 8 ? 8 : n;
   vkEnumeratePhysicalDevices(inst, &n, devs);
   for (uint32_t i = 0; i < n; i++) {
      VkPhysicalDeviceProperties p;
      vkGetPhysicalDeviceProperties(devs[i], &p);
      if (strstr(p.deviceName, "NVIDIA"))
         return devs[i];
   }
   return VK_NULL_HANDLE;
}

static void try_layout(VkDevice dev, uint32_t w, uint32_t h, uint64_t off0,
                       uint64_t pitch0, uint64_t off1, uint64_t pitch1)
{
   const VkSubresourceLayout want[2] = {
      { .offset = off0, .rowPitch = pitch0 },
      { .offset = off1, .rowPitch = pitch1 },
   };
   const VkImageDrmFormatModifierExplicitCreateInfoEXT explicit_ci = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
      .drmFormatModifier = 0, /* LINEAR */
      .drmFormatModifierPlaneCount = 2,
      .pPlaneLayouts = want,
   };
   const VkExternalMemoryImageCreateInfo ext = {
      .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
      .pNext = &explicit_ci,
      .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
   };
   const VkImageCreateInfo ci = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .pNext = &ext,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
      .extent = { w, h, 1 },
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
      .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   VkImage img;
   VkResult r = vkCreateImage(dev, &ci, NULL, &img);
   printf("create %ux%u p0[%llu:%llu] p1[%llu:%llu] -> %d\n", w, h,
          (unsigned long long)off0, (unsigned long long)pitch0,
          (unsigned long long)off1, (unsigned long long)pitch1, r);
   if (r != VK_SUCCESS)
      return;
   for (int pl = 0; pl < 2; pl++) {
      const VkImageSubresource sub = {
         .aspectMask = pl == 0 ? VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT
                               : VK_IMAGE_ASPECT_MEMORY_PLANE_1_BIT_EXT,
      };
      VkSubresourceLayout got;
      vkGetImageSubresourceLayout(dev, img, &sub, &got);
      printf("  driver plane %d: offset=%llu rowPitch=%llu size=%llu%s\n", pl,
             (unsigned long long)got.offset, (unsigned long long)got.rowPitch,
             (unsigned long long)got.size,
             (got.offset == want[pl].offset && got.rowPitch == want[pl].rowPitch)
                ? "  (matches)"
                : "  (DIFFERS from supplied!)");
   }
   VkMemoryRequirements reqs;
   vkGetImageMemoryRequirements(dev, img, &reqs);
   printf("  reqs: size=%llu alignment=%llu\n", (unsigned long long)reqs.size,
          (unsigned long long)reqs.alignment);
   vkDestroyImage(dev, img, NULL);
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
   VkPhysicalDevice phys = pick_nvidia(inst);
   if (!phys)
      return fprintf(stderr, "no NVIDIA device\n"), 1;

   const char *exts[] = { "VK_KHR_image_format_list", "VK_EXT_image_drm_format_modifier",
                          "VK_KHR_sampler_ycbcr_conversion", "VK_KHR_maintenance1",
                          "VK_KHR_bind_memory2", "VK_KHR_get_memory_requirements2",
                          "VK_KHR_external_memory", "VK_KHR_external_memory_fd",
                          "VK_EXT_external_memory_dma_buf", "VK_EXT_queue_family_foreign" };
   float prio = 1.0f;
   const VkDeviceQueueCreateInfo q = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                       .queueCount = 1,
                                       .pQueuePriorities = &prio };
   const VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                    .queueCreateInfoCount = 1,
                                    .pQueueCreateInfos = &q,
                                    .enabledExtensionCount = 10,
                                    .ppEnabledExtensionNames = exts };
   VkDevice dev;
   if (vkCreateDevice(phys, &dci, NULL, &dev) != VK_SUCCESS)
      return fprintf(stderr, "no device\n"), 1;

   /* the guest's exact camera-preview layout */
   try_layout(dev, 800, 600, 0, 800, 480000, 800);
   /* 256-aligned pitch, 4K-aligned chroma offset */
   try_layout(dev, 800, 600, 0, 1024, 614400, 1024);
   /* the blob's real backing pitch (4096) as the plane pitch */
   try_layout(dev, 800, 600, 0, 4096, 2457600, 4096);

   vkDestroyDevice(dev, NULL);
   vkDestroyInstance(inst, NULL);
   return 0;
}
