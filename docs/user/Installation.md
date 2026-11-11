# Installation

This page covers the three ways to install Remora (the `.deb`, the Arch `PKGBUILD`, and a build
from source), how to verify what you downloaded, and the two steps that follow every install: the
`docker` group and the first `remora check`.

## What a release contains

Every [release](https://github.com/EtherAura/Remora/releases/latest) carries:

- a source tarball,
- a `.deb` for Ubuntu 24.04 and later, and Debian (amd64),
- a `PKGBUILD` for Arch Linux,
- SHA-256 checksums, an SBOM, and a Sigstore build-provenance attestation.

Remora uses calendar versions, `YYYY.MM.PATCH`. `remora --version` prints the version you have.

A release never contains an Android image. You build your own: see
[Building the Image](Building-the-Image.md).

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

## From source

Install the build dependencies for your distribution first: see
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

The source tarball and the `.deb` each carry a Sigstore-backed build-provenance attestation that
records which workflow, at which commit, produced those exact bytes. Check it with the GitHub CLI:

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
release assets at all. [SECURITY.md](../../SECURITY.md) has the full statement.

## After installing

Whichever way you installed it:

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

Continue with [Getting Started](Getting-Started.md).
