#!/usr/bin/env python3
"""Authenticate the image catalog and archive before Docker import/promotion."""
import argparse
import datetime as dt
import fcntl
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys
import tarfile
import tempfile
import urllib.parse
import urllib.request


class VerificationError(Exception):
    pass


def require(value, message):
    if not value:
        raise VerificationError(message)


class HttpsOnly(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, url):
        require(url.startswith('https://'), 'Refusing an HTTP redirect')
        return super().redirect_request(req, fp, code, msg, headers, url)


def get(url, limit):
    require(url.startswith('https://'), 'Image sources must use HTTPS')
    with urllib.request.build_opener(HttpsOnly()).open(url, timeout=60) as src:
        data = src.read(limit + 1)
    require(len(data) <= limit, 'Response exceeds size limit')
    return data


def sha(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(1024 * 1024), b''):
            h.update(b)
    return h.hexdigest()


def run(*argv):
    p = subprocess.run(argv, capture_output=True, text=True)
    require(p.returncode == 0, p.stderr.strip() or p.stdout.strip() or 'Command failed: ' + argv[0])
    return p.stdout.strip()


def atomic_json(path, value):
    with tempfile.NamedTemporaryFile(mode='w', dir=path.parent, delete=False) as f:
        tmp = Path(f.name)
        json.dump(value, f, sort_keys=True, indent=2)
        f.write('\n')
    try:
        os.replace(tmp, path)
    finally:
        tmp.unlink(missing_ok=True)


def verify_catalog(data, signature, key, last=None, now=None):
    require(len(data) <= 1024 * 1024 and len(signature) == 64, 'Invalid catalog/signature size')
    with tempfile.TemporaryDirectory(prefix='remora-catalog-') as d:
        payload, sig = Path(d)/'catalog', Path(d)/'signature'
        payload.write_bytes(data)
        sig.write_bytes(signature)
        try:
            run('openssl', 'pkeyutl', '-verify', '-pubin', '-inkey', str(key), '-rawin',
                '-in', str(payload), '-sigfile', str(sig))
        except VerificationError:
            # OpenSSL's own wording names a digest routine; the fact that matters is the refusal.
            raise VerificationError('Catalog signature does not verify against the Remora release '
                                    'key — refusing it (tampered, truncated or not from Remora)')
    m = json.loads(data)
    require(m.get('schema') == 1 and m.get('channel') == 'remora-public-android', 'Wrong catalog type')
    require(type(m.get('sequence')) is int and m['sequence'] > 0, 'Invalid catalog sequence')
    now = now or dt.datetime.now(dt.timezone.utc)
    published = dt.datetime.fromisoformat(m['published_at'])
    expires = dt.datetime.fromisoformat(m['expires_at'])
    require(published.tzinfo and expires.tzinfo, 'Catalog timestamps must include timezone')
    require(published <= now + dt.timedelta(minutes=5) and published < expires and now < expires,
            'Catalog is expired or not yet valid')
    require(expires - published <= dt.timedelta(days=93), 'Catalog validity exceeds 93 days')
    digest = hashlib.sha256(data).hexdigest()
    if last:
        require(m['sequence'] >= last['sequence'], 'Refusing a stale catalog sequence')
        require(m['sequence'] != last['sequence'] or digest == last['sha256'], 'Sequence was reused for different bytes')
    require(isinstance(m.get('images'), list) and len(m['images']) <= 100, 'Invalid image list')
    tags = set()
    for image in m['images']:
        tag = image['tag']
        require(re.fullmatch(r'remora24:x86_64-public(?:-[a-zA-Z0-9_.-]+)?', tag), 'Invalid public image tag')
        require(tag not in tags, 'Duplicate image tag')
        tags.add(tag)
        for ref in [image['archive'], image['sbom'], image['notices'], image['audit'], image['provenance'], *image['sources']]:
            name = ref['file']
            require(re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_.-]+', name) and '..' not in name, 'Unsafe artifact filename')
            require(type(ref['bytes']) is int and 0 < ref['bytes'] <= 2 * 1024**3, 'Invalid artifact size')
            require(re.fullmatch(r'[0-9a-f]{64}', ref['sha256']), 'Invalid artifact digest')
        require(re.fullmatch(r'sha256:[0-9a-f]{64}', image['image_id']), 'Invalid Docker image ID')
        require(image['android_sdk'] == 37 and image['abi'] == ['x86_64'] and image['arch_variant'] == 'x86_64',
                'Unsupported public image platform')
        require(image.get('public_audit_passed') is True and image.get('google_payloads') is False,
                'Image lacks public release approval')
    return m, {'sequence': m['sequence'], 'sha256': digest}


