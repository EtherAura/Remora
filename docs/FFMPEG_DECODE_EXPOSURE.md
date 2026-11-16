# FFmpeg build-dependency inventory

Partial record for an unverified advisory (reported as CVE-2026-125252, a VA-API decode leak in
`libavcodec/vaapi_decode.c`). This page lists only what the repository **declares** about the
FFmpeg and libva it builds against. It is not a vulnerability assessment: it does not say whether
remora is exposed or safe, and it describes no runtime code path.

## 1. CMake configure constraints (`CMakeLists.txt`)

There are no other CMake modules in the tree; `CMakeLists.txt` is the only CMake file that names
FFmpeg, libva or `pkg_check_modules`.

| Consumer | Declaration | Required? | Version operator |
|---|---|---|---|
| `remora_mirror` (linked into `remora`) | `pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavformat libavutil libswscale libswresample)` | yes: configure fails without it | none on any module |
| `remora-frame-encoder` (host_encode) | `pkg_check_modules(REMORA_ENCODER_DEPS QUIET libavcodec "libavfilter>=9.16.100" libavutil)` plus `check_include_file(vulkan/vulkan.h REMORA_HAVE_VULKAN_H)` | no: skipped with a status message | `libavfilter>=9.16.100` only |
| `remora-frame-decoder` (host_decode) | `pkg_check_modules(REMORA_DECODER_DEPS QUIET libavcodec libavutil libva)` | no: skipped with a status message | none on any module |

The only enforced FFmpeg version constraint is `libavfilter>=9.16.100`, and it gates only the
optional encoder. No constraint exists on `libavcodec` for any target, so the configure step sets
no floor on the library that contains `vaapi_decode.c`.

Not constraints, though they mention versions:

- The comment above the encoder check (`FFmpeg 6.1 (Ubuntu 24.04) predates it and fails to
  compile`) explains the `libavfilter` floor; it is not itself checked.
- `message(STATUS "remora-frame-encoder: NOT built ... libavfilter >= 9.16.100 (FFmpeg 7 or newer) ...")`
  and the matching decoder message are configure output only.

These are configure checks, not a statement that every version passing them compiles or works.

## 2. CI and release build inputs (`.github/workflows/`)

Every job below runs on `runs-on: ubuntu-24.04` and installs, with
`apt-get install -y --no-install-recommends`, the same FFmpeg/libva names:
`libffmpeg-nvenc-dev`, `libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libswresample-dev`,
`libva-dev`. None carries a version (`=`, `>=`) or a pin.

| File | Job / leg | Notes |
|---|---|---|
| `ci.yml` | `build-and-test` | Release build, all suites |
| `ci.yml` | `sanitizers` | Debug build with ASan + UBSan |
| `codeql.yml` | `analyze`, matrix leg `language: c-cpp` only | the other legs (`java-kotlin`, `python`, `actions`) install nothing |
| `coverage.yml` | coverage job | also installs `lcov` |
| `release.yml` | release job | builds the dev tree and the packaged tree, then `cpack -G DEB` |

The comments in `ci.yml` (`this runner has 6.1`) and `release.yml` (`Ubuntu 24.04 ships 6.1`) are
expectations written by hand. They are not checked by any step and do not record the FFmpeg
version a given run actually resolved. `release.yml` does check the configure log for
`remora-frame-decoder: building`, which shows that the decoder check passed, not which version.

## 3. Distribution packages

| Package | Build-time | Runtime | Version operator |
|---|---|---|---|
| `.deb` (CPack, `CMakeLists.txt`) | the apt list in `release.yml` | **computed**: `set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)` makes `dpkg-shlibdeps` derive `Depends` from the installed ELF files; nothing FFmpeg-related is hand-written | whatever `dpkg-shlibdeps` emits for the build host; not in the repository |
| Arch (`packaging/PKGBUILD`) | `makedepends`: `cmake`, `ninja`, `ffnvcodec-headers`, `vulkan-headers` (FFmpeg comes in through `depends`) | `depends`: `'ffmpeg'`, `'libva'` | none |
| Gentoo (`packaging/remora.ebuild.in`) | `DEPEND`: `${COMMON_DEPEND}` + `dev-util/vulkan-headers`, `media-libs/nv-codec-headers`; `BDEPEND`: `virtual/pkgconfig` | `RDEPEND`: `${COMMON_DEPEND}`, where `COMMON_DEPEND` holds `media-libs/libva:=` and `media-video/ffmpeg:=` | none; `:=` is a slot-rebuild operator, not a version bound |

An unpinned package name means "whatever the distribution provides at build time". It does not
establish which FFmpeg version a CI run, a release `.deb` or a user's package was built with.

## 4. What this repository cannot establish

- Whether the advisory is real, and which FFmpeg release or commit fixes it.
- The FFmpeg and libva versions any remote CI run, released `.deb`, Arch or Gentoo build actually
  resolved.
- Whether the alleged leak is reachable at runtime through remora, and so whether remora is
  exposed. Nothing on this page bears on that question.

A local `build/CMakeCache.txt`, where one exists, describes only that one configuration.

## 5. Checking an actual build environment

Run these inside the environment in question: a CI runner, a release build host, or a package
build chroot. They are read-only. None of them has been run against any remote build for this page.

```sh
pkg-config --modversion libavcodec libavfilter libavutil libva   # what configure would see
grep -E '^(FFMPEG|REMORA_(EN|DE)CODER_DEPS)_[A-Za-z_]*VERSION' build/CMakeCache.txt
dpkg-query -W libavcodec-dev libavcodec60 libva-dev               # Debian/Ubuntu build host
dpkg-deb -f remora_*.deb Depends                                  # computed .deb runtime deps
pacman -Q ffmpeg libva                                            # Arch
portageq best_version / media-video/ffmpeg                        # Gentoo
```

Package names such as `libavcodec60` change with the FFmpeg soname, so adjust them to the
distribution.
