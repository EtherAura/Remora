# Installation

Remora releases include packages for Linux, FreeBSD and Windows, the phone app, and a source
tarball.

## What a release contains

Every [release](https://github.com/EtherAura/Remora/releases/latest) carries:

- a source tarball,
- a `.deb` for Ubuntu 24.04 and later, and Debian (amd64),
- a `PKGBUILD` for Arch Linux,
- an ebuild for Gentoo, `remora-<version>.ebuild`,
- a native package for FreeBSD 15 amd64, `Remora-<version>-freebsd15-amd64.pkg`,
- an installer for 64-bit Windows 10 and 11, `Remora-<version>-windows-x64-setup.exe`, a preview
  ([Windows](Windows.md#installing)),
- the phone app for Android 10 and later, `Remora-<version>-android-phone.apk`, a preview
  ([The phone app](#the-phone-app)),
- SHA-256 checksums, an SBOM, and a Sigstore build-provenance attestation.

Remora uses calendar versions, `YYYY.MM.PATCH`. `remora --version` prints the version you have.

A Remora release never contains an Android image. The ready-made image is published separately
and fetched by Remora itself once it is installed; see [The Android image](#the-android-image).

## Ubuntu 24.04+ and Debian

```sh
ver=$(curl -fsSL https://api.github.com/repos/EtherAura/Remora/releases/latest \
      | sed -n 's/.*"tag_name": *"v\([^"]*\)".*/\1/p')
curl -fLO "https://github.com/EtherAura/Remora/releases/download/v$ver/remora_${ver}_amd64.deb"
gh attestation verify "remora_${ver}_amd64.deb" --repo EtherAura/Remora   # optional, needs gh
sudo apt install "./remora_${ver}_amd64.deb"
```

The `.deb` is built against Ubuntu 24.04's FFmpeg 6.1, which is too old for the host frame
encoder. `host_encode` is therefore not available from the package, and mirroring encodes on the
device instead. If you want the host encoder, build from source on a system with FFmpeg 7 or newer
and the Vulkan headers.

## Arch

```sh
mkdir remora && cd remora
curl -fLO https://github.com/EtherAura/Remora/releases/latest/download/PKGBUILD
makepkg -si
```

Take the `PKGBUILD` from the release, not from the repository. The copy in the repository leaves
the checksum unset, because the tarball it names does not exist until a release is tagged. Each
release carries a copy with the real checksum filled in.

A `PKGBUILD` is a recipe that your machine runs, and the provenance attestation does not cover what
it does. Read it before you run `makepkg`.

## Gentoo

The ebuild goes in a local overlay. This uses `app-eselect/eselect-repository` to create one:

```sh
ver=$(curl -fsSL https://api.github.com/repos/EtherAura/Remora/releases/latest \
      | sed -n 's/.*"tag_name": *"v\([^"]*\)".*/\1/p')
sudo eselect repository create local   # once, if you have no local overlay yet
sudo mkdir -p /var/db/repos/local/app-containers/remora
cd /var/db/repos/local/app-containers/remora
sudo curl -fLO "https://github.com/EtherAura/Remora/releases/download/v$ver/remora-$ver.ebuild"
sudo ebuild "remora-$ver.ebuild" manifest
echo 'app-containers/remora ~amd64' | sudo tee /etc/portage/package.accept_keywords/remora
sudo emerge -av app-containers/remora
```

`ebuild ... manifest` downloads the release's source tarball into your distfiles directory and
records its checksum in the overlay's `Manifest`. It trusts what it downloaded. To check the tarball
came from the release workflow, run
`gh attestation verify /var/cache/distfiles/Remora-$ver.tar.gz --repo EtherAura/Remora` before you
emerge.

The ebuild is keyworded `~amd64`, which is why the `package.accept_keywords` line is there. If
`emerge` asks for USE flag changes on the Qt packages, add them to `/etc/portage/package.use`.
After install, `emerge` lists the optional packages (Docker, OpenSSH, the NVIDIA driver and others)
and what each one enables.

Like a `PKGBUILD`, an ebuild is a recipe your machine runs. Read it before you emerge it.

## FreeBSD 15 amd64

Download the `.pkg` and matching `.sha256` from the
[release page](https://github.com/EtherAura/Remora/releases/latest). With root privileges, install
the local package using `pkg install` so pkg can resolve dependencies from the configured
FreeBSD repositories. The package includes the GUI and CLI and pulls in `android-tools` for ADB.
OpenSSH is part of the FreeBSD base system.

```sh
pkg install ./Remora-<version>-freebsd15-amd64.pkg
```

Run the desktop application as your regular user. In the workspace, set the profile target to
**Remote** and enter the SSH host. The host needs Linux, Docker and binderfs. A Linux guest
running under bhyve can also serve as this host when it provides those features.

```sh
remora check --backend remote --ssh-host user@linux-host
remora images fetch --tag remora24:x86_64-public --host user@linux-host
remora gui
```

Image builds and local Android deployment run on Linux. The FreeBSD package carries the two
static Linux helpers deployed with the Android container. See [Deploy Targets](Deploy-Targets.md)
for host prerequisites and ADB networking.

## The phone app

The phone app opens this computer's apps, and the Android apps of your devices, on an Android phone
(Android 10 or later), each on a screen of the phone's own size. It is a preview. Download
`Remora-<version>-android-phone.apk` from the release and open it on the phone, or install it from
a computer with `adb install`. Then pair it: `remora link pair`, or *New code* on the workspace's
Phone page, shows the code the app scans ([CLI Reference](CLI-Reference.md)).

Every release's APK is signed with the same key, so a newer release installs over an older one. The
key's certificate has this SHA-256 fingerprint, which `phone/release-cert.sha256` in the source
also holds:

```
d97e8019ebe8d9c1789b7682113e28d2e67f1dde1ce64ceaa17268aebcc6f9e2
```

`apksigner verify --print-certs Remora-<version>-android-phone.apk`, from the Android SDK's
build-tools, prints it as the signer's SHA-256 digest. A copy you built yourself with
`phone/build.sh` is signed with your own debug key instead, and Android does not update one with
the other: uninstall it first, then pair the phone again.

## From source

On FreeBSD, install the build dependencies below and configure with
`-DCMAKE_INSTALL_PREFIX=/usr/local -DREMORA_INSTALLED_VENDOR=ON`. Build
`vendor/native/fwd` and `vendor/native/uinput-kbd` on Linux amd64 with `make -C vendor/native`
and place them in the FreeBSD checkout's `vendor/native` directory before installing or creating a
package with `python3 tools/freebsd-package.py build`. Start from a clean public source checkout.

```sh
pkg install cmake-core ninja pkgconf qt6-base qt6-multimedia qt6-webengine \
    qt6-base_sqldriver-sqlite qt6-svg ffmpeg ffnvcodec-headers gmake python3 android-tools
```

For Linux, install the build dependencies for your distribution first: see
[Requirements](Requirements.md#to-build-remora-itself). Then:

```sh
git clone https://github.com/EtherAura/Remora.git
cd Remora
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DREMORA_INSTALLED_VENDOR=ON
cmake --build build
sudo cmake --install build
```

`REMORA_INSTALLED_VENDOR=ON` points the installed binary at the installed copy of the `vendor`
tree (the scripts and native helpers Remora deploys), not at the source tree it was built from.

The configure step reports which optional helpers it builds and why it skipped any. That is where
you find out whether you got the host frame encoder and decoder.

### Running from the build tree

For development, or to try Remora without installing it:

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/remora gui
```

This produces a single `build/remora` binary (the command line plus the `gui` subcommand), the
native helpers it deploys, and the test executables. The tests run offline, with no hardware:

```sh
ctest --test-dir build
```

## Verifying a download

The source tarball, `.deb`, FreeBSD package, SBOM, Windows installer and phone app each carry a
Sigstore-backed build-provenance attestation that records which workflow, at which commit, produced those exact
bytes. Check it with the GitHub CLI:

```sh
gh attestation verify remora_<version>_amd64.deb --repo EtherAura/Remora
```

A pass means the file came from the project's release workflow and has not been altered since.

| check | what it proves |
|---|---|
| `gh attestation verify` | the file was produced by the release workflow and is unchanged |
| the `.sha256` file beside each asset | only that the file matches a hash published on the same page |
| the SBOM | what went into the release |

Provenance is not code signing, and it covers only what the workflow built. Android images are not
assets of a Remora release: `remora images fetch` checks the ready-made image against its signed
catalog instead ([Official images](CLI-Reference.md#official-images)).
[SECURITY.md](../../SECURITY.md) has the full statement.

## After installing

For a local Linux Docker target:

```sh
sudo usermod -aG docker "$USER"   # then log out and back in
remora check                      # what this machine still needs, with the fix for each
remora gui
```

Membership of the `docker` group is required for the `bare` target, and it only takes effect after
you log in again. Be aware that it is root-equivalent on that machine: see
[Security Model](Security-Model.md).

`remora check` prints one row per prerequisite, each marked ok or MISSING with a remedy, and ends
with a verdict. Fix what it names and run it again until it reports that the target is ready.
[Deploy Targets](Deploy-Targets.md#checking-readiness) explains every row.

The install also adds a **Remora** entry to your application menu. It runs `remora gui`.

## The Android image

Remora needs an Android image to deploy. Getting one is separate from installing Remora: it does
not replace Remora's own dependencies, the `docker` group or what `remora check` asks for (see
[Requirements](Requirements.md#to-run-android)).

### The ready-made image

With Remora installed, fetch the audited Android 17 image the project publishes:

```sh
remora images fetch --tag remora24:x86_64-public
```

The download is 716 MB and the image takes 2.4 GB in Docker
([Requirements](Requirements.md#to-run-android) has the memory and disk figures). The signature,
validity window, archive hash, Docker identity and tag are checked before Docker
loads it, and an image it replaces is kept for rollback. Add `--host user@host` to fetch onto a
remote Docker host. In the workspace, **Official downloads** adds the same source to the Image
library. A fetched image is one row there, with the source named on it, and Fetch on that row
loads the source's current copy. [Official images](CLI-Reference.md#official-images) covers `list`, `check` and `rollback`.

The image is x86_64 only, and carries no Google apps, ARM translation, Widevine or root, so apps
that ship only ARM native code refuse to install on it. Google Play is optional:
`remora play-install` installs it locally, on your own Docker host, as a separate image over this
one (see [Optional Google Play](CLI-Reference.md#optional-google-play)). Root, ARM translation and
Widevine can be added the same way, as a layer built on your own machine without a source build
(see [Add-ons](Image-Features.md#add-ons-root-arm-translation-and-widevine)). ARM translation and
Widevine need payloads you supply yourself.

### Build your own image

To build an Android image from source instead, for example with features the ready-made image
leaves out, see [Building the Image](Building-the-Image.md) and its
[requirements](Requirements.md#to-build-the-android-image); they apply to the image build only,
not to installing Remora or to the ready-made image. [Image Features](Image-Features.md) lists
what a build can include. This is not the same as [building Remora from source](#from-source),
which builds the application only.

Continue with [Getting Started](Getting-Started.md).
