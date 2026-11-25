# Building the host side against FFmpeg 8.x

This page records one build of the host-side FFmpeg consumers against an installed FFmpeg 8.x. It
shows that this source tree compiles and links against that one FFmpeg release, and that the
offline suites pass on that build. It does not set a minimum version, it does not promise that
every distribution's FFmpeg 8 build will work, and it says nothing about runtime behaviour.

## What uses FFmpeg (`CMakeLists.txt`)

| Target | Kind | FFmpeg / libva inputs | When it is built |
|---|---|---|---|
| `remora_mirror` | static library, linked into the `remora` executable | `pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavformat libavutil libswscale libswresample)` | always. Configure fails without these modules |
| `vendor/native/remora-frame-encoder` | custom command (one `cc` call compiles and links), part of `remora_native` (ALL) | `pkg_check_modules(REMORA_ENCODER_DEPS QUIET libavcodec "libavfilter>=9.16.100" libavutil)` and `vulkan/vulkan.h` | only when both checks pass. Otherwise configure prints `NOT built` |
| `vendor/native/remora-frame-decoder` | custom command (one `cc` call compiles and links), part of `remora_native` (ALL) | `pkg_check_modules(REMORA_DECODER_DEPS QUIET libavcodec libavutil libva)` | only when the check passes. Otherwise configure prints `NOT built` |

`remora_mirror` is an archive, so it reaches the FFmpeg shared libraries when `remora` links. No
test executable links `remora_mirror`. The two native helpers are optional, so their absence is
a configure message, not a build failure. A build only counts as covering them if configure
reports `building` for both.

`vendor/host-prereqs/nvdec-vaapi/decode-test-client.c` also calls FFmpeg, but CMake does not
build it, and this record does not cover it.

## FFmpeg used by this build

All of these values were read on the build host. None were copied from a package list.

| Source | Value |
|---|---|
| `FFMPEG_VERSION` in `libavutil/ffversion.h`, the header the targets include (`-I/usr/include`) | `8.1.3` |
| libavcodec (pkg-config, as reported by configure and in `CMakeCache.txt`) | 62.28.103 |
| libavformat | 62.12.103 |
| libavutil | 60.26.103 |
| libswscale | 9.5.103 |
| libswresample | 6.3.103 |
| libavfilter (`REMORA_ENCODER_DEPS_libavfilter_VERSION`) | 11.14.103 |
| libva (`REMORA_DECODER_DEPS_libva_VERSION`; `va/va_version.h` says VA-API 1.24.0) | 1.24.0 |
| Compiler | GCC 16.2.1 (C and C++) |

How the build metadata ties these versions to the targets:

- `remora`'s link line in `build/build.ninja` names the `libavformat`, `libavcodec`, `libswscale`,
  `libswresample` and `libavutil` `.so` files from the same library directory that pkg-config
  reports.
- The custom commands in `build.ninja` pass the pkg-config flags directly:
  `-lavfilter -lavcodec -lavutil` for the encoder and `-lavcodec -lavutil -lva` for the decoder.
- The `DT_NEEDED` sonames in the built binaries match FFmpeg 8's library majors:

  | Binary | Shared libraries it needs |
  |---|---|
  | `remora` | `libavformat.so.62`, `libavcodec.so.62`, `libswscale.so.9`, `libswresample.so.6`, `libavutil.so.60` |
  | `remora-frame-encoder` | `libavfilter.so.11`, `libavcodec.so.62`, `libavutil.so.60` |
  | `remora-frame-decoder` | `libavcodec.so.62`, `libavutil.so.60`, `libva.so.2` |

## Commands and outcomes

Run from the repository root:

```bash
cmake --fresh -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --clean-first
cmake --build build
ctest --test-dir build
ctest --test-dir build --output-on-failure -R prereqs
```

- **Configure** succeeded. It found the five `FFMPEG` modules at the versions listed above and
  printed `remora-frame-encoder: building (host_encode)` and
  `remora-frame-decoder: building (host_decode)`.
- **Clean build** finished 99/99 steps with no errors. The steps that matter here:
  `Building vendor/native/remora-frame-decoder`, `Building vendor/native/remora-frame-encoder`,
  all nine `src/mirror/*.cpp` objects, `Linking CXX static library libremora_mirror.a` and
  `Linking CXX executable remora`. Both helpers exist afterwards as dynamically linked ELF
  executables.
- **Normal build** printed `ninja: no work to do.`
- **ctest** passed all 12 registered tests: play_install, public_images, public_audit,
  play_engine, builders, gating, parsers, mirrorproto, prereqs, engine, reddit_session and store.
  `-R prereqs` passed on its own as well.

## Errors and warnings

- **Removed symbols:** none. No compile or link error appeared in any target.
- **FFmpeg deprecation warnings:** none. The native helpers build with `-Wall -Wextra`, and
  `remora_mirror` builds with the project's default flags. No `-Wno-deprecated-declarations`
  or `-w` is set anywhere, and GCC turns on `-Wdeprecated-declarations` by default, so FFmpeg's
  `attribute_deprecated` markings would have been reported.
