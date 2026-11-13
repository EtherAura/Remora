#!/usr/bin/env python3
"""Package an audited public_release image as a signed, hostable release (bd remora-qksj).

    package-release.py build  --image <tag> --tree <aosp tree> --public-tree <dir> --out <dir>
                              --key <ed25519 private key> [--previous-index <index.json>]
    package-release.py resign --out <dir> --key <key>
    package-release.py publish --out <dir> [--repo EtherAura/Remora] [--release android-images]

`build` writes everything a release consists of into --out and touches nothing outside it except
one `docker tag` (the immutable version tag the archive carries):

    <name>.tar.xz           the image, `docker save` of the version tag, xz-compressed
    <name>.spdx.json        the product SBOM the build generated (`m sbom`)
    <name>-notices.tar      every partition's NOTICE.xml.gz, as shipped inside the image
    <name>-audit.json.gz    the release audit public-audit.py wrote when the image was assembled
    <name>-provenance.json  what was built from what: fingerprint, features, patches, revisions
    <name>-src-*.tar.xz     corresponding source: Remora's public tree, and every third-party
                            project the audit found under a copyleft licence, at the revision built
    index.json + .sig       the catalog `remora images` authenticates before it downloads anything

Nothing is uploaded by `build`. `publish` runs `gh release upload` for the files in --out and is
the only step that reaches the network; it is a separate command on purpose, because publishing is
the one irreversible step and the owner decides when it happens.

THE SOURCE IS PUBLISHED ALONGSIDE THE BINARY, NOT OFFERED. GPLv2 §3(a) and GPLv3 §6(a)/(d) are
satisfied by making the Corresponding Source available from the same place as the object code, for
as long as the object code is: so the sources are release assets of the same release, and the
catalog binds their hashes exactly as it binds the image's. Never delete a source asset while the
image it belongs to is still listed.
"""

import argparse
import datetime as dt
import gzip
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import xml.etree.ElementTree as ET

HERE = Path(__file__).resolve().parent
ASSET_LIMIT = 2 * 1024**3  # GitHub's per-asset ceiling, and the catalog's
VALIDITY = dt.timedelta(days=90)  # the catalog refuses more than 93; re-sign quarterly

spec = importlib.util.spec_from_file_location('catalog', HERE / 'catalog.py')
catalog = importlib.util.module_from_spec(spec)
spec.loader.exec_module(catalog)


class PackagingError(Exception):
    pass


def require(ok, msg):
    if not ok:
        raise PackagingError(msg)


def sh(*argv, **kw):
    return subprocess.run(argv, check=True, capture_output=True, text=True, **kw).stdout


def asset(path):
    path = Path(path)
    size = path.stat().st_size
    require(size <= ASSET_LIMIT, '%s is %d bytes, over the 2 GiB asset limit' % (path.name, size))
    return {'file': path.name, 'bytes': size, 'sha256': catalog.sha(path)}


def image_file(image, inner, dest):
    """docker cp one file out of an image without starting it."""
    cid = sh('docker', 'create', image).strip()
    try:
        r = subprocess.run(['docker', 'cp', '%s:%s' % (cid, inner), str(dest)],
                           capture_output=True, text=True)
        return r.returncode == 0
    finally:
        subprocess.run(['docker', 'rm', '-f', cid], capture_output=True)


def props(path):
    out = {}
    for line in Path(path).read_text(errors='replace').splitlines():
        k, sep, v = line.partition('=')
        if sep and not k.startswith('#'):
            out[k.strip()] = v.strip()
    return out


def project_paths(tree):
    return sorted((Path(tree) / '.repo/project.list').read_text().split(), key=len, reverse=True)


def owning_project(package, projects):
    for p in projects:
        if package == p or package.startswith(p + '/'):
            return p
    return None


def pinned_revisions(remora_checkout):
    m = ET.parse(remora_checkout / 'vendor/source-build/pinned-manifest-a17.xml').getroot()
    return {p.get('path', p.get('name')): p.get('revision') for p in m.iter('project')}


