# Mesa for Android, from source

This directory builds Mesa for Android x86_64 from source — the job that replaced the Mesa 24.0.8
prebuilts the image used to inherit (bd `remora-ykhz`).

`build-mesa.sh` produces the payload; the `mesa_source` build feature
(`vendor/source-build/features/mesa_source/`) installs it. **A source build of `remora_x86_64`
requires it**, on Android 16 and 17 alike: the upstream prebuilts project has left both manifests, so
this payload is the image's only GL stack and its only Vulkan ICDs besides lavapipe. Without it the
build would finish cleanly and produce an image that cannot composite, so it refuses instead —
`remora build --source` in about a second, and a `$(error)` in `device-remora/remora.mk` for a
manual `do-build.sh` run. The feature is nonetheless **default-off**, because making it a default
would commit every profile to a source build; the reasoning is on the `mesa_source` entry in
`src/core/Features.cpp`.

Run it on the host before a source build. The payload lands in
`vendor/source-build/features/mesa_source/lib64`, which is gitignored and never committed.

## The set it replaced, measured

Hashing every file under the prebuilts project's `prebuilts/x86_64` and looking each hash up in the
image that shipped them gave **seventeen** prebuilts actually in use — matching by *name*
under-reports, because AOSP builds several of these itself under the same names and the soong
output wins. Fifteen were Mesa; the other two were `libc++_shared` and the closed prebuilt
composer.

**They are not one thing, and this is the finding that reshapes the work:**

| cluster | files | rule |
|---|---|---|
| **Vulkan ICDs** | `vulkan.{intel,intel_hasvk,radeon,broadcom,freedreno,nouveau}` | each is a standalone driver the loader dlopens by name; **replaceable one at a time** |
| **GL/DRI** | `libgallium_dri`, `libgallium_drv_video`, `libEGL_mesa`, `libGLESv1_CM_mesa`, `libGLESv2_mesa`, `libgbm`, `libglapi` | **all or nothing** — see the ABI note below |
| *not Mesa* | `gralloc.cros`, `gralloc.gbm` | minigbm / gbm_gralloc; a separate slice |

So the correct first slice is **a single Vulkan driver**, not the GL cluster — it is independently
swappable, independently revertible, and ANV and RADV are already ABI-complete against the
prebuilts. `mesa.mk` enforces the distinction: it takes whichever ICDs the payload contains, and
*refuses* a GL cluster that is missing any member rather than shipping a mixed pair.

Neither `vulkan.virtio` nor `vulkan.lvp` was in that set: they matched no prebuilt hash. `virtio`
is Venus, from the prebuilt guest-library release `build-venus.sh` fetches (pinned by tag,
SHA256-verified), and is the driver actually in use on the NVIDIA host; `lvp` is built by AOSP and requested directly in `remora.mk`.

## Why this exists at all

The image's GPU stack used not to be built from source at all: every Mesa driver it shipped was
byte-identical to a binary in the upstream prebuilts project, stamped `Mesa 24.0.8 (git-441f064c1c)`.
`external/mesa3d` is in the manifest (pinned to `android-17.0.0_r1`, `VERSION` 26.1.0-devel) but
its `Android.bp` builds only gfxstream infrastructure — `vulkan_runtime`, `vulkan_util`, `vulkan_wsi`, gallium
auxiliary/drivers/winsys, compiler, util. It defines **no** `libEGL_mesa`, **no** `vulkan.<driver>`,
**no** `libgallium_dri`, **no** `libgbm`, **no** `libglapi`.

So bumping the manifest pin cannot move the drivers: the pin does not build them. Mesa reaches
Android through its *own* meson with an NDK cross-file, which is how those prebuilts were produced
in the first place.

## What is proven

Mesa **26.1.0-devel** cross-builds for Android x86_64, against the prebuilts' Mesa 24.0.8. Eight
artifacts, every one `ELF 64-bit, x86-64, for Android 34, built by NDK r27c`:

| built | size | replaces |
|---|---|---|
| `libvulkan_intel.so` | 25M | `hw/libvulkan_intel.so` |
| `libvulkan_radeon.so` | 18M | `hw/libvulkan_radeon.so` |
| `libgallium_dri.so` | 30M | `dri/libgallium_dri.so` — **iris only, see below** |
| `libEGL.so` | 475K | `egl/libEGL_mesa.so` |
| `libGLESv2.so` | 80K | `egl/libGLESv2_mesa.so` |
| `libGLESv1_CM.so` | 33K | `egl/libGLESv1_CM_mesa.so` |
| `dri_gbm.so` | 213K | `libgbm.so.1` |

**radeonsi is now in** (`amdgpu_winsys_create` defined, matching the prebuilt), which needed LLVM
20.1.8 cross-compiled for Android — see below.

