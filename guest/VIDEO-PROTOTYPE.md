# Windows guest video prototype

This opt-in build tests whether a Linux guest on Windows can encode H.264 and H.265 through
virglrenderer and the host GPU's Media Foundation encoder. The first test uses FFmpeg in the
guest. Android Codec2 and the Windows mirror need their own live test after that passes.

## Build on Linux

```sh
guest/build.sh --video-prototype
tools/windows-stage.sh build-win dist-windows-video build-guest-video
```

`build-win` is a Windows x64 build of this checkout. The guest build includes virtio-gpu and the
Mesa VA-API driver. Copy `dist-windows-video` to the Windows test system. Enable WHPX and use a GPU
whose Windows driver exposes H.264 and HEVC hardware encoders. A Windows VM also needs nested
virtualization and GPU passthrough.

## Start the guest on Windows

Use the matched QEMU and renderer from [WINQ-EMU Alpha 10](https://github.com/cmspam/winq-emu/releases/tag/alpha10).
The release asset `WINQ-EMU-Alpha10-Setup.exe` has SHA-256
`7a50aa4fac7a53ef911de2c9db756fbc0b2685dd5522f2a4591910c520f15fbe`. Keep its QEMU executables,
DLLs and firmware together. In PowerShell, from the portable Remora folder, point `$winq` at the
WINQ-EMU `bin` folder.

Apply [the renderer patch](winq-async-encoders.patch) to
[the pinned WINQ renderer source](https://github.com/awemorris/virglrenderer/tree/e80354b8f5a0dedc62374b905a181874682659cc)
for asynchronous Media Foundation encoders and correct H.264 and HEVC display dimensions.
Build the renderer for Windows x64 with UCRT and Meson options
`-Dvenus=true -Dvideo=true -Dtests=false -Dvulkan-dload=true -Dplatforms=egl`.
Put the resulting `libvirglrenderer-1.dll` in a copy of the WINQ-EMU Alpha 10 `bin` directory.

```powershell
$winq = 'C:\path\to\WINQ-EMU\bin'
$env:REMORA_GUEST_VIDEO_PROTOTYPE = '1'
$env:REMORA_GUEST_IMAGE_DIR = (Resolve-Path .\guest).Path
$env:REMORA_GUEST_QEMU = Join-Path $winq 'qemu-system-x86_64.exe'
$env:REMORA_GUEST_QEMU_IMG = Join-Path $winq 'qemu-img.exe'
$env:WINQ_DIAG = '1'
.\remora.exe guest start
.\remora.exe guest status
```

Remora enables WINQ-EMU's VA-API backend and opens QEMU's SDL window to provide a GL context.
On WHPX, the video prototype uses WINQ's host CPU and default interrupt controller. It masks
XSAVEC in the Linux guest to work around a boot panic observed with nested WHPX on AMD. AVX and
AVX2 remain available on supported CPUs.
The prototype has its own disks, SSH keys and port range. Stop it with `remora.exe guest stop`
from the same PowerShell session so the guest can power down cleanly.

## Check both codecs

```powershell
$state = Join-Path $env:LOCALAPPDATA 'remora\guest-video-prototype'
$guest = Get-Content (Join-Path $state 'state.json') | ConvertFrom-Json
Push-Location $state
ssh -p $guest.sshPort -i ./client -o BatchMode=yes -o StrictHostKeyChecking=yes -o UserKnownHostsFile=./known_hosts remora@127.0.0.1 remora-video-probe
Pop-Location
```

The probe requires a VA-API encode entrypoint for each codec. It encodes 300 moving 1080p frames
at 60 fps, decodes every frame, and compares the picture to the source. The printed guest directory
holds both `.mkv` files, the encode timings, the `vainfo` report and the decoded-picture checks.
During the run, check the Windows GPU's Video Encode engine and gather the renderer log from
`%LOCALAPPDATA%\winq-emu\virglrenderer.log`. Check H.264 and HEVC separately. The backend stages
input frames in system memory before hardware encoding.

To check Windows hardware decode, copy the two `.mkv` files out of the guest and run this with
Windows FFmpeg for each file. The hardware-frame download makes a software fallback fail the test.

```powershell
ffmpeg -hide_banner -xerror -hwaccel d3d11va -hwaccel_output_format d3d11 -i h264.mkv -vf hwdownload,format=nv12 -f null NUL
ffmpeg -hide_banner -xerror -hwaccel d3d11va -hwaccel_output_format d3d11 -i hevc.mkv -vf hwdownload,format=nv12 -f null NUL
```

## Android image

Copy `remora-a17-virgl-fixed.tar.zst` into the guest and import it there with
`docker load -i remora-a17-virgl-fixed.tar.zst`. The image combines the Android 17 base, its updated
Codec2 VAAPI component, and the rebuilt Mesa stack. When `REMORA_MESA_GALLIUM=iris,radeonsi,virgl`
is selected, the Mesa patch provides RGB-to-NV12 conversion and uses the backing texture's size
to keep allocation padding outside the visible picture. The image uses
`REMORA_MESA_VULKAN=virtio`. [Image build guide](../docs/user/Building-the-Image.md)

Use a test profile targeting the prototype guest. These settings select the image and its render
node.

```ini
android_version=17
image_tag=remora-local:a17-virgl-fixed
gpu_mode=host
gpu_node=/dev/dri/renderD128
va_driver=virtio_gpu
vulkan=virtio
gralloc=minigbm_gbm_mesa
venus=false
use_codec2=true
```

Request `c2.remora.vaapi.avc.encoder` or
`c2.remora.vaapi.hevc.encoder` explicitly in `remora mirror --video-encoder`, with the matching
`--video-codec=h264` or `--video-codec=h265`. Check Android's Codec2 log, the host encoder log, and
the Windows mirror's hardware decode log while showing live motion and recording each codec.

## Second-long frame gaps on a nested Windows host

On Amd-host-b's Ryzen 2700X and passed-through RX 550, the Android frame counter advanced by
one across pauses of up to 1.8 seconds. A Windows waitable timer also stalled with the inner
WHPX guest paused. For this KVM test configuration, expose HPET on the **outer Windows VM**
(`hpet=on` on its Q35 machine, or `HPET=Y` with the Dockur launcher) and select the platform
tick in a dedicated Windows test boot entry.

From an elevated Windows shell, copy the current entry and use the identifier printed by the
first command in the next two commands. The boot sequence selects the copy for one boot.

```powershell
bcdedit /copy '{current}' /d 'Remora nested platform timer test'
bcdedit /set '{copied-guid}' useplatformtick yes
bcdedit /bootsequence '{copied-guid}'
```

After booting that entry, verify `useplatformtick Yes` with `bcdedit /enum '{current}'` and
check that Windows lists a working High precision event timer. Measure distinct frame counters
and the longest interval between consecutive video timestamps for each codec. On the RX 550,
the first pair of 1920×1080 captures requesting 60 fps had maximum intervals of 108 ms for
H.264 and 111 ms for HEVC. Both streams decoded on the Radeon. Microsoft documents
`useplatformtick` as a debugging option. [BCDEdit boot options](https://learn.microsoft.com/en-us/windows-hardware/drivers/devtest/bcdedit--set)

### Pipelined AMD encoder with D3D11 input

For the Android frame-delivery image with pipelined Codec2 and Mesa feedback, apply the
[AMD encoder patch](winq-amf-dx11-upload.patch) **after** the asynchronous Media Foundation
patch above. Place the AMD AMF SDK v1.4.36 checkout at `../amf` relative to the renderer
source tree and build with the same UCRT toolchain and Meson options. [AMD AMF SDK v1.4.36](https://github.com/GPUOpen-LibrariesAndSDKs/AMF/tree/v1.4.36)

Set these environment variables in the process that launches WINQ, before starting the
inner guest. The Windows driver supplies the AMF runtime.

```powershell
$env:WINQ_VAAPI = '1'
$env:WINQ_TEST_AMF = '1'
$env:WINQ_TEST_AMF_REQUIRE_AMD = '1'
$env:WINQ_TEST_AMF_PIPELINE = '1'
$env:WINQ_TEST_AMF_WAIT = 'timer'
$env:WINQ_TEST_AMF_NOMINAL_FPS = '60'
$env:WINQ_TEST_AMF_DX11_UPLOAD = '1'
```

The upload places the NV12 frame into an AMF-owned D3D11 texture on the selected Radeon.
When the coded and visible dimensions differ, the renderer copies the visible rectangle on
the GPU. OpenGL plane readback still runs on each frame. With the timer configuration above,
Amd-host-b delivered 45.15 distinct H.264 frames/s and 39.50 distinct HEVC frames/s over the
capture windows. Both recordings decoded at 1920×1080 through Windows D3D11VA.

### NV12 readback and Android mirror pacing

Apply [the NV12 readback patch](winq-nv12-pack-readback.patch) after the AMD encoder
patch and rebuild the Windows renderer. The Radeon can transfer both NV12 planes into
one reusable OpenGL pixel pack buffer before the CPU maps it. The log entry
`WINQ NV12 pack readback active` identifies this path.

Rebuild the Android 17 Codec2 component, Remora composer and mirror agent for the Android
changes. The VirGL host-GPU profile with Codec2 enabled passes
`androidboot.remora_mirror_only=1` when Venus and host encoding are disabled. This lets
the headless primary skip a duplicate draw while the virtual display renders the mirror.
Recreate an existing test container to pick up the new boot argument.

On Amd-host-b's passed-through RX 550, the three-frame AMF pipeline delivered 59.22
distinct H.264 frames/s and 58.74 distinct HEVC frames/s over the recordings' presentation
timestamps. Including mirror startup and shutdown, the 35-second captures delivered 57.65
and 56.75 distinct frames/s. Both streams decoded through Windows D3D11VA without errors.
After the first five seconds, H.264 delivered 59.86 distinct frames/s and HEVC delivered
59.38. The longest H.264 gap in that interval was 37 ms. The two HEVC gaps longer than
50 ms spanned one increment each on the Android source's visible frame counter.
Five in-flight frames gave 59.41 and 57.82 distinct frames/s on the media timeline and
roughly doubled the time an AMF job spent waiting to be collected. The prototype retains
the three-frame default.

### Android mirror texture reuse

The Android 17 SurfaceFlinger patch sizes the virtual display's GPU texture cache from the
encoder surface's minimum undequeued buffer count plus one render target. It applies when
`androidboot.remora_mirror_only=1`. Rebuild SurfaceFlinger with the Android 17 container
patch series to pick it up. [SurfaceFlinger patch](../vendor/source-build/container-patches/android-17.0.0/frameworks/native/0003-SurfaceFlinger-retain-negotiated-VirGL-mirror-textures.patch)

On Amd-host-b's RX 550 the encoder surface rotated through 18 buffers. In separate 16-second
HEVC Perfetto traces, SurfaceFlinger created 804 GPU textures for 827 mirror draws with
the original cache and 18 for 875 draws with the patch. Time spent creating those textures
fell from 1,211 ms to 10 ms. The 99th-percentile interval between application draws fell
from 63.8 ms to 43.5 ms in those traces.

The following untraced runs requested 1920×1080 at 60 fps with three in-flight AMF frames.
The figures count changes in a visible frame counter after the first five seconds of each
35-second recording. Every recording passed hardware decoding on the RX 550. Both patched
pairs also passed the bottom-row and color checks.

| Codec | Original distinct fps / longest gap | Patched distinct fps / longest gap | Patched repeat |
| --- | ---: | ---: | ---: |
| H.264 | 59.31 / 79 ms | 59.15 / 60 ms | 59.26 / 59 ms |
| HEVC | 57.71 / 75 ms | 57.23 / 73 ms | 57.88 / 57 ms |

Full-recording rates with the patch were 58.10–58.18 distinct fps for H.264 and
56.03–56.07 for HEVC. One HEVC startup gap reached 246 ms. Android still allocates input
buffers on demand. In the original trace, one 140 ms source pause included a 123 ms
allocation request, and an independent 92 ms pause included an 87 ms producer-side GPU
fence wait. Across the four patched recordings, the source counter advanced by one across
nine of the ten gaps over 50 ms after startup. Some source frames also never appeared in
the video. After five seconds, the two H.264 recordings skipped 8 and 5 counter values,
and the HEVC recordings skipped 34 and 75.

During the repeat, the encoder input surface rejected the optional producer
throttling control and used its usual asynchronous queue. Sustained delivery of 60 distinct
frames per second remains open.

### Android encoder input buffers and fences

The Android 17 mirror agent requests a GPU-only RGBA input surface for the Remora AVC and
HEVC encoders. On the VirGL mirror-only boot profile, Codec2 allocates the encoder's render
targets before returning the surface to the agent. The component reads the linear RGB stride
from gralloc metadata. This avoids CPU-mapping a rendered frame to discover its layout.
The local GBM wrapper serializes access to Mesa's shared mapping context and probes a single
full-width row when a linear allocation needs its mapped stride.

Codec2 turns off the redundant producer fence wait on the encoder's own BufferQueue.
SurfaceFlinger configures the named GPU mirror's bounded synchronous queue when display
setup requests swap interval zero. The encoder still waits for rendering to finish on each
input before reading its pixels.

On Amd-host-b's RX 550, the H.265 control trace showed 13 on-demand input allocations.
Twelve started after the first mirror draw and the last finished 3.31 seconds into capture.
With preallocation, all 17 buffers were ready 56 ms before that draw. The producer's
previous-buffer fence waits fell from 874 to zero. In one 86 ms source gap, the app waited
84 ms for a buffer release. The GPU completion waits in the app and SurfaceFlinger
returned within 1 ms of the encoder's current-frame fence wait. Identifying the host
operation behind that delay requires timing from the Windows renderer.

The H.264 trace also recorded all 17 allocations before the first draw, with 52 ms to
spare, and zero previous-buffer fence waits. Its longest source-draw interval was
102 ms. The app spent 95 ms waiting for buffer release, overlapping 46 ms while
SurfaceFlinger imported a render target as a texture for the first time.

Two 35-second captures per codec requested 1920×1080 at 60 fps. The recorded source
counter distinguishes new pictures from duplicate packets. The warm window starts five
seconds after the first picture.

| RX 550 codec | Full recording, distinct fps | Warm, distinct fps | Longest warm gap |
| --- | --- | --- | --- |
| H.264 | 59.03 / 58.89 | 59.61 / 59.22 | 45 / 93 ms |
| H.265 | 57.08 / 58.96 | 57.97 / 59.21 | 78 / 53 ms |

Windows decoded all 8,003 frames through D3D11VA with zero errors. Both codecs passed
the color bars and bottom-row checks. The source counter advanced by one across all eight
warm gaps over 50 ms. The longest startup gap was 273 ms in the first H.265 run.

### Windows GPU fence delivery

The Windows trace matched Android virtio fence sequence numbers to host OpenGL waits and QEMU callbacks. In the H.264 control, one warm fence waited 27.7 ms after submission for its first check. In HEVC, 35.0 ms of an encode command overlapped a 37.2 ms delay before another fence reached the host renderer. GPU packets completed during both intervals.

With binding reuse enabled, WINQ reuses the native Windows OpenGL binding when the renderer context, device context and subcontext still match the last successful bind. Any mismatch takes the normal context switch path.

Apply the Windows fence patch after the NV12 readback patch and rebuild the pinned renderer. The extra check is opt-in. Set it in the environment that launches WINQ for an experimental run. [Windows fence patch](winq-wgl-context-reuse.patch)

```powershell
$env:WINQ_REUSE_WGL_BINDING = '1'
```

The RX 550 ran Windows driver 31.0.21925.1001 with Android 17 under WHPX. The following timings come from separate traces with each codec. Android signal time includes all matched fences.

| Codec | First check p99, control → patched | Android emit-to-signal p99, control → patched |
|---|---:|---:|
| H.264 | 6.39 → 8.14 ms | 16.66 → 17.73 ms |
| H.265 | 7.74 → 9.23 ms | 18.44 → 20.61 ms |

Binding reuse eliminated at least 22,145 redundant GL binds in the H.264 trace and 22,273 in the HEVC trace. The longest context-binding step during fence polling fell from 33.44 to 6.06 ms for H.264 and from 18.37 to 9.32 ms for HEVC. Guest-wide fence p99 increased for both codecs.

The clean renderer build was retested with tracing disabled. One 35-second capture per codec requested 1920×1080 at 60 fps and 12 Mb/s. Delivery counts distinct rendered frame numbers. Warm measurements start five seconds after the first frame.

| Codec | Full distinct fps | Warm distinct fps | Longest warm gap |
|---|---:|---:|---:|
| H.264 | 59.22 | 59.70 | 46 ms |
| H.265 | 58.91 | 59.76 | 38 ms |

Windows hardware-decoded all 4,029 frames with zero decode errors. Both recordings passed the color-bar check and the bottom row was correct in every frame. The longest gaps across the full recordings were 120 ms for H.264 and 138 ms for H.265.


### Windows AMF readback and command latency

Apply the asynchronous NV12 readback patch after the NV12 pack readback patch and rebuild the pinned Windows renderer. The three-frame AMF pipeline with D3D11 upload selects this path when OpenGL supports coherent persistent buffers and D3D11 multithread protection is active. Set `WINQ_ASYNC_NV12_READBACK=0` in the WINQ process environment to run the inline control. [Renderer patch](winq-async-nv12-readback.patch)

The renderer queues both planes into a staging buffer owned by the frame’s queue slot. Its normal fence poll checks when the pixels are ready. A CPU worker then submits the matching descriptor and mapped pixels to AMF. Feedback follows the frame’s ticket, including when the guest reuses an output buffer. The device upload consumes the source bytes before the slot can be reused. [D3D11 UpdateSubresource](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-updatesubresource)

On Amd-host-b’s RX 550, the slowest warm control commands spent 24.97 ms (H.264) and 30.25 ms (HEVC) mapping the pixel pack buffer. A second HEVC recording found a 34.03 ms D3D11 flush in a 36.86 ms command that delayed creation of an Android virtio fence. With the patch, warm command p99 fell from 5.841 to 0.383 ms for H.264 and from 11.834 to 0.355 ms for HEVC. All 3,475 warm commands finished within the 16.67 ms frame budget in the two diagnostic captures.

In the matched HEVC traces, the largest warm overlap between an encode command and a guest fence awaiting host creation fell from 36.86 to 0.70 ms. Warm guest fence p99 fell from 17.68 to 14.07 ms. Its maximum rose from 50.79 to 68.45 ms.

The uninstrumented release tests used the same DLL for the inline control and the default worker. Each rate counts distinct source counters after the first five presentation seconds.

| Codec | Inline control, distinct fps | Default worker, distinct fps | Longest warm gaps with the worker |
|---|---:|---:|---:|
| H.264 | 59.52, 21.45 | 59.92, 59.69 | 40 ms, 81 ms |
| HEVC | 59.40, 59.32 | 59.93, 59.90 | 42 ms, 50 ms |

Across the four default-worker captures, Windows D3D11VA decoded all 8,158 frames without errors. Each recording passed the source-counter ordering, sampled color-bar, and per-frame bottom-row checks. The longest gap across the full recordings was 131 ms.

One H.264 inline control repeat delivered 21.45 distinct fps after warm-up. An intermediate build with a second WGL context also exhibited this slow state. Both final worker repeats exceeded 59.6 distinct fps. The trigger for the intermittent slow state remains unresolved.

### Windows WGL thread affinity on the RX 550

WINQ dispatches virtio GPU commands and creates their fences on its main thread. On Amd-host-b, the process could use eight Windows logical CPUs. The Radeon driver changed the calling thread's affinity to one CPU inside `wglCreateContext`. A standalone WGL program reproduced the change before making the context current, and the restriction persisted after deleting the context.

Android Perfetto and Windows context-switch events identified time lost before Windows created a matching fence. For H.264, the renderer was runnable for 31.94 ms of a 33.90 ms delay. For HEVC it was runnable for 43.89 ms of a 47.77 ms delay. Both measurements used context-switch tracing with stack sampling disabled.

Build the Windows x64 shared SDL library from SDL 2.32.10 (commit `5d249570393f7a37e037abf22cd6012a4cc56a71`). Apply the patch at the root of the SDL checkout. Install the rebuilt `SDL2.dll` alongside the prototype's WINQ executable while WINQ is stopped. [SDL patch](winq-sdl-wgl-affinity.patch)

```sh
git apply /path/to/Remora/guest/winq-sdl-wgl-affinity.patch
cmake -S . -B build-windows -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=/path/to/windows-x64-toolchain.cmake \
  -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST=OFF
cmake --build build-windows --target SDL2
```

With `WINQ_PRESERVE_WGL_AFFINITY=1` on a single processor group, SDL saves the calling thread's eligible CPUs around WGL initialization, window setup, and context creation. It restores the saved mask when the driver changes it. This also preserves a caller's deliberate affinity restriction. On the RX 550, a separate SDL test verified masks of eight, two, and one eligible CPUs. With the opt-in disabled, the driver still pinned the thread.

The release renderer and Android image were held constant for six 35-second 1920×1080, 60 fps, 12 Mb/s recordings. Each pair contains one recording per codec. Warm rates and gaps use changes in the visible source counter, starting five presentation seconds into each recording.

| Windows configuration | H.264 warm fps | H.264 longest warm gap | HEVC warm fps | HEVC longest warm gap |
|---|---:|---:|---:|---:|
| Original SDL, BelowNormal priority | 59.80 | 50 ms | 59.88 | 43 ms |
| SDL affinity preservation, BelowNormal priority | 47.98 | 116 ms | 52.85 | 175 ms |
| Same SDL and WINQ process, Normal priority and original CPU mask restored before the pair | 59.93 | 36 ms | 59.97 | 34 ms |

The best pair delivered 59.74 and 59.68 distinct fps across the full presentation spans. Their longest gaps including startup were 72 and 80 ms. Windows D3D11VA decoded all 4,097 frames in this pair without errors. There were zero repeated or reversed counters. Both color samples and every bottom row passed. A 60 fps stream has a 16.67 ms frame interval.

The capture task originally started WINQ at BelowNormal priority. For a scheduled test intended to represent a normal interactive launch, set the boot task's `New-ScheduledTaskSettingsSet` result's `Priority` property to `4` before registering it, then verify the running WINQ process reports `Normal`. The exploratory SDL build used the existing video flag to exercise the same affinity code. The patch above requires the dedicated opt-in because the BelowNormal runs regressed and WINQ's thread was observed pinned again after startup.

A second trace boundary was needed when switching codecs. With Normal priority and the SDL fix enabled from boot, the H.264 trace dispatched on all eight CPUs. The 99th percentile time the dispatcher was runnable before fence creation fell from 2.45 to 0.21 ms compared with the original SDL at the same priority. On closing H.264, `DESTROY_VIDEO_CODEC` changed the dispatcher mask from `0xff` to `0x02`. Every one of the 201,963 warm dispatcher switches in the following HEVC recording ran on CPU 1.

Apply the codec teardown patch to the Windows renderer after the asynchronous readback patch. The renderer takes a separate snapshot of the calling thread’s affinity before it stops the codec worker, releases pending jobs and destroys the codec. It restores that mask after cleanup. Both patches use the same opt-in. [Codec teardown patch](winq-amf-affinity.patch)

Check the active boot entry before collecting each nested benchmark. On the standard Windows timer entry, the corrected renderer retained all eight CPUs after four encoder sessions, but distinct warm delivery fell to 15.99–16.94 fps for H.264 and 22.22–32.13 fps for HEVC. In a 3.67-second H.264 gap, the Android source counter advanced by one. The Windows test entry applies to one boot. For the matched retest, the test disk was prepared while the passthrough VM was off, then cold-booted and checked for `useplatformtick Yes` and a working HPET.

On the verified platform-tick boot, the corrected build produced 2,021 hardware-decoded HEVC frames at 1920×1080. Distinct frame delivery averaged 59.52 fps after warm-up, with a 27.7 ms p99 and a 64 ms maximum gap. The full recording averaged 59.23 fps with a 120 ms maximum gap. Both warm gaps above 50 ms advanced the visible Android frame counter by one. The color sample and bottom row of every frame passed. H.264 produced no frames across three attempts. The renderer reported D3D11 device removal and a standalone Windows AMF H.264 control also timed out. The affinity correction remains opt-in, and a matched H.264 delivery rate is still outstanding.

After the test VM shut down cleanly, Linux reattached the RX 550 and started its display manager. The peer journal then recorded a second GPU teardown eight seconds later. Amd-host-b stopped answering LAN pings between 20:22:19 and 20:22:25 EST. Recovered host records confirmed the separate installer test initiated that teardown. Its runner created the handoff start flag at 20:22:08.838 and its service logged binding the RX 550 at 20:22:11. The encoder handback had released the shared GPU lock before its seven-minute host observation ended. The specific cause of LAN loss remains open. The physical host outage and codec delivery are tracked in remora-1978.6.1 and remora-1978.6.2.

### Keeping the Amd-host-b GPU lock through handback verification

The physical encoder and installer harnesses use the same host lock, `/run/remora-amd-host-b-vfio.lock`. The handback service now owns it while Linux restores the native drivers, while Yggdrasil checks seven minutes of pings and host heartbeats, and until the originating controller confirms its final checks. The confirmation names the current systemd invocation. The host then releases the lock. Both launchers pin the patched supervisor. [Host patch](../tools/amd-host-b-vfio/handback-health.patch) · [Controller patch](../tools/amd-host-b-vfio/controller-health.patch) · [Controller confirmation](../tools/amd-host-b-vfio/health-receipt.sh)

Before touching the GPU, the host verifies the peer monitor and creates a persistent directory at `/var/lib/remora-amd-host-b-vfio-health-pending`. A failed check, missing confirmation or killed owner leaves that directory in place. Patched launchers check it under the shared lock before admitting another handoff, including after a reboot. Recovery requires reviewing the outstanding handback before clearing that directory. The flock file itself must retain its inode.

The peer verifier reads existing telemetry. It requires continuous samples on the same host boot, wired carrier up and fresh replies throughout the observation. Local regression tests exercise a competing process during observation and controller confirmation, and admission after success or failure. Run `test-lease.sh` with the patched encoder or installer supervisor as its argument. The test uses Bubblewrap with isolated networking and a synthetic device filesystem. [Peer verifier](../tools/amd-host-b-vfio/check-receiver.py) · [Lease regression](../tools/amd-host-b-vfio/test-lease.sh) · [Telemetry tests](../tools/amd-host-b-vfio/test-receiver.py)

On November 20, 2026, Amd-host-b ran both codecs in one cold-booted Windows session with the GPU lock fix, the verified platform timer and WINQ at Normal priority. Android 17 requested 1920×1080 at 60 fps and 12 Mb/s for 35 seconds per codec. The RX 550 initialized AMD's HEVC encoder first and its AVC encoder second. D3D11VA decoded both recordings with zero errors. The sampled colors and the bottom row of every decoded frame passed.

| Codec | Decoded frames | Full distinct fps | Distinct fps after 5 s | Warm p99 gap | Longest warm gap |
|---|---:|---:|---:|---:|---:|
| H.265 | 1,976 | 58.64 | 59.34 | 28.0 ms | 61 ms |
| H.264 | 2,022 | 59.09 | 59.51 | 27.6 ms | 52 ms |

The visible Android counter advanced by one across each warm gap over 50 ms (two for H.265 and one for H.264). Neither recording contained a repeated or reversed counter. Both codecs still exceeded the 16.67 ms frame interval after warm-up.

Windows exited cleanly and Linux restored the native GPU and audio drivers and the display manager at 20:58:55 EST. At the seven-minute mark the original supervisor still held the same lock inode. It accepted the controller's confirmation at 21:06:07 EST and released the lock. The peer recorded 155 successful probes with zero losses, covering 424 seconds after native restoration. SSH confirmed the original host boot after release.

### Android mirror timestamps and benchmark interference

On Android 17 in the VirGL mirror-only configuration, SurfaceFlinger now timestamps
each refreshed `remora-mirror` frame at the start of its composition. This records the
time of the scene before the GPU finishes rendering it. The encoder still waits for
the rendered buffer's acquire fence. On Amd-host-b, visible frame IDs matched the
recorded pictures to SurfaceFlinger and the source app. With the original renderer
timestamps, 90% of H.264 and HEVC samples matched the buffer queue time within
0.45 ms. With the timestamp change, 90% matched the mirror composition start within
0.46 ms for both codecs. [Android patch](../vendor/source-build/container-patches/android-17.0.0/frameworks/native/0005-SurfaceFlinger-timestamp-Remora-mirror-frames-before-composition.patch)

The Windows benchmark used to launch PowerShell and query Task Scheduler during the
35-second recording. In a single H.264 trace, two such queries occupied Windows CPU 3
for 45.0 ms during a 76.4 ms mirror queue interval and 48.7 ms during a 60.9 ms
interval. Two Radeon OpenGL workers were pinned to that CPU. Windows kernel thread
lifetimes and the PowerShell event log identified both query processes. Among 154
frame intervals entirely inside those two threads' lifetimes, 11 exceeded 33.34 ms.
Among 1,455 intervals outside them, six exceeded 33.34 ms.

The capture wrapper now publishes a receipt for its run when it exits. The launcher
waits for the receipt before checking the task and validating the recording. A
reconnecting controller can wait for the same receipt. To update a staged Amd-host-b
encoder harness, apply the integration patch to its parent directory and copy
`capture-completion.ps1` into `encoder/` beside `install-test.sh`. Keep the usual
host GPU lease and post-handback observation active. [Harness patch](../tools/amd-host-b-vfio/capture-completion.patch) · [Windows receipt helper](../tools/amd-host-b-vfio/capture-completion.ps1)