def image_id(ref):
    p = subprocess.run(['docker','image','inspect','--format','{{.Id}}',ref], capture_output=True,text=True)
    if not p.returncode:
        return p.stdout.strip()
    require('No such image' in p.stderr or 'No such object' in p.stderr, p.stderr.strip())
    return None


def check_local_docker():
    require(not os.environ.get('DOCKER_HOST') or os.environ['DOCKER_HOST'].startswith('unix://'), 'Use Remora --host for a remote Docker daemon')
    c = json.loads(run('docker','context','inspect'))[0]
    require(c['Endpoints']['docker']['Host'].startswith('unix://'), 'Use Remora --host for a remote Docker context')


def download(base, item, cache):
    path = cache/item['sha256']
    if path.exists():
        if path.stat().st_size == item['bytes'] and sha(path) == item['sha256']:
            return path
        # A damaged cache entry is discarded and fetched again, never trusted and never a dead
        # end: the cache is named by the signed digest, so the only correct content is known.
        print('Discarding a corrupted cached copy of ' + item['file'], flush=True)
        path.unlink()
    print('Downloading verified artifact ' + item['file'], flush=True)
    fd, temp = tempfile.mkstemp(dir=cache, prefix='download-')
    try:
        url = base + '/' + item['file']
        with os.fdopen(fd,'wb') as dst, urllib.request.build_opener(HttpsOnly()).open(url,timeout=60) as src:
            count, h = 0, hashlib.sha256()
            for b in iter(lambda: src.read(1024 * 1024),b''):
                count += len(b)
                require(count <= item['bytes'], 'Archive exceeds signed size')
                dst.write(b)
                h.update(b)
        require(count == item['bytes'] and h.hexdigest() == item['sha256'], 'Archive size/SHA-256 mismatch; Docker was not changed')
        os.replace(temp,path)
        return path
    finally:
        Path(temp).unlink(missing_ok=True)


def archive_identity(path, image):
    """Read bounded config/manifest entries; never extract files onto the host.

    Both `docker save` layouts are accepted: the legacy one (config at <id>.json) and the OCI one
    Docker 25+ writes (config at blobs/sha256/<id>, plus an index.json). The OCI index carries its
    own image-name annotations, which `docker load` also tags by, so they are held to the same
    single signed version tag as manifest.json's RepoTags."""
    digest = image['image_id'][7:]
    configs = (digest + '.json', 'blobs/sha256/' + digest)
    with tarfile.open(path,'r:*') as t:
        names = set()
        manifest = index = config = None
        for member in t:
            p = PurePosixPath(member.name)
            require(not p.is_absolute() and '..' not in p.parts and str(p) not in names, 'Unsafe/duplicate Docker archive path')
            names.add(str(p))
            require(member.isfile() or member.isdir(), 'Docker archive contains links/devices')
            if str(p) in ('manifest.json', 'index.json') + configs:
                require(member.size <= 2 * 1024 * 1024, 'Oversize Docker manifest/config')
                data = t.extractfile(member).read()
                if str(p) == 'manifest.json': manifest = json.loads(data)
                elif str(p) == 'index.json': index = json.loads(data)
                else:
                    require(hashlib.sha256(data).hexdigest() == digest, 'Docker config digest mismatch')
                    config = json.loads(data)
        require(manifest and len(manifest) == 1 and manifest[0]['Config'] in configs and config,
                'Archive does not contain the signed image identity')
        require(manifest[0]['RepoTags'] == [image['version_tag']], 'Archive attempts to change unexpected Docker tags')
        repo, _, ref = image['version_tag'].rpartition(':')
        for m in (index or {}).get('manifests', []):
            a = m.get('annotations') or {}
            named = a.get('io.containerd.image.name')
            require(named in (None, image['version_tag'], 'docker.io/library/' + image['version_tag'])
                    and a.get('org.opencontainers.image.ref.name') in (None, ref),
                    'Archive index names an unexpected Docker tag')
        require(config['architecture'] == 'amd64' and config['os'] == 'linux', 'Wrong Docker platform')
        labels = config.get('config',{}).get('Labels') or {}
        require(labels.get('org.remora.play.local_only') is None,
                'Private Play overlays cannot be public images')
        require(labels.get('remora.public_release') == 'true',
                'Image is not a public_release build')