**Both Vulkan drivers are ABI-complete**: zero symbols the prebuilt exported are missing from the
source build. Reproduce with `android-x86_64.cross.in` (substitute `@NDK@`, `@PCDIR@`):

```
# once, natively — ANV needs CLC, and Gentoo slots the SPIRV translator off the default path
PKG_CONFIG_PATH=/usr/lib/llvm/22/lib64/pkgconfig meson setup <native> \
  -Dgallium-drivers= -Dvulkan-drivers= -Dplatforms= \
  -Dinstall-mesa-clc=true -Dinstall-precomp-compiler=true \
  -Dmesa-clc=enabled -Dprecomp-compiler=enabled -Dprefix=<clc-install>
ninja -C <native> install

# then cross, with the native tools on PATH
PATH=<clc-install>/bin:$PATH meson setup <build> --cross-file android-x86_64.cross \
  -Dplatforms=android -Dplatform-sdk-version=34 -Dandroid-stub=true \
  -Dgallium-drivers=iris -Dvulkan-drivers=intel,amd \
  -Dmesa-clc=system -Dprecomp-compiler=system \
  -Dgbm=enabled -Degl=enabled -Dllvm=disabled -Dbuildtype=release
```

RADV needs no LLVM (it compiles shaders with ACO); ANV needs CLC, which the native `mesa-clc`
supplies.

### Two traps already paid for

- **`sys_root` in the cross-file corrupts every dependency include path.** meson prefixes it onto
  `-I` flags coming from pkg-config, so an absolute tree path becomes
  `<ndk-sysroot>/<absolute-tree-path>/...` and the build dies on a missing `xf86drm.h`. The NDK's clang driver
  already knows its own sysroot; do not set the property.
- **Target libdrm must come from the AOSP tree**, via the generated `.pc` files and
  `pkg_config_libdir`. Leave it to the host's `/usr/lib64/pkgconfig` and meson hands a Linux x86_64
  libdrm to an Android cross build without complaining.

Note the built driver links `libdrm.so` and `libdrm_amdgpu.so` **unversioned** — exactly what the
libdrm slice installs. The versioned symlinks (`libdrm_amdgpu.so.1`) existed only for the 24.0.8
prebuilts.

## What the replacement had to get right

The prebuilt set was fifteen Mesa files, and a file count was never the measure. In rough order of
difficulty:

1. **THE DRI LOADER ABI CHANGED between 24.0.8 and 26.x, and this is the real constraint.**
   *(Enforced rather than merely documented: `mesa.mk` `$(error)`s on a partial GL payload, and
   a test walks every proper subset to prove it.)* The
   prebuilt exports a `__driDriverGetExtensions_<driver>` entry point per driver; Mesa 26 exports
   none of them. That shows up as 49 "missing" symbols, and the list is misleading at a glance
   because it names `iris`, `radeonsi` and `zink` — all of which ARE built here. They are absent
   because the interface is gone, not the driver.

   The consequence is concrete: **gallium cannot be swapped on its own.** `libgallium_dri`,
   `libEGL`, the GLES pair and `libgbm` have to move together, because the loader half and the
   driver half must agree on which interface they speak.
2. **Size.** Stripped, `libgallium_dri.so` is 90 MB against the prebuilt's 34 MB, and
   `libvulkan_radeon.so` is 61 MB against 11 MB, because both statically link LLVM 20. The prebuilt
   was evidently built against a much more tightly trimmed LLVM. ~120 MB of image growth, and still
   open.
3. **nouveau gallium** is not in the default `-Dgallium-drivers=iris,radeonsi`; NVK, the nouveau
   Vulkan driver, is available on request (below).
