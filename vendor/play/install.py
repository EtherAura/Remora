#!/usr/bin/env python3
"""Opt-in, host-local Play overlay. No third-party installer code is executed.

The pinned bytes were independently verified with Android's APK signature verifier;
hash checks bind installation to those exact APKs and their recorded signing lineages.
"""
import argparse
import contextlib
import datetime
import fcntl
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import re
import signal
import struct
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
import zipfile


# The API 37 GMS requests this permission but the publisher omits an allow/deny entry.
# It is unnecessary for this container's Play Store; explicitly deny it rather than
# disabling privileged-permission enforcement or granting companion-device access.
LOCAL_FILES = {'system/product/etc/permissions/remora-play-denials.xml': b'''<?xml version="1.0" encoding="utf-8"?>
<permissions>
    <privapp-permissions package="com.google.android.gms">
        <deny-permission name="android.permission.ACCESS_COMPANION_INFO" />
    </privapp-permissions>
</permissions>
'''}


class InstallError(Exception):
    pass


def require(ok, message):
    if not ok:
        raise InstallError(message)


def terms(m):
    return (f"Provider: {m['provider']} — {m['release']}\n"
            f"Third-party terms: {m['terms_url']}\nGoogle terms: {m['google_terms_url']}\n"
            "Google APKs remain proprietary; the installer licence does not license them.\n"
            "Remora is not Google licensed or certified. Service access, Play Integrity, DRM\n"
            "and app compatibility are not guaranteed. No identity/attestation spoofing is added.\n"
            "This downloads directly from the provider on the Docker host and creates a local\n"
            "image. Do not publish or mirror it. The original image and Android data are retained.\n"
            "Use the new image with fresh Android data; removing it does not reset app data.\n"
            f"Acceptance version: {m['terms_version']}")


def digest_file(path):
    h = hashlib.sha256()
    size = 0
    with open(path, 'rb') as f:
        for data in iter(lambda: f.read(1024 * 1024), b''):
            h.update(data)
            size += len(data)
    return size, h.hexdigest()


def verify_archive(path, m):
    require(digest_file(path) == (m['archive_bytes'], m['archive_sha256']),
            'Archive size/SHA-256 mismatch; nothing installed.')