def fetch(base, image, catalog_state, cache):
    check_local_docker()
    require(re.fullmatch(r'remora24:x86_64-public-[A-Za-z0-9_.-]+', image['version_tag']), 'Invalid immutable version tag')
    old_version = image_id(image['version_tag'])
    require(old_version in (None,image['image_id']), 'Immutable version tag already names different bytes')
    archive = download(base,image['archive'],cache)
    archive_identity(archive,image)
    previous = image_id(image['tag'])
    # All verification precedes docker load. The archive only carries the immutable version tag.
    print('Importing authenticated image…', flush=True)
    run('docker','load','-i',str(archive))
    require(image_id(image['version_tag']) == image['image_id'], 'Imported image ID differs from signed metadata')
    backup = None
    if previous and previous != image['image_id']:
        backup = 'remora-rollback:public-' + previous[7:]
        require(image_id(backup) in (None,previous), 'Rollback tag conflicts with an existing image')
        run('docker','tag',previous,backup)
    receipt = {'tag':image['tag'],'image':image['image_id'],'previous':previous,'backup':backup,
               'catalog':catalog_state,'archive':image['archive'],'source':base,'data_unchanged':True}
    receipt_path = cache/(image['tag'].replace(':','_')+'.receipt.json')
    # Retain the original rollback boundary on an idempotent repeat.
    if previous == image['image_id'] and receipt_path.exists():
        receipt = json.loads(receipt_path.read_text())
    prepared = receipt_path.with_suffix('.pending.json')
    atomic_json(prepared,receipt)
    try:
        run('docker','tag',image['image_id'],image['tag'])
        os.replace(prepared,receipt_path)
    except BaseException:
        if previous:
            run('docker','tag',previous,image['tag'])
        elif image_id(image['tag']) == image['image_id']:
            run('docker','image','rm','--no-prune',image['tag'])
        raise
    return receipt | {'receipt':str(receipt_path)}


def rollback(tag, cache):
    check_local_docker()
    require(re.fullmatch(r'remora24:x86_64-public(?:-[A-Za-z0-9_.-]+)?',tag),'Invalid public image tag')
    path = cache/(tag.replace(':','_')+'.receipt.json')
    require(path.exists(),'No verified-import receipt for this tag')
    r = json.loads(path.read_text())
    require(image_id(tag) == r['image'],'Tag changed outside this import; rollback refused')
    if r.get('previous'):
        require(image_id(r['previous']) == r['previous'],'Retained previous image is missing')
        run('docker','tag',r['previous'],tag)
        return {'tag':tag,'image':r['previous'],'data_unchanged':True}
    for cid in run('docker','ps','-aq','--no-trunc').split():
        c = json.loads(run('docker','inspect',cid))[0]
        require(c['Image'] != r['image'],'Image is in use; remove its disposable container before rollback')
    run('docker','image','rm','--no-prune',tag)
    return {'removed':tag,'immutable_version_retained':True,'data_unchanged':True}


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('action',choices=['list','fetch','rollback'])
    p.add_argument('--source',default='https://github.com/EtherAura/Remora/releases/download/android-images')
    p.add_argument('--tag')
    p.add_argument('--key',default=str(Path(__file__).with_name('image-signing-key.pem')))
    a = p.parse_args(argv)
    u = urllib.parse.urlsplit(a.source)
    require(u.scheme == 'https' and u.netloc and not u.username and not u.password and not u.query and not u.fragment,
            'Source must be an HTTPS directory URL')
    base = a.source.rstrip('/')
    cache = Path(os.environ.get('XDG_CACHE_HOME',str(Path.home()/'.cache')))/'remora'/'images'
    cache.mkdir(parents=True,exist_ok=True,mode=0o700)
    cache.chmod(0o700)
    with open(cache/'import.lock','a') as lock:
        fcntl.flock(lock,fcntl.LOCK_EX)
        if a.action == 'rollback':
            require(a.tag,'Rollback needs --tag')
            result = rollback(a.tag,cache)
        else:
            statefile = cache/('catalog-'+hashlib.sha256(Path(a.key).read_bytes()).hexdigest()+'.json')
            previous = json.loads(statefile.read_text()) if statefile.exists() else None
            m,state = verify_catalog(get(base+'/index.json',1024*1024),get(base+'/index.json.sig',64),a.key,previous)
            atomic_json(statefile,state)
            if a.action == 'list': result = m
            else:
                selected = [im for im in m['images'] if im['tag'] == a.tag]
                require(len(selected) == 1,'Requested tag is absent from the authenticated catalog')
                result = fetch(base,selected[0],state,cache)
    print('REMORA_IMAGES_RESULT='+json.dumps(result,separators=(',',':')),flush=True)
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (VerificationError, OSError, ValueError, KeyError, tarfile.TarError, KeyboardInterrupt) as e:
        print('Image verification/import failed: '+str(e),file=sys.stderr)
        sys.exit(1)
