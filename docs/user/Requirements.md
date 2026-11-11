# Requirements

This page lists what you need for three separate jobs: running Android, building the Android
image, and building Remora itself. The first is modest. The second is not: plan the disk, memory
and time before you start.

## To run Android

| you need | notes |
|---|---|
| Linux on x86_64 | Android runs in a container on the host's own kernel |
| Docker | and membership of the `docker` group on the machine that runs the containers |
| A kernel that provides binder (binderfs) | most desktop kernels do. `remora check` does not probe this |
| `adb` | Remora reaches every device it deploys through adb. Install your distribution's android-tools package |
| An Android image you built | Remora ships none. Each image takes about 3 GB in Docker |
| Disk for Android's data | each instance's data grows with the apps you install |

Two things depend on the Android release:

- **Android 17** needs no kernel module. Shared memory runs on memfd.
- **Android 16** images also need the `ashmem_linux` kernel module, in a build that loads on an
  IBT-enabled kernel. Remora does not ship that module. `remora check` reports whether it is loaded
  when the profile needs it.

`remora check` probes all of this read-only and names what is missing, with the fix for each row.
It installs nothing. See [Deploy Targets](Deploy-Targets.md#checking-readiness).

### A GPU is optional

| host GPU | how Android uses it |
|---|---|
| Intel or AMD | the render node is used directly |
| NVIDIA (proprietary driver) | through the Venus render server, which `remora venus-build` builds and installs |
| none | Android renders in software |

Details are in [GPU Acceleration](GPU-Acceleration.md).

### A remote target

A `remote` target needs the same things on the docker host (Docker, binder, the GPU pieces), plus
key-based ssh from your machine to it. Your machine still needs `adb`, because the client connects
from there.

### Optional tools

The Arch package lists these as optional dependencies, which is a fair summary for any
distribution:

| tool | used for |
|---|---|
| docker | running Android on this machine (the `bare` target) |
| openssh | deploying to a docker host over ssh (the `remote` target) |
| NVIDIA userspace utilities | NVIDIA acceleration through the Venus render server |
| python | sharing host input devices with the container (`shared_inputs`) |
| unzip | showing the boot animation in the mirror while Android starts |
| polkit | opening a device's storage from the workspace |

## To build the Android image

You build an image once per Android release and feature set. The figures below are the ones the
project measured, on one machine.

| | |
|---|---|
| **Disk** | About **450 GB** free, on an SSD if you can. One release's source tree and build output measured 470 GB after many rebuilds: a 99 GB source mirror, a checkout of about 100 GB and 269 GB of build output. Add about 20 GB for the Mesa toolchain built on the host and about 3 GB per finished image. |
| **Memory** | **32 GB** of RAM at minimum, **64 GB** recommended, plus swap. The build's executor alone peaks around 21 GB, and every Java or metalava step running beside it takes about 4 GB more. |
| **Time** | The first build downloads about **100 GB** of source, then compiles for **3½ to 13 hours** on a 24-thread i9-12900K, depending on how many jobs you give it. Later rebuilds take under an hour (47 minutes in the project's measurement, 29 of them compiling). |

Two inputs come from you, not from the download:

- **The Mesa graphics stack**, built on the host once by
  `vendor/host-prereqs/mesa-android/build-mesa.sh`.
- **Third-party payloads** behind features such as Google apps, ARM translation and Widevine, which
  Remora does not redistribute. [vendor/PAYLOADS.md](../../vendor/PAYLOADS.md) says what each one
  is and where it comes from.

A build that is missing either one stops and says so before it starts compiling. The build also
needs two host settings (`vm.max_map_count` and the systemd-oomd limits). All of this is covered in
[Building the Image](Building-the-Image.md).

## To build Remora itself

You only need this when you install from source or from the Arch `PKGBUILD`. The `.deb` is already
built.

| requirement | notes |
|---|---|
| A C++20 compiler | |
| CMake 3.20 or newer | |
| Qt 6 | the `Core`, `Widgets`, `Test`, `Network`, `DBus`, `OpenGLWidgets`, `Multimedia` and `WebEngineWidgets` modules |
| FFmpeg development libraries | `libavcodec`, `libavformat`, `libavutil`, `libswscale`, `libswresample` |
| FFmpeg's NVIDIA codec headers (`ffnvcodec`) | headers only. The mirror loads the driver at runtime, so you do not need an NVIDIA GPU to build |
| Your C library's static archive (`libc.a`) | two of the small native helpers in `vendor/native` are static. On most distributions it is part of glibc's development files |

Two more helpers are optional and are built when their libraries are found. CMake says which it
built.

| helper | needs | what you lose without it |
|---|---|---|
| host frame encoder (`host_encode`) | FFmpeg 7 or newer (`libavfilter`) and the Vulkan headers | the mirror encodes inside Android instead of on the host GPU |
| host frame decoder (`host_decode`) | `libva` | in-Android video is not decoded on the host |

### Gentoo

```sh
sudo emerge -av dev-qt/qtbase:6 dev-qt/qtmultimedia:6 dev-qt/qtwebengine:6 \
    dev-build/cmake media-video/ffmpeg media-libs/nv-codec-headers dev-util/android-tools
```

### Arch

```sh
sudo pacman -S qt6-base qt6-multimedia qt6-webengine cmake ffmpeg ffnvcodec-headers android-tools
```

### Debian and Ubuntu

```sh
sudo apt install build-essential cmake qt6-base-dev qt6-multimedia-dev qt6-webengine-dev \
    libgl1-mesa-dev libavcodec-dev libavformat-dev libavutil-dev libswscale-dev \
    libswresample-dev libffmpeg-nvenc-dev adb
```

Then follow [Installation](Installation.md#from-source).
