# ffmpeg-on-NVIDIA host guard

Host-level FFmpeg patch for a SIGSEGV in the host mirror encoder (bd `remora-fi8c`). Like the rest of `host-prereqs/`, this is **host machine configuration** — the
repo documents and versions it, but applying it is an operator action, and nothing in the
runtime depends on it being present.

## The crash

`remora-frame-encoder`'s `scale_vulkan` stage hands a `VK_NULL_HANDLE` imageView to
`vkCmdPushDescriptorSetKHR` (ffmpeg 8.1.2, `libavutil/vulkan.c` `ff_vk_shader_update_img` →
`update_set_pool_write`). The NVIDIA proprietary driver (610.43.03, crash offset `0xf8a1b5`)
NULL-checks the **sampler** but dereferences the **view** unconditionally: `si_addr=0xf0`,
process dead, mirror black. Three cores were captured; two of the three correlated with a
reconnect's VRAM/display churn.

How a NULL view gets that far is **unproven** — `ff_vk_create_imageviews`' creation loop is
RET-checked, so the candidate is the driver returning `VK_SUCCESS` with a null handle under
resource pressure. Remora's connect gate detects the resulting
never-served mirror and fails loudly with the encoder log tail, so a recurrence is visible;
this patch is about not dying in the first place.

## The guard

[`0001-vulkan-refuse-NULL-imageView-descriptor-writes.patch`](0001-vulkan-refuse-NULL-imageView-descriptor-writes.patch)
makes `ff_vk_shader_update_img` skip (and `av_log`-warn about) a descriptor write whose
imageView is `VK_NULL_HANDLE` instead of pushing it into the driver. Worst case is one frame's
sampling correctness on that binding; the process and the mirror survive. The per-plane callers
already ignore individual write failures, so the skip degrades instead of aborting the chain.

## Install

The patch applies cleanly against FFmpeg 8.1.2. How to carry it depends on how the host builds
FFmpeg; on Gentoo it rides `/etc/portage/patches` and survives every rebuild and version bump that
still applies cleanly:

```bash
sudo mkdir -p /etc/portage/patches/media-video/ffmpeg
sudo cp 0001-vulkan-refuse-NULL-imageView-descriptor-writes.patch /etc/portage/patches/media-video/ffmpeg/
sudo emerge --oneshot media-video/ffmpeg
```

`user patches applied` in the emerge output confirms pickup. After the rebuild, a healthy
encoder run logs nothing new; if the underlying condition ever recurs, the encoder log carries
`Refusing descriptor write of a VK_NULL_HANDLE imageView` **instead of a coredump**, and that
line plus the connect gate's log tail is the dataset the root-cause theory needs.

## Adjacent defects

Two more defects were found by inspection while chasing this, neither provably the crash:
`ff_vk_filter_process_simple` ignores `ff_vk_exec_start`'s return, and `ff_vk_create_imageviews`
memcpys from a freed buffer on its `ff_vk_exec_add_dep_buf` failure path. The guard above does not
address either; they are recorded here so the next person chasing a Vulkan-filter crash starts
from them.