- **Unrelated warnings:** two `-Wunused-result` warnings in `tests/test_engine.cpp`, at lines 748
  and 749. They come from Qt's `[[nodiscard]]` on `QFile::open` in the test's fixture setup and
  do not involve FFmpeg.

## The CI jobs (`.github/workflows/ffmpeg.yml`)

`ci.yml` runs on Ubuntu 24.04, whose FFmpeg 6.1 skips the encoder. A separate read-only workflow
builds all three FFmpeg consumers twice, one job against FFmpeg 8 and one against FFmpeg 9:

- **Environment.** An Arch Linux image pinned by digest, on the same `ubuntu-24.04` hosted
  runner. Every enabled pacman repository is frozen to one dated
  `archive.archlinux.org` snapshot, and the job fails if any repository resolves elsewhere or does
  not require package signatures. `pacman -Syyuuw` downloads the packages with up to three
  attempts. `pacman -Suu` then installs from the same snapshot, including upgrades or downgrades
  for packages already in the image. The FFmpeg 8 job's snapshot carries FFmpeg
  8.1.2, the FFmpeg 9 job's FFmpeg 9.1.
- **Version guard (after configure).** For libavcodec, libavformat, libavutil, libswscale,
  libswresample and libavfilter, the job reads the pkg-config version that configure recorded in
  `CMakeCache.txt` and the `LIB*_VERSION_MAJOR` macro from the headers in the same include
  directory. Both must match the job's FFmpeg: majors 62, 62, 60, 9, 6 and 11 for FFmpeg 8, and
  63, 63, 61, 10, 7 and 12 for FFmpeg 9. `FFMPEG_VERSION` in `libavutil/ffversion.h` must be 8.x
  or 9.x to match, with or without a tag's leading `n`. The job also prints
  the libva version and whether `vulkan/vulkan.h` was found.
- **Target guard (after configure).** Configure only prints `NOT built` when a helper's
  dependencies are missing, so the job looks in `build.ninja` for the encoder's and the decoder's
  build statements. Both must exist, both must be part of `remora_native`, and their commands
  must link `-lavfilter` or `-lva` respectively. `remora` must link `libremora_mirror.a`. Neither
  helper binary may exist before the build.
- **Build.** A fresh configure, then `cmake --build build --clean-first`. Warnings and errors go
  to the log unfiltered. The job continues past no failure.
- **Link guard (after the build).** The job reads `DT_NEEDED` from `remora`, `remora-frame-encoder`
  and `remora-frame-decoder`. They must load the job's libavcodec and libavutil (`.so.62` and
  `.so.60` for FFmpeg 8, `.so.63` and `.so.61` for FFmpeg 9) and their own libraries:
  libavformat, libswscale and libswresample for `remora`, libavfilter for the encoder and
  `libva.so.2` for the decoder, each at the job's major.
- **Tests.** `ctest --output-on-failure --no-tests=error` runs every registered test with
  `QT_QPA_PLATFORM=offscreen` and a fresh `HOME`.

Each job expects its FFmpeg exactly. When one moves to another snapshot, change its matrix entry's
snapshot URL, image (if it is older than the new snapshot) and expected majors together. Neither
sets or changes the supported minimum, which is still the one in `CMakeLists.txt`.

## FFmpeg 9.1

FFmpeg 9 is a new major of every library, and the one Arch has shipped. One build,
in a throwaway Arch container (ffmpeg 2:9.1-1, Qt 6.12.0, GCC 16.2.1, CMake 4.5.1, libva 2.24.1),
from a clean copy of the tree: configure found both helpers' dependencies, the build finished,
and all 19 registered tests passed, `phone_qr` by skipping (the container had no JDK). The only
warnings were three `-Wunused-result` in `tests/test_engine.cpp`, Qt 6.12's `[[nodiscard]]` on
`QFile::open`, since fixed. Nothing FFmpeg marks deprecated was reported.

`decoderDecodesH264AndHevcInSoftware` in `tests/test_mirrorproto.cpp` decodes one H.264 and one
HEVC frame through the mirror's decoder in software and checks the pixels, so these runs, and
every CI job's, decode with the FFmpeg they built against. The Windows package's FFmpeg, BtbN's
build of the 9.0 branch, passed the same suites in a Windows 11 VM.

## Not verified

- Distribution packages (the Gentoo ebuild included) and containers. The CI jobs above are
  defined, but this page records no hosted run of them.
- Any FFmpeg 8.x release other than 8.1.3, any 9.x other than 9.1, and any other set of FFmpeg
  configure options.
- Live decoding past those two frames, encoding, audio playback or mirroring, the optional GPU
  paths (VA-API, Vulkan, CUDA interop) and runtime performance.