def source_archive(tree, project, out, name):
    """`git archive HEAD` of one project as built, plus its uncommitted diff when there is one.

    HEAD, not the pinned revision: the patch pass commits Remora's patches into each project
    (git am), so HEAD IS the patched source that was compiled."""
    path = Path(tree) / project
    head = sh('git', '-C', str(path), 'rev-parse', 'HEAD').strip()
    dest = out / ('%s-src-%s.tar.xz' % (name, project.replace('/', '_')))
    with tempfile.TemporaryDirectory() as tmp:
        tar = Path(tmp) / 'src.tar'
        sh('git', '-C', str(path), 'archive', '--format=tar', '--prefix=%s/' % project,
           '-o', str(tar), 'HEAD')
        diff = sh('git', '-C', str(path), 'diff', 'HEAD')
        if diff:
            with tarfile.open(tar, 'a') as t:
                data = diff.encode()
                info = tarfile.TarInfo('%s.uncommitted.diff' % project)
                info.size = len(data)
                t.addfile(info, fileobj=io.BytesIO(data))
        with open(dest, 'wb') as f:
            subprocess.run(['xz', '-T0', '-6', '-c', str(tar)], stdout=f, check=True)
    return dest, head, bool(diff)


def build(a):
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    require(not any(out.iterdir()), '%s is not empty — refusing to mix two releases' % out)
    tree, public_tree = Path(a.tree), Path(a.public_tree)
    remora = HERE.parent.parent

    info = json.loads(sh('docker', 'image', 'inspect', a.image))[0]
    image_id = info['Id']
    labels = info['Config'].get('Labels') or {}
    require(labels.get('remora.public_release') == 'true', 'not a public_release image')
    require(labels.get('remora.arch_variant') == 'x86_64',
            'arch variant is %r; only the portable x86_64 baseline is published'
            % labels.get('remora.arch_variant'))

    audit_path = Path(a.audit_dir) / (image_id.replace(':', '-') + '.audit.json')
    require(audit_path.exists(), 'no release audit for %s at %s' % (image_id, audit_path))
    audit = json.loads(audit_path.read_text())
    require(audit['passed'] and audit['image_id'] == image_id and not audit['failures'],
            'the audit for %s did not pass' % image_id)

    with tempfile.TemporaryDirectory() as tmp:
        bp = Path(tmp) / 'build.prop'
        require(image_file(a.image, '/system/build.prop', bp), 'no /system/build.prop in the image')
        p = props(bp)
    sdk = int(p['ro.build.version.sdk'])
    fingerprint = p['ro.build.fingerprint']
    build_number = p['ro.build.version.incremental']
    require(re.fullmatch(r'[0-9]{8}\.[0-9]{6}', build_number),
            'build number %r is not a do-build.sh UTC stamp' % build_number)
    version_tag = 'remora24:x86_64-public-%s' % build_number
    name = 'remora24-x86_64-public-%s' % build_number
    existing = catalog.image_id(version_tag)
    require(existing in (None, image_id), '%s already names different bytes' % version_tag)
    sh('docker', 'tag', image_id, version_tag)

    # --- the image ---------------------------------------------------------------------------
    archive = out / (name + '.tar.xz')
    print('saving %s -> %s' % (version_tag, archive.name), flush=True)
    with open(archive, 'wb') as f:
        save = subprocess.Popen(['docker', 'save', version_tag], stdout=subprocess.PIPE)
        subprocess.run(['xz', '-T0', '-6', '-c'], stdin=save.stdout, stdout=f, check=True)
        require(save.wait() == 0, 'docker save failed')
    entry = {'tag': 'remora24:x86_64-public', 'version_tag': version_tag, 'image_id': image_id,
             'archive': asset(archive)}
    catalog.archive_identity(archive, entry)  # the verifier the client runs, run on our own output

    # --- SBOM: must be THIS build's, which gen_sbom records by fingerprint ------------------------
    sbom_src = tree / 'out/soong/sbom/remora_x86_64/sbom.spdx.json'
    require(sbom_src.exists(), 'no SBOM at %s (the public build runs `m droid sbom`)' % sbom_src)
    require(fingerprint in sbom_src.read_text(errors='replace')[:200000],
            'the SBOM does not name fingerprint %s — it is from another build' % fingerprint)
    shutil.copyfile(sbom_src, out / (name + '.spdx.json'))
    entry['sbom'] = asset(out / (name + '.spdx.json'))

    # --- notices: exactly the NOTICE files the image carries ---------------------------------
    notices = out / (name + '-notices.tar')
    with tempfile.TemporaryDirectory() as tmp, tarfile.open(notices, 'w') as t:
        found = 0
        for part in ('system', 'vendor', 'system/product', 'system/system_ext', 'product',
                     'system_ext', 'odm', 'vendor_dlkm', 'system_dlkm'):
            dest = Path(tmp) / (part.replace('/', '_') + '-NOTICE.xml.gz')
            if image_file(a.image, '/%s/etc/NOTICE.xml.gz' % part, dest):
                t.add(dest, arcname='%s/etc/NOTICE.xml.gz' % part)
                found += 1
        require(found, 'the image carries no NOTICE.xml.gz at all')
        # Projects that declare no licence in their build files are missing from the NOTICE files
        # above (soong generates those from the same metadata), which is how bash, nano and the
        # other LineageOS extras would ship without the GPL text that has to travel with them. The
        # audit classified them from each project's own COPYING/LICENSE; ship exactly those.
        inferred = sorted({e['package'] for e in audit['files']
                           if any(l.startswith('(project text)') for l in e.get('licences', []))})
        for proj in inferred:
            for n in sorted(os.listdir(tree / proj)):
                f = tree / proj / n
                if re.match(r'(COPYING|LICEN[CS]E|NOTICE)', n) and f.is_file():
                    t.add(f, arcname='project-licences/%s/%s' % (proj, n))
    entry['notices'] = asset(notices)

    with gzip.open(out / (name + '-audit.json.gz'), 'wb') as f:
        f.write(audit_path.read_bytes())
    entry['audit'] = asset(out / (name + '-audit.json.gz'))

    # --- corresponding source -----------------------------------------------------------------
    sources = []
    require((public_tree / 'LICENSE').exists() and (public_tree / 'vendor').is_dir(),
            '%s is not a make-public-tree.sh output' % public_tree)
    pub = out / ('%s-src-remora.tar.xz' % name)
    with open(pub, 'wb') as f:
        tar = subprocess.Popen(['tar', '-C', str(public_tree.parent), '--exclude=.git', '-cf', '-',
                                public_tree.name], stdout=subprocess.PIPE)
        subprocess.run(['xz', '-T0', '-6', '-c'], stdin=tar.stdout, stdout=f, check=True)
        require(tar.wait() == 0, 'tar of the public tree failed')
    sources.append(asset(pub) | {'project': 'remora', 'role': 'Remora public tree'})
    public_commit = subprocess.run(['git', '-C', str(public_tree), 'rev-parse', 'HEAD'],
                                   capture_output=True, text=True).stdout.strip() or None

    projects = project_paths(tree)
    pins = pinned_revisions(remora)
    wanted = {}
    for o in audit['copyleft_obligations']:
        proj = owning_project(o['package'], projects)
        if proj is None:
            continue  # Remora-staged, in the public tree above
        wanted.setdefault(proj, set()).update(o['licences'])
    # The Mesa payload is a host-side build of external/mesa3d (its NOTICE says which commit), so
    # its source is that project even though no soong module of it is installed.
    if any(e.get('package', '').startswith('vendor/mesa_source') for e in audit['files']):
        wanted.setdefault('external/mesa3d', set()).add('external_mesa3d_license')
    revisions = {}
    for proj in sorted(wanted):
        print('source: %s' % proj, flush=True)
        dest, head, dirty = source_archive(tree, proj, out, name)
        revisions[proj] = {'head': head, 'pinned': pins.get(proj), 'uncommitted_diff': dirty}
        sources.append(asset(dest) | {'project': proj, 'revision': head,
                                      'licences': sorted(wanted[proj])})
    entry['sources'] = sources

    # --- provenance --------------------------------------------------------------------------
    prov = {
        'schema': 1, 'image_id': image_id, 'version_tag': version_tag,
        'fingerprint': fingerprint, 'build_number': build_number, 'android_sdk': sdk,
        'labels': labels,
        'audit_component_inventory_sha256': audit['component_inventory_sha256'],
        'remora_public_tree_commit': public_commit,
        'pinned_manifest_sha256': hashlib.sha256(
            (remora / 'vendor/source-build/pinned-manifest-a17.xml').read_bytes()).hexdigest(),
        'patched_projects': {p: r for p, r in revisions.items() if r['head'] != r['pinned']},
        'source_projects': revisions,
        'abi': ['x86_64'],
        'limitations': 'x86_64 only: no ARM translation, so apps that ship only ARM native '
                       'code refuse to install. No Google apps; `remora play-install` adds '
                       'Google Play locally, as a private derivative that is never published.',
    }
    (out / (name + '-provenance.json')).write_text(json.dumps(prov, indent=2, sort_keys=True) + '\n')
    entry['provenance'] = asset(out / (name + '-provenance.json'))
    entry.update(android_sdk=sdk, abi=['x86_64'], arch_variant='x86_64', fingerprint=fingerprint,
                 public_audit_passed=True, google_payloads=False)

    previous = json.loads(Path(a.previous_index).read_text()) if a.previous_index else None
    images = [im for im in (previous or {}).get('images', [])
              if im['version_tag'] != version_tag]
    sign_catalog(out, a.key, [entry] + images, (previous or {}).get('sequence', 0) + 1)
    total = sum(f.stat().st_size for f in out.iterdir())
    print('release %s: %d files, %.2f GiB in %s' % (version_tag, len(list(out.iterdir())),
                                                     total / 1024**3, out))