class HttpsOnly(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        require(newurl.startswith('https://'), 'Refusing non-HTTPS download redirect')
        return super().redirect_request(req, fp, code, msg, headers, newurl)


def download(m, cache):
    path = cache / (m['archive_sha256'] + '.zip')
    if path.exists():
        verify_archive(path, m)
        return path
    require(m['source_url'].startswith('https://'), 'Provider URL must use HTTPS')
    print('Downloading pinned package directly from ' + m['source_url'], flush=True)
    fd, tmp = tempfile.mkstemp(prefix='download-', dir=cache)
    try:
        with os.fdopen(fd, 'wb') as dst, urllib.request.build_opener(HttpsOnly()).open(
                m['source_url'], timeout=60) as src:
            count = 0
            for data in iter(lambda: src.read(1024 * 1024), b''):
                count += len(data)
                require(count <= m['archive_bytes'], 'Download exceeds pinned size')
                dst.write(data)
        verify_archive(tmp, m)
        os.replace(tmp, path)
        return path
    finally:
        Path(tmp).unlink(missing_ok=True)


def safe_name(name):
    p = PurePosixPath(name)
    require(not p.is_absolute() and '..' not in p.parts and '\\' not in name,
            'Unsafe archive path: ' + name)
    return str(p)


def extract_payload(archive, m, root):
    """Allowlisted, byte-pinned COPY data only; never extract archive code/tools."""
    seen = set()
    with zipfile.ZipFile(archive) as z:
        require(z.namelist().count(m['tar_entry']) == 1, 'Missing/duplicate payload tar')
        with z.open(m['tar_entry']) as nested, tarfile.open(fileobj=nested, mode='r|xz') as tar:
            for member in tar:
                name = safe_name(member.name)
                require(name not in seen, 'Duplicate archive member: ' + name)
                seen.add(name)
                require(member.isdir() or member.isfile(), 'Archive contains a link/device: ' + name)
                if not name.startswith(m['tar_prefix']):
                    continue
                rel = name[len(m['tar_prefix']):]
                if rel not in m['files']:
                    continue
                expected = m['files'][rel]
                require(member.isfile() and member.size == expected['bytes'], 'Wrong payload size: ' + rel)
                target = root / rel
                target.parent.mkdir(parents=True, exist_ok=True, mode=0o755)
                with tar.extractfile(member) as src, open(target, 'wb') as dst:
                    h = hashlib.sha256()
                    for data in iter(lambda: src.read(1024 * 1024), b''):
                        dst.write(data)
                        h.update(data)
                require(h.hexdigest() == expected['sha256'], 'Payload SHA-256 mismatch: ' + rel)
                target.chmod(0o644)
    require(all((root / name).is_file() for name in m['files']), 'Incomplete payload')
    for directory in [root, *[p for p in root.rglob('*') if p.is_dir()]]:
        directory.chmod(0o755)
    expected_apks = {a['package']: a['sha256'] for a in m['apks']}
    actual = {}
    for name, meta in m['files'].items():
        if name.endswith('.apk'):
            pkg = apk_package((root / name).read_bytes())
            require(pkg not in actual, 'Duplicate payload package: ' + pkg)
            actual[pkg] = meta['sha256']
    require(actual == expected_apks, 'APK package/hash set differs from audited signing identities')


def axml_package(data):
    """Read the manifest package from Android binary XML, without SDK binaries."""
    require(len(data) >= 8, 'Truncated Android manifest')
    kind, header, total = struct.unpack_from('<HHI', data)
    require(kind == 3 and header == 8 and total == len(data), 'Invalid Android XML header')
    strings = []
    pos = header
    while pos < total:
        require(pos + 8 <= total, 'Truncated XML chunk')
        kind, head, size = struct.unpack_from('<HHI', data, pos)
        require(8 <= head <= size and pos + size <= total, 'Invalid XML chunk bounds')
        chunk = data[pos:pos + size]
        if kind == 1:
            require(head >= 28, 'Invalid string pool')
            count, _, flags, start, _ = struct.unpack_from('<IIIII', chunk, 8)
            require(count <= 100000 and head + count * 4 <= size and start <= size,
                    'Invalid string offsets')
            strings = []
            def length(at, wide=False):
                unit = 2 if wide else 1
                require(at + unit <= size, 'Truncated XML string length')
                n = int.from_bytes(chunk[at:at + unit], 'little')
                at += unit
                mask = 0x8000 if wide else 0x80
                if n & mask:
                    require(at + unit <= size, 'Truncated XML string length')
                    n = ((n & (mask - 1)) << (16 if wide else 8)) | int.from_bytes(chunk[at:at + unit], 'little')
                    at += unit
                return n, at
            for i in range(count):
                off = start + struct.unpack_from('<I', chunk, head + i * 4)[0]
                if flags & 0x100:
                    _, off = length(off)
                    n, off = length(off)
                    require(off + n < size, 'Invalid UTF-8 string bounds')
                    strings.append(chunk[off:off + n].decode('utf-8'))
                else:
                    n, off = length(off, True)
                    require(off + n * 2 + 2 <= size, 'Invalid UTF-16 string bounds')
                    strings.append(chunk[off:off + n * 2].decode('utf-16le'))
        elif kind == 0x102:
            require(head == 16 and size >= 36, 'Invalid XML element')
            _, name, attr_start, attr_size, count = struct.unpack_from('<IIHHH', chunk, 16)
            require(name < len(strings), 'Invalid element name')
            if strings[name] == 'manifest':
                require(attr_size >= 20 and 16 + attr_start + count * attr_size <= size,
                        'Invalid manifest attributes')
                for i in range(count):
                    at = 16 + attr_start + i * attr_size
                    ns, key, raw = struct.unpack_from('<III', chunk, at)
                    require(key < len(strings), 'Invalid attribute name')
                    if ns == 0xffffffff and strings[key] == 'package':
                        if raw == 0xffffffff:
                            require(chunk[at + 15] == 3, 'Non-string package name')
                            raw = struct.unpack_from('<I', chunk, at + 16)[0]
                        require(raw < len(strings), 'Invalid package string')
                        pkg = strings[raw]
                        require(pkg == 'android' or re.fullmatch(r'[A-Za-z][A-Za-z0-9_]*(?:\.[A-Za-z0-9_]+)+', pkg),
                                'Invalid package identifier')
                        return pkg
                raise InstallError('Manifest has no package')
        pos += size
    raise InstallError('No manifest element')


def apk_package(data):
    with zipfile.ZipFile(io.BytesIO(data)) as z:
        info = z.getinfo('AndroidManifest.xml')
        require(info.file_size <= 16 * 1024 * 1024, 'Oversize APK manifest')
        return axml_package(z.read(info))


def docker(*args, check=True):
    p = subprocess.run(['docker', *args], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    require(not check or p.returncode == 0, p.stderr.strip() or 'Docker command failed')
    return p


def inspect_image(ref):
    require(ref and not ref.startswith('-') and not any(c.isspace() for c in ref), 'Invalid image reference')
    p = docker('image', 'inspect', ref, check=False)
    if p.returncode:
        # A transport/daemon failure must not be mistaken for a tag available to overwrite.
        require('No such image' in p.stderr or 'No such object' in p.stderr, p.stderr.strip())
        return None
    return json.loads(p.stdout)[0]


def base_preflight(base, m):
    require(not base['Config'].get('OnBuild'), 'Base has ONBUILD instructions; COPY-only installation refused')
    require(base['Architecture'] == m['architecture'] and base['Os'] == 'linux',
            'This package requires a Linux x86_64 image')
    require(not (base['Config'].get('Labels') or {}).get('org.remora.play.local_only'),
            'Base already contains a Play overlay')
    cid = docker('create', '--entrypoint', '/system/bin/true', base['Id']).stdout.strip()
    props, links, packages = {}, set(), set()
    blocked = {a['package'] for a in m['apks']} | {'org.microg.gms', 'org.microg.gsf', 'com.mgoogle.android.gms'}
    try:
        print('Inspecting immutable base filesystem (no Android process is started)…', flush=True)
        with tempfile.TemporaryFile() as err:
            p = subprocess.Popen(['docker', 'export', cid], stdout=subprocess.PIPE, stderr=err)
            try:
                with tarfile.open(fileobj=p.stdout, mode='r|') as tar:
                    for entry in tar:
                        name = safe_name(entry.name)
                        if entry.issym() or entry.islnk():
                            links.add(name)
                        if not entry.isfile():
                            continue
                        if name in ('system/build.prop', 'system/system/build.prop'):
                            require(entry.size <= 1024 * 1024, 'Oversize build properties')
                            for line in tar.extractfile(entry).read().decode('utf-8').splitlines():
                                if '=' in line and not line.lstrip().startswith('#'):
                                    k, v = line.split('=', 1)
                                    props[k.strip()] = v.strip()
                        if name.endswith('.apk'):
                            require(entry.size <= 512 * 1024 * 1024, 'Oversize base APK')
                            pkg = apk_package(tar.extractfile(entry).read())
                            require(pkg not in blocked and not pkg.startswith('org.microg.'),
                                    'Base contains conflicting Google/microG package: ' + pkg)
                            packages.add(pkg)
                require(p.wait() == 0, 'Cannot export base filesystem')
            finally:
                if p.poll() is None:
                    p.terminate()
                p.stdout.close()
                p.wait()
        require(props.get('ro.build.version.sdk') == str(m['android_sdk']), 'Base must be Android SDK ' + str(m['android_sdk']))
        require('x86_64' in props.get('ro.product.cpu.abilist', props.get('ro.product.cpu.abi', '')).split(','),
                'Base does not advertise x86_64 Android ABI')
        for name in list(m['files']) + list(LOCAL_FILES):
            require(name not in links and not any(str(p) in links for p in PurePosixPath(name).parents if str(p) != '.'),
                    'Overlay would traverse a base symlink: ' + name)
        return {'sdk': m['android_sdk'], 'packages_scanned': len(packages)}
    finally:
        docker('rm', '-v', cid)


def local_tag(tag):
    require(re.fullmatch(r'remora-local:[a-zA-Z0-9_][a-zA-Z0-9_.-]{0,127}', tag),
            'Derived images must use a remora-local:<tag> local-only name')
    return tag


def atomic_json(path, value):
    fd, tmp = tempfile.mkstemp(dir=path.parent, prefix='.receipt-')
    try:
        with os.fdopen(fd, 'w') as f:
            json.dump(value, f, indent=2, sort_keys=True)
            f.write('\n')
        os.replace(tmp, path)
    finally:
        Path(tmp).unlink(missing_ok=True)


def rollback(tag, cache):
    local_tag(tag)
    image = inspect_image(tag)
    require(image is not None, 'Overlay does not exist')
    labels = image['Config'].get('Labels') or {}
    require(labels.get('org.remora.play.local_only') == 'true', 'Not a Remora Play overlay')
    base_id = labels.get('org.remora.play.base', '')
    require(base_id and inspect_image(base_id), 'Original base is missing; restore it before rollback')
    for cid in docker('ps', '-aq', '--no-trunc').stdout.split():
        c = json.loads(docker('inspect', cid).stdout)[0]
        require(c['Image'] != image['Id'], 'Overlay is used by a container (running or stopped); remove that container first')
    # Docker otherwise prunes untagged parent images, including a base supplied by ID.
    docker('image', 'rm', '--no-prune', tag)
    result = {'removed': tag, 'base': base_id, 'data_unchanged': True}
    atomic_json(cache / ('rollback-' + datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%S%f') + '.json'), result)
    return result


def install(args, m, cache):
    require(args.accept_terms == m['terms_version'], 'Read --show-terms; explicit acceptance of ' + m['terms_version'] + ' is required')
    base = inspect_image(args.base)
    require(base is not None, 'Base image must already be present on this Docker host')
    recipe = hashlib.sha256(Path(__file__).read_bytes() + json.dumps(m, sort_keys=True).encode()).hexdigest()
    tag = local_tag(args.tag or ('remora-local:play-' + base['Id'][7:19] + '-' + recipe[:12]))
    labels = {'org.remora.play.local_only': 'true', 'org.remora.play.base': base['Id'],
              'org.remora.play.archive_sha256': m['archive_sha256'], 'org.remora.play.terms': m['terms_version'],
              'org.remora.play.recipe': recipe}
    existing = inspect_image(tag)
    if existing:
        have = existing['Config'].get('Labels') or {}
        require(all(have.get(k) == v for k, v in labels.items()), 'Refusing to overwrite an existing image tag')
        receipt = cache / (existing['Id'][7:] + '.json')
        require(receipt.is_file(), 'Existing overlay has no local receipt; refusing to adopt it')
        return json.loads(receipt.read_text()) | {'tag': tag, 'reused': True}
    preflight = base_preflight(base, m)
    archive = Path(args.archive).resolve() if args.archive else download(m, cache)
    verify_archive(archive, m)
    with tempfile.TemporaryDirectory(prefix='stage-', dir=cache) as work:
        work = Path(work)
        payload = work / 'payload'
        extract_payload(archive, m, payload)
        local_files = {}
        for name, data in LOCAL_FILES.items():
            (payload / name).write_bytes(data)
            (payload / name).chmod(0o644)
            local_files[name] = {'sha256': hashlib.sha256(data).hexdigest(), 'bytes': len(data)}
        # No RUN instruction, archive script, bundled binary, or shell interpolation.
        lines = ['FROM ' + base['Id']]
        lines += ['LABEL ' + k + '=' + json.dumps(v) for k, v in labels.items()]
        lines += ['COPY --chown=0:0 payload/system/ /system/']
        (work / 'Dockerfile').write_text('\n'.join(lines) + '\n')
        iid = work / 'image-id'
        print('Building COPY-only local Play overlay…', flush=True)
        subprocess.run(['docker', 'build', '--network=none', '--pull=false', '--iidfile', str(iid), str(work)], check=True)
        image_id = iid.read_text().strip()
        require(inspect_image(tag) is None, 'Target tag appeared during build; refusing to overwrite it')
        result = {'tag': tag, 'image': image_id, 'base_ref': args.base, 'base': base['Id'],
                  'provider': m['provider'], 'archive_sha256': m['archive_sha256'],
                  'source_url': m['source_url'], 'terms_version': m['terms_version'],
                  'accepted_at': datetime.datetime.now(datetime.timezone.utc).isoformat(),
                  'files': m['files'], 'local_files': local_files, 'recipe_sha256': recipe,
                  'apks': m['apks'], 'preflight': preflight,
                  'local_only': True, 'data_unchanged': True}
        receipt = cache / (image_id[7:] + '.json')
        result['receipt'] = str(receipt)
        atomic_json(receipt, result)
        docker('tag', image_id, tag)
        return result


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--manifest', default=str(Path(__file__).with_name('package.json')))
    p.add_argument('--base')
    p.add_argument('--tag')
    p.add_argument('--archive', help='Exact pinned archive already present on the Docker host')
    p.add_argument('--accept-terms', metavar='VERSION')
    p.add_argument('--show-terms', action='store_true')
    p.add_argument('--rollback', metavar='TAG')
    args = p.parse_args(argv)
    m = json.loads(Path(args.manifest).read_text())
    if args.show_terms:
        print(terms(m))
        return 0
    require(bool(args.base) != bool(args.rollback), 'Specify exactly one of --base or --rollback')
    require(not args.rollback or not (args.tag or args.archive or args.accept_terms), 'Rollback cannot be combined with install options')
    # Acceptance precedes all filesystem changes and Docker/network activity.
    if not args.rollback:
        require(args.accept_terms == m['terms_version'], terms(m) + '\nExplicit acceptance is required.')
    require(not os.environ.get('DOCKER_HOST') or os.environ['DOCKER_HOST'].startswith('unix://'),
            'Use --host in Remora to run the download on a remote Docker host')
    context = json.loads(docker('context', 'inspect').stdout)[0]
    endpoint = context.get('Endpoints', {}).get('docker', {}).get('Host', '')
    require(endpoint.startswith('unix://'), 'Use Remora --host for a remote Docker context')
    cache = Path(os.environ.get('XDG_CACHE_HOME', str(Path.home() / '.cache'))) / 'remora' / 'play'
    cache.mkdir(parents=True, exist_ok=True, mode=0o700)
    cache.chmod(0o700)
    with open(cache / 'install.lock', 'a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        result = rollback(args.rollback, cache) if args.rollback else install(args, m, cache)
    visible = {k: result[k] for k in ('tag', 'image', 'base', 'receipt', 'removed', 'reused') if k in result}
    print('REMORA_PLAY_RESULT=' + json.dumps(visible, separators=(',', ':')), flush=True)
    return 0


if __name__ == '__main__':
    def interrupted(signum, frame):
        raise InstallError('Interrupted; original image and Android data are unchanged')
    signal.signal(signal.SIGTERM, interrupted)
    try:
        sys.exit(main())
    except (InstallError, OSError, ValueError, KeyError, struct.error, tarfile.TarError,
            zipfile.BadZipFile, subprocess.CalledProcessError, KeyboardInterrupt) as e:
        print('Play installation failed: ' + str(e), file=sys.stderr)
        sys.exit(1)
