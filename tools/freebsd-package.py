#!/usr/bin/env python3
"""Package a native Remora build on FreeBSD with its Linux deployment helpers."""

import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
from tempfile import TemporaryDirectory


PREFIX = Path("/usr/local")
HELPERS = ("fwd", "uinput-kbd")


def output(*command: str) -> str:
    return subprocess.check_output(command, text=True).strip()


def dependencies(executable: Path) -> dict[str, dict[str, str]]:
    # ADB is a subprocess and Qt loads its SQLite and SVG plugins at runtime.
    packages = {"android-tools", "qt6-base_sqldriver-sqlite", "qt6-svg"}
    needed = re.findall(r"\(NEEDED\).*?\[([^]]+)\]", output("readelf", "-d", str(executable)))
    libraries = dict(line.split("\t", 1) for line in
                     output("ldd", "-f", r"%o\t%p\n", str(executable)).splitlines())
    for name in needed:
        path = Path(libraries.get(name, ""))
        if not path.is_absolute() or not path.is_file():
            raise RuntimeError(f"Cannot resolve Remora runtime library {name}")
        if not path.is_relative_to("/lib") and not path.is_relative_to("/usr/lib"):
            packages.add(output("pkg", "which", "-q", str(path)))

    result = {}
    for package in sorted(packages):
        name, origin, version = output("pkg", "query", r"%n\t%o\t%v", package).split("\t")
        result[name] = {"origin": origin, "version": version}
    return result


def check_public_vendor(vendor: Path) -> None:
    # Match the payload exclusions in the public source export. A local image build can leave
    # these files even in a public checkout, and CMake installs the entire vendor directory.
    blob_suffixes = {".apk", ".so", ".jar", ".dex", ".zip", ".img", ".bin", ".ttf"}
    features = vendor / "source-build/features"
    private_trees = (
        features / "widevine/vendor", features / "ndk_translation/etc",
        features / "ndk_translation/guest", features / "ndk_translation/lib64",
        features / "gapps/product",
    )
    shamiko = features / "zygisk_pif/shamiko"
    for path in sorted(vendor.rglob("*")):
        if not path.is_file() and not path.is_symlink():
            continue
        if (path.suffix.lower() in blob_suffixes
                or any(path.is_relative_to(tree) for tree in private_trees)
                or (path.is_relative_to(shamiko) and path.name != "REMORA.md")):
            raise RuntimeError(f"Local vendor payload {path.relative_to(vendor)} would enter "
                               "the package. Use a clean public source checkout.")


def package(build_dir: Path, output_dir: Path) -> Path:
    abi = output("pkg", "config", "ABI")
    _, major, arch = abi.split(":")
    if arch != "amd64":
        raise RuntimeError(f"The bundled Linux helpers require amd64, got {abi}")

    cache = (build_dir / "CMakeCache.txt").read_text().splitlines()
    if (f"CMAKE_INSTALL_PREFIX:PATH={PREFIX}" not in cache
            or "REMORA_INSTALLED_VENDOR:BOOL=ON" not in cache):
        raise RuntimeError("Configure with -DCMAKE_INSTALL_PREFIX=/usr/local "
                           "and -DREMORA_INSTALLED_VENDOR=ON")

    output_dir.mkdir(parents=True, exist_ok=True)
    with TemporaryDirectory(prefix="remora-pkg-", dir=output_dir.resolve()) as temp:
        root = Path(temp) / "stage"
        metadata = Path(temp) / "metadata"
        metadata.mkdir()
        subprocess.run(["cmake", "--install", str(build_dir)],
                       env={**os.environ, "DESTDIR": str(root)}, check=True)
        installed = root / PREFIX.relative_to("/")
        vendor = installed / "share/remora/vendor"
        check_public_vendor(vendor)
        native = vendor / "native"
        for name in HELPERS:
            helper = native / name
            if (not helper.is_file() or not os.access(helper, os.X_OK)
                    or "OS: Linux" not in output("readelf", "-n", str(helper))
                    or re.search(r"^\s*INTERP\s", output("readelf", "-lW", str(helper)), re.M)
                    or "X86-64" not in output("readelf", "-h", str(helper)).upper()):
                raise RuntimeError(f"Build {name} on Linux amd64 with make -C vendor/native "
                                   "and copy it to vendor/native before packaging")

        executable = installed / "bin/remora"
        version = output(str(executable), "--version").removeprefix("remora ")
        if not re.fullmatch(r"\d{4}\.\d{2}\.\d+", version):
            raise RuntimeError(f"Unexpected version from {executable}: {version!r}")
        manifest = {
            "name": "remora",
            "version": version,
            "origin": "deskutils/remora",
            "comment": "Deploy and mirror Android on a Linux Docker host",
            "desc": "A Qt desktop and command-line client for deploying and mirroring "
                    "Android on a Linux Docker host over SSH.",
            "maintainer": "etheraura@protonmail.com",
            "www": "https://github.com/EtherAura/Remora",
            "prefix": str(PREFIX),
            "abi": abi,
            "categories": ["deskutils"],
            "licenses": ["GPLv3"],
            "licenselogic": "single",
            "deps": dependencies(executable),
        }
        (metadata / "+MANIFEST").write_text(json.dumps(manifest, indent=2) + "\n")
        plist = metadata / "plist"
        files = sorted(path.relative_to(installed).as_posix() for path in installed.rglob("*")
                       if path.is_file() or path.is_symlink())
        plist.write_text("\n".join(files) + "\n")
        subprocess.run(["pkg", "create", "-r", str(root), "-m", str(metadata),
                        "-p", str(plist), "-o", temp], check=True)
        artifact = output_dir / f"Remora-{version}-freebsd{major}-{arch}.pkg"
        (Path(temp) / f"remora-{version}.pkg").replace(artifact)
    return artifact


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build_dir", type=Path, help="CMake build directory containing Remora")
    parser.add_argument("--output-dir", type=Path, default=Path("dist"))
    args = parser.parse_args()
    if not sys.platform.startswith("freebsd"):
        parser.error("run this command on the FreeBSD build host")
    print(package(args.build_dir, args.output_dir))