def sign_catalog(out, key, images, sequence):
    now = dt.datetime.now(dt.timezone.utc).replace(microsecond=0)
    index = {'schema': 1, 'channel': 'remora-public-android', 'sequence': sequence,
             'published_at': now.isoformat(), 'expires_at': (now + VALIDITY).isoformat(),
             'images': images}
    data = (json.dumps(index, indent=2, sort_keys=True) + '\n').encode()
    (out / 'index.json').write_bytes(data)
    sh('openssl', 'pkeyutl', '-sign', '-inkey', key, '-rawin', '-in', str(out / 'index.json'),
       '-out', str(out / 'index.json.sig'))
    # Verify with the PUBLIC trust anchor the client ships, so a key mix-up fails here and not
    # on every downloader's machine.
    catalog.verify_catalog(data, (out / 'index.json.sig').read_bytes(),
                           HERE / 'image-signing-key.pem')
    print('signed index.json: sequence %d, valid until %s' % (sequence, index['expires_at']))


def resign(a):
    out = Path(a.out)
    index = json.loads((out / 'index.json').read_text())
    sign_catalog(out, a.key, index['images'], index['sequence'] + 1)


def publish(a):
    out = Path(a.out)
    files = sorted(str(f) for f in out.iterdir())
    require(any(f.endswith('index.json.sig') for f in files), 'no signed catalog in %s' % out)
    exists = subprocess.run(['gh', 'release', 'view', a.release, '--repo', a.repo],
                            capture_output=True).returncode == 0
    if not exists:
        sh('gh', 'release', 'create', a.release, '--repo', a.repo, '--title',
           'Remora public Android images', '--notes',
           'Signed catalog (index.json) and audited, redistributable Android images for '
           '`remora images fetch`, with their SBOMs, notices and corresponding source. '
           'See docs/IMAGE_MIRROR.md.', '--latest=false')
    # Image and sources first, the catalog LAST: a client never sees an index naming assets
    # that are not up yet.
    body = [f for f in files if not os.path.basename(f).startswith('index.json')]
    sh('gh', 'release', 'upload', a.release, '--repo', a.repo, '--clobber', *body)
    sh('gh', 'release', 'upload', a.release, '--repo', a.repo, '--clobber',
       str(out / 'index.json'), str(out / 'index.json.sig'))
    print('published %d files to %s release %s' % (len(files), a.repo, a.release))


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    sub = p.add_subparsers(dest='cmd', required=True)
    b = sub.add_parser('build')
    b.add_argument('--image', required=True)
    b.add_argument('--tree', required=True)
    b.add_argument('--public-tree', required=True)
    b.add_argument('--out', required=True)
    b.add_argument('--key', required=True)
    b.add_argument('--audit-dir', default=str(Path.home() / '.local/share/remora/release-audits'))
    b.add_argument('--previous-index')
    r = sub.add_parser('resign')
    r.add_argument('--out', required=True)
    r.add_argument('--key', required=True)
    u = sub.add_parser('publish')
    u.add_argument('--out', required=True)
    u.add_argument('--repo', default='EtherAura/Remora')
    u.add_argument('--release', default='android-images')
    a = p.parse_args(argv)
    {'build': build, 'resign': resign, 'publish': publish}[a.cmd](a)
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (PackagingError, catalog.VerificationError, subprocess.CalledProcessError) as e:
        detail = getattr(e, 'stderr', '') or ''
        print('package-release: %s %s' % (e, detail.strip()[:400]), file=sys.stderr)
        sys.exit(1)
