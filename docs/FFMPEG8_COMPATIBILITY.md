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

## Not verified

- CI, distribution packages (the Gentoo ebuild included) and containers.
- Any FFmpeg 8.x release other than 8.1.3, and any other set of FFmpeg configure options.
- Live decoding, encoding, audio playback or mirroring, the optional GPU paths (VA-API, Vulkan,
  CUDA interop) and runtime performance. A clean offline build does not exercise any of them.