4. **`libglapi` has no counterpart**, because Mesa removed shared-glapi upstream — and nothing
   needs it: Mesa 26's `libEGL_mesa` does not link it, and a sweep of every vendor library finds no
   reference. **`libgallium_drv_video` is not a separate file any more**: Mesa 26 serves VA from
   `libgallium_dri.so` itself, and `build-mesa.sh` installs a `<driver>_drv_video.so` alias beside it
   for each VA-capable driver (it configures `-Dgallium-va` against libva's headers only).
5. **`gralloc.cros` / `gralloc.gbm`** are not Mesa at all; they come from minigbm.
6. ~~**32-bit.**~~ **No longer needed** — the image went 64-bit-only (bd `remora-ykhz`), so
   x86_64 is the whole job. Measured first: the only 32-bit processes were the two zygotes
   themselves, zero 32-bit apps against 21 64-bit.

## NVK (vulkan.nouveau) needs Rust, and three traps were paid for

NVK's compiler (NAK) is Rust, so `REMORA_MESA_VULKAN=nouveau` makes the cross build need rustc,
the Rust std for `x86_64-linux-android`, and the host std (NAK's proc-macro crates compile for the
build machine and are loaded into rustc itself). `build-mesa.sh` assembles all of it automatically;
what follows is why it does it the way it does.

- **A distro rustc cannot drive this build (E0514).** rustc refuses any rlib whose producer
  version *string* differs from its own, and Gentoo's identifies as `1.96.1 (31fca3adb …) (gentoo)`
  while the official std tarballs say `1.96.1 (31fca3adb …)` — same commit, still "incompatible".
  A sysroot overlay of official std under the distro rustc dies at meson's sanity check. So the
  script fetches the whole self-consistent set — `rustc`, host `rust-std`, android `rust-std` —
  from static.rust-lang.org into a private prefix under `$WORK`, fronts it on `PATH` for the build,
  and touches nothing system-wide.
- **bindgen dlopens libclang at runtime**, and Gentoo slots it under `/usr/lib/llvm/<ver>/lib64`,
  off the default loader path. The failure is a mid-build panic ~235 targets in that ninja reports
  only as `exit 101`. `LIBCLANG_PATH` is set by the script (same slotting trap as the SPIRV
  translator in stage 1). `BINDGEN_EXTRA_CLANG_ARGS` aims libclang at the NDK sysroot, or the
  Android headers are parsed as host code.
- **`meson setup --wipe` does not give you the configure you asked for.** After three runs with
  different driver sets the build dir held a *union* configure no invocation had requested, and
  ninja raced meson's regeneration — compiles failing on output dirs that did not exist and on
  generated headers whose producers had not run. Stage 4 now does `rm -rf $BUILD` and configures
  from nothing, every time. The driver set is the script's whole input; the configure must be
  exactly it.

The payoff is not like-for-like: the prebuilt `vulkan.nouveau.so` **has never exported `HMI`** —
`verify-image.sh` carries it as a known-bad prebuilt — so the Android loader has never once been
able to use it. The source-built NVK exports HMI and is the first loadable nouveau ICD this image
family has shipped. (`verify-image.sh` now scopes that exemption to the prebuilt generation: a
Mesa-26 nouveau without HMI FAILS.)

## Building LLVM for Android

LLVM **20.1.8**, AMDGPU target only, static, installed to a prefix. Two pieces are non-obvious:

- **A native tblgen of the SAME version is required first** — the host's LLVM 22 tblgen cannot
  drive an LLVM 20 build. Build `llvm-tblgen` natively, pass it as `-DLLVM_TABLEGEN=`.
- **`llvm-config` cannot be cross-built usefully**, because Mesa has to execute it. See
  `llvm-config-android.in`: a NATIVE llvm-config copied into the cross install tree reports that
  tree's paths (it derives its prefix from its own location) while carrying LLVM's real component
  database, which is what makes `--libs amdgpu` expand to 58 libraries in the right order.

```
cmake -S llvm -B <cross> -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=x86_64 -DANDROID_PLATFORM=android-34 \
  -DLLVM_TARGETS_TO_BUILD=AMDGPU -DLLVM_TABLEGEN=<native>/bin/llvm-tblgen \
  -DLLVM_HOST_TRIPLE=x86_64-linux-android -DLLVM_ENABLE_PIC=ON \
  -DLLVM_ENABLE_ZLIB=OFF -DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_TERMINFO=OFF \
  -DLLVM_ENABLE_LIBXML2=OFF -DCMAKE_BUILD_TYPE=Release
```

RTTI is off in that build, so Mesa must be configured `-Dcpp_rtti=false` or it errors out on the
mismatch.

### libelf, and two traps inside it

radeonsi requires libelf. AOSP's `external/elfutils` builds one and it is `vendor_available`, but:

- **The default Android variants are LTO bitcode**, produced by AOSP's LLVM 22. The NDK r27c linker
  is LLVM 18 and rejects them outright: `Invalid attribute group entry (Producer: 'LLVM22.0.1'
  Reader: 'LLVM 18.0.3')`. Use an `..._lto-none_...` variant, which contains real ELF objects.
- **libelf pulls zstd**, because elfutils reads `ELFCOMPRESS_ZSTD` sections. soong links that
  dependency implicitly; standing the archive up by hand means naming `libzstd.a` yourself, or the
  link dies on `ZSTD_decompress` / `ZSTD_isError`.

## Validate on the hardware that uses it

On an NVIDIA/Venus host the fifteen Mesa files read **zero** processes: the only live Vulkan
driver there is `vulkan.virtio.so`, from the prebuilt guest-library release `build-venus.sh`
fetches. On an AMD host the same set is the whole GPU stack. So the NVIDIA machine that builds this payload cannot exercise it, and
the check has to happen on an AMD (or Intel) test host — where a wrong answer is a black screen.

That is how it was proven: an AMD test host (an RX 580) adopted a Mesa-26 image with radeonsi GL
composing, RADV naming the card, and hardware VAAPI encode running through the source-built stack.
Build on one machine, validate on the other, and keep a rollback image tag when adopting a new
payload.
