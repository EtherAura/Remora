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
    contexts = json.loads(run('docker','context','inspect'))
    require(type(contexts) is list and len(contexts) == 1, 'Invalid Docker context inspection')
    try:
        endpoint = contexts[0]['Endpoints']['docker']['Host']
    except (KeyError, TypeError):
        raise VerificationError('Invalid Docker context inspection') from None
    require(type(endpoint) is str and endpoint.startswith('unix://'),
            'Use Remora --host for a remote Docker context')
    return endpoint


# `fetch --progress` events, one stdout line each, flushed as it is written:
#   REMORA_FETCH_PROGRESS=download <done> <total>   bytes read so far of the signed archive size
#   REMORA_FETCH_PROGRESS=cached <total>            a cached copy of that size is being verified
#   REMORA_FETCH_PROGRESS=check                     the archive's identity is being inspected
#   REMORA_FETCH_PROGRESS=import                    docker load and promotion have begun
# Only these phase names and whole counts, never log text or URLs. The total is always the size
# the catalog signed (a download that exceeds it is refused), so there is no unknown total.
PROGRESS_COUNTS = {'download': 2, 'cached': 1, 'check': 0, 'import': 0}


def report_progress(phase, *counts):
    require(PROGRESS_COUNTS.get(phase) == len(counts)
            and all(type(n) is int and n >= 0 for n in counts)
            and (not counts or (counts[-1] > 0 and counts[0] <= counts[-1])),
            'Invalid progress event')
    print(' '.join(['REMORA_FETCH_PROGRESS=' + phase] + [str(n) for n in counts]), flush=True)


def download(base, item, cache, progress=None):
    path = cache/item['sha256']
    if path.exists():
        if progress: progress('cached', item['bytes'])
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
        if progress: progress('download', 0, item['bytes'])
        with os.fdopen(fd,'wb') as dst, urllib.request.build_opener(HttpsOnly()).open(url,timeout=60) as src:
            count, h = 0, hashlib.sha256()
            for b in iter(lambda: src.read(1024 * 1024),b''):
                count += len(b)
                require(count <= item['bytes'], 'Archive exceeds signed size')
                dst.write(b)
                h.update(b)
                # Each count is out before the next read, which may block: none waits on a chunk.
                if progress: progress('download', count, item['bytes'])
        require(count == item['bytes'] and h.hexdigest() == item['sha256'], 'Archive size/SHA-256 mismatch; Docker was not changed')
        os.replace(temp,path)
        return path
    finally:
        Path(temp).unlink(missing_ok=True)


def archive_identity(path, image):
    """Read bounded config/manifest entries; never extract files onto the host. Returns the image
    IDs `docker load` may give this image.

    Both `docker save` layouts are accepted: the legacy one (config at <id>.json) and the OCI one
    Docker 25+ writes (config at blobs/sha256/<id>, plus an index.json). The OCI index carries its
    own image-name annotations, which `docker load` also tags by, so they are held to the same
    single signed version tag as manifest.json's RepoTags.

    The ID depends on the daemon's image store, not on the image: the classic store names an image
    by its config digest (the signed image_id), the containerd store, the default on a fresh Docker
    29 install, by the manifest digest the index points at. Each manifest the index names must be
    in this archive, hash to its digest and carry the signed config, so either ID is the signed
    image: the archive's bytes are the signed ones, and this ties the manifest to its config."""
    digest = image['image_id'][7:]
    configs = (digest + '.json', 'blobs/sha256/' + digest)
    with tarfile.open(path,'r:*') as t:
        names = set()
        manifest = index = config = None
        blobs, kept = {}, 0  # small blobs: the index's manifests are among them
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
            elif member.isfile() and p.parent == PurePosixPath('blobs/sha256') and \
                    member.size <= 64 * 1024 and kept < 16 * 1024 * 1024:
                blobs[p.name] = t.extractfile(member).read()
                kept += member.size
        require(manifest and len(manifest) == 1 and manifest[0]['Config'] in configs and config,
                'Archive does not contain the signed image identity')
        require(manifest[0]['RepoTags'] == [image['version_tag']], 'Archive attempts to change unexpected Docker tags')
        repo, _, ref = image['version_tag'].rpartition(':')
        ids = {image['image_id']}
        for m in (index or {}).get('manifests', []):
            a = m.get('annotations') or {}
            named = a.get('io.containerd.image.name')
            require(named in (None, image['version_tag'], 'docker.io/library/' + image['version_tag'])
                    and a.get('org.opencontainers.image.ref.name') in (None, ref),
                    'Archive index names an unexpected Docker tag')
            md = m.get('digest') or ''
            data = blobs.get(md[7:]) if re.fullmatch(r'sha256:[0-9a-f]{64}', md) else None
            require(data is not None and hashlib.sha256(data).hexdigest() == md[7:]
                    and (json.loads(data).get('config') or {}).get('digest') == image['image_id'],
                    'Archive index names a manifest other than the signed image')
            ids.add(md)
        require(config['architecture'] == 'amd64' and config['os'] == 'linux', 'Wrong Docker platform')
        labels = config.get('config',{}).get('Labels') or {}
        require(labels.get('org.remora.play.local_only') is None,
                'Private Play overlays cannot be public images')
        require(labels.get('remora.public_release') == 'true',
                'Image is not a public_release build')
    return ids


def fetch(base, image, catalog_state, cache, progress=None):
    check_local_docker()
    require(re.fullmatch(r'remora24:x86_64-public-[A-Za-z0-9_.-]+', image['version_tag']), 'Invalid immutable version tag')
    old_version = image_id(image['version_tag'])
    archive = download(base,image['archive'],cache,progress)
    if progress: progress('check')
    ids = archive_identity(archive,image)
    require(old_version in {None} | ids, 'Immutable version tag already names different bytes')
    previous = image_id(image['tag'])
    # All verification precedes docker load. The archive only carries the immutable version tag.
    if progress: progress('import')
    print('Importing authenticated image…', flush=True)
    run('docker','load','-i',str(archive))
    # From here on the image is named by the ID this daemon gave it (see archive_identity).
    local = image_id(image['version_tag'])
    require(local in ids, 'Imported image ID differs from signed metadata')
    backup = None
    if previous and previous != local:
        backup = 'remora-rollback:public-' + previous[7:]
        require(image_id(backup) in (None,previous), 'Rollback tag conflicts with an existing image')
        run('docker','tag',previous,backup)
    receipt = {'tag':image['tag'],'image':local,'signed_image_id':image['image_id'],
               'previous':previous,'backup':backup,'catalog':catalog_state,
               'archive':image['archive'],'source':base,'data_unchanged':True}
    receipt_path = cache/(image['tag'].replace(':','_')+'.receipt.json')
    # Retain the original rollback boundary on an idempotent repeat.
    if previous == local and receipt_path.exists():
        receipt = json.loads(receipt_path.read_text())
    prepared = receipt_path.with_suffix('.pending.json')
    atomic_json(prepared,receipt)
    try:
        run('docker','tag',local,image['tag'])
        os.replace(prepared,receipt_path)
    except BaseException:
        if previous:
            run('docker','tag',previous,image['tag'])
        elif image_id(image['tag']) == local:
            run('docker','image','rm','--no-prune',image['tag'])
        raise
    # Imported and promoted, so this fetch's archive has done its job; until here it stays, so a
    # failed import retries from it. Only its own digest-named entry goes, and failing to remove it
    # is not a failed import: the image is already in Docker.
    try:
        archive.unlink(missing_ok=True)
    except OSError as e:
        print('Could not remove the imported archive ' + image['archive']['file'] + ': ' + str(e),
              file=sys.stderr, flush=True)
    # Then what earlier updates left (release_prune). Every path that imports a release ends here,
    # so none of them leaves 3.5 GB behind per release; and a cleanup that cannot decide, as after
    # a first install or a rollback, or that fails, leaves everything and fails nothing.
    try:
        cleanup = release_cleanup(image['tag'], cache)
    except (VerificationError, OSError, ValueError, KeyError) as e:
        cleanup = {'removed': [], 'stopped': str(e)}
    if cleanup['removed']:
        print('Removed what an earlier update left: ' + ', '.join(cleanup['removed']), flush=True)
    return receipt | {'receipt':str(receipt_path), 'cleanup':cleanup}


def installed_version(image, installed):
    """The release stamp of the image this host runs under the catalog's tag: its own immutable
    version name when it has one, else None (a local build that was never fetched)."""
    tags = json.loads(run('docker', 'image', 'inspect', '--format', '{{json .RepoTags}}', installed))
    prefix = image['tag'] + '-'
    stamps = [t[len(prefix):] for t in tags or []
              if t.startswith(prefix) and re.fullmatch(r'\d{8}\.\d{6}', t[len(prefix):])]
    return max(stamps) if stamps else None


def stamp_time(stamp):
    return dt.datetime.strptime(stamp, '%Y%m%d.%H%M%S').replace(tzinfo=dt.timezone.utc)


def check(image):
    """Whether the authenticated catalog's release is newer than what this host has under its tag.
    Nothing is downloaded. A different image is not enough: a host that built a newer image
    itself must not be offered an older release as an update."""
    check_local_docker()
    stamp = image['version_tag'][len(image['tag']) + 1:]
    installed = image_id(image['tag'])
    have = image_id(image['version_tag'])
    current = installed is not None and installed in {image['image_id'], have}
    if installed is None:
        update = True
    elif current:
        update = False
    elif not re.fullmatch(r'\d{8}\.\d{6}', stamp):
        update = True  # an unreadable stamp cannot be ranked; offering it leaves the choice to the user
    else:
        mine = installed_version(image, installed)
        if mine:
            update = stamp > mine
        else:
            # Never fetched, so no stamp to rank by: the image's creation time stands in. Assembly
            # follows the build that stamped it, so a release is newer only if it was stamped after
            # this image was put together.
            created = run('docker', 'image', 'inspect', '--format', '{{.Created}}', installed).strip()
            update = stamp_time(stamp) > dt.datetime.fromisoformat(created[:19] + '+00:00')
    return {'tag': image['tag'], 'version_tag': image['version_tag'], 'version': stamp,
            'installed': installed, 'current': current, 'update': update,
            'bytes': image['archive']['bytes'], 'fingerprint': image.get('fingerprint', '')}


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


RECEIPT_RETENTION_FIELDS = ('tag', 'image', 'signed_image_id', 'previous', 'backup')


def release_retention_receipts(tag, receipts):
    """Validate parsed verified-import receipts, as fetch writes them, and name the image IDs they
    hold for rollback. Pure: no Docker, no files. `receipts` is every channel's parsed receipt, one
    per tag, since rollback tags are shared across channels.

    Returns {'complete': True, 'reason': None, 'retain': frozenset of image IDs} — each receipt's
    current image, its signed image ID and its non-null previous image — or {'complete': False,
    'reason': <why>, 'retain': None}. A receipt must carry every field in RECEIPT_RETENTION_FIELDS:
    a missing previous or backup is unknown rollback state, never the first-install null. Passing
    is only a precondition: it names no tag to drop, matches no live inventory and does not know
    which images a container uses."""
    def incomplete(reason):
        return {'complete': False, 'reason': reason, 'retain': None}
    tag_pattern = r'remora24:x86_64-public(?:-[A-Za-z0-9_.-]+)?'
    is_id = lambda v: type(v) is str and re.fullmatch(r'sha256:[0-9a-f]{64}', v) is not None
    if type(tag) is not str or not re.fullmatch(tag_pattern, tag):
        return incomplete('Invalid public image tag')
    if type(receipts) not in (list, tuple):
        return incomplete('Receipts are not a list')
    seen, retain = set(), set()
    for r in receipts:
        if type(r) is not dict:
            return incomplete('A receipt is not an object')
        missing = [k for k in RECEIPT_RETENTION_FIELDS if k not in r]
        if missing:
            return incomplete('A receipt lacks ' + ', '.join(missing))
        t = r['tag']
        if type(t) is not str or not re.fullmatch(tag_pattern, t):
            return incomplete('A receipt names an invalid tag')
        if t in seen:
            return incomplete('More than one receipt for ' + t)
        seen.add(t)
        if not is_id(r['image']) or not is_id(r['signed_image_id']):
            return incomplete('Receipt for ' + t + ' lacks a valid image ID')
        previous, backup = r['previous'], r['backup']
        # fetch's three shapes: a first install (both null), an import over the same image (that
        # image as previous, no backup), and a replacement (the backup tag named by previous).
        if previous is None:
            expected = None
        elif not is_id(previous):
            return incomplete('Receipt for ' + t + ' has a malformed previous image')
        elif previous == r['image']:
            expected = None
        else:
            expected = 'remora-rollback:public-' + previous[7:]
        if backup != expected:
            return incomplete('Receipt for ' + t + ' has a backup that contradicts its previous image')
        retain |= {r['image'], r['signed_image_id']} | ({previous} if previous else set())
    if tag not in seen:
        return incomplete('No verified-import receipt for ' + tag)
    return {'complete': True, 'reason': None, 'retain': frozenset(retain)}


def obsolete_release_tags(tag, receipts, images, containers):
    """Name the tags release updates left behind that a prune may drop: the rollback alias fetch
    gave an image it replaced, with that same image's immutable version tags, once no receipt
    holds the image and no container uses it. Pure: it decides over what it is given and removes
    nothing; untagging alone frees no layer and no builder cache.

    `receipts` is what release_retention_receipts takes, which this calls first. `images` is the
    whole image inventory as `docker image inspect` gives it ({'Id': <full image ID>, 'RepoTags':
    [...]}), the daemon's own IDs, which are the ones fetch records; `containers` is every
    container, running or stopped, as `docker inspect` gives it ({'Id': <64 hex>, 'Image': <full
    image ID>}). None is an inventory nobody collected, never an empty one.

    Stricter than the preflight, for cleanup only: every receipt must record the image it replaced
    and its backup alias, and the inventory must hold the active tag on the receipt's image and the
    backup alias on the replaced one. A null previous or backup (a first install, an import over
    the same image) stands cleanup down rather than becoming a default a rollback tag could match.
    Every remora-rollback: tag must be its own image's remora-rollback:public-<hex>. An image is
    selectable only if it carries that alias of its own, so fetch replaced it, and no tag but
    receipt channels' <channel>-<stamp> version tags: a version-only image may be a release being
    staged, and any other tag is someone's hold on the whole image.

    Returns {'complete': True, 'reason': None, 'tags': sorted tuple of tags, 'retain': sorted tuple
    of the image IDs held: the receipts', the containers' and every tagged image not selected}, or
    {'complete': False, 'reason': <why>, 'tags': (), 'retain': None}."""
    def incomplete(reason):
        return {'complete': False, 'reason': reason, 'tags': (), 'retain': None}
    held = release_retention_receipts(tag, receipts)
    if not held['complete']:
        return incomplete(held['reason'])
    is_id = lambda v: type(v) is str and re.fullmatch(r'sha256:[0-9a-f]{64}', v) is not None
    alias = lambda iid: 'remora-rollback:public-' + iid[7:]
    if type(images) not in (list, tuple):
        return incomplete('No image inventory')
    if type(containers) not in (list, tuple):
        return incomplete('No container inventory')
    owner, tags_of = {}, {}
    for im in images:
        if type(im) is not dict or not is_id(im.get('Id')) or type(im.get('RepoTags')) is not list \
                or not all(type(t) is str and t for t in im['RepoTags']):
            return incomplete('An image inventory entry is malformed')
        iid = im['Id']
        if iid in tags_of:
            return incomplete('The image inventory lists ' + iid + ' twice')
        tags_of[iid] = set(im['RepoTags'])
        for t in im['RepoTags']:
            if t in owner:
                return incomplete('The image inventory gives ' + t + ' to more than one image')
            owner[t] = iid
            if t.startswith('remora-rollback:') and t != alias(iid):
                return incomplete('Rollback tag ' + t + ' is not the alias of the image it names')
    used, seen = set(), set()
    for c in containers:
        if type(c) is not dict or type(c.get('Id')) is not str \
                or not re.fullmatch(r'[0-9a-f]{64}', c['Id']) or not is_id(c.get('Image')):
            return incomplete('A container inventory entry is malformed')
        if c['Id'] in seen:
            return incomplete('The container inventory lists ' + c['Id'] + ' twice')
        seen.add(c['Id'])
        used.add(c['Image'])
    channels = set()
    for r in receipts:
        if r['previous'] is None or r['backup'] is None:
            return incomplete('Receipt for ' + r['tag'] + ' records no replaced image')
        if r['tag'] not in owner or owner[r['tag']] != r['image']:
            return incomplete('Tag ' + r['tag'] + ' is absent or not on its receipt\'s image')
        if r['backup'] not in owner or owner[r['backup']] != r['previous']:
            return incomplete('Rollback tag ' + r['backup'] + ' is absent or not on its receipt\'s image')
        channels.add(r['tag'])

    def version_tag(t):
        channel, dash, stamp = t.rpartition('-')
        return bool(dash) and channel in channels and re.fullmatch(r'\d{8}\.\d{6}', stamp) is not None
    retain, candidates = set(held['retain']) | used, []
    for iid in sorted(tags_of):
        tags = tags_of[iid]
        if iid in retain or not tags:
            continue
        if alias(iid) in tags and all(t == alias(iid) or version_tag(t) for t in tags):
            candidates += tags
        else:
            retain.add(iid)
    return {'complete': True, 'reason': None, 'tags': tuple(sorted(candidates)),
            'retain': tuple(sorted(retain))}


# The labels that name what an image was built on, besides its Parent: a Google Play layer's base
# (vendor/play/install.py) and the release under a stack of local add-on layers.
LINEAGE_LABELS = ('org.remora.play.base', 'org.remora.addons.root')


def release_prune(tag, receipts, images, containers, configs):
    """What a prune after an update removes, in the order to remove it: the Google Play layers built
    on a release obsolete_release_tags selects, then that release's own tags. Pure, like the
    selector it builds on.

    `configs` gives every image in `images` as image inspect does ({'Id', 'Parent', 'Labels'}).
    What an image is built on is its Parent and the images its LINEAGE_LABELS name, and whatever a
    kept image is built on is kept, transitively: an add-on layer still on an old release keeps it
    and the Play layer between them, as a container's image does (the selector counts only the
    image a container names). A Play layer goes only with its release, unused, and carrying no tag
    but its own remora-local:play- ones. An untagged image holds nothing: removing the image it was
    built toward prunes it too.

    Returns the selector's result with 'remove': ((tag, image ID), ...), Play layers first, its
    'tags' cut to the releases still removed and its 'retain' grown by those lineage keeps; or the
    selector's incomplete answer with 'remove': ()."""
    def incomplete(reason):
        return {'complete': False, 'reason': reason, 'tags': (), 'retain': None, 'remove': ()}
    decision = obsolete_release_tags(tag, receipts, images, containers)
    if not decision['complete']:
        return decision | {'remove': ()}
    if type(configs) not in (list, tuple):
        return incomplete('No image lineage')
    tags_of = {im['Id']: set(im['RepoTags']) for im in images}
    base_of, play_base = {}, {}
    for cfg in configs:
        labels = cfg.get('Labels') if type(cfg) is dict else None
        if type(cfg) is not dict or cfg.get('Id') not in tags_of or cfg['Id'] in base_of \
                or type(cfg.get('Parent')) is not str or type(labels or {}) is not dict \
                or not all(type(v) is str for v in (labels or {}).values()):
            return incomplete('An image lineage entry is malformed')
        labels = labels or {}
        base_of[cfg['Id']] = ({cfg['Parent']} if cfg['Parent'] else set()) \
            | {labels[k] for k in LINEAGE_LABELS if labels.get(k)}
        if labels.get('org.remora.play.local_only') == 'true' and labels.get('org.remora.play.base'):
            play_base[cfg['Id']] = labels['org.remora.play.base']
    if set(base_of) != set(tags_of):
        return incomplete('The image lineage does not cover the image inventory')
    used = {c['Image'] for c in containers}
    selected = set(decision['tags'])
    releases = {iid for iid, tags in tags_of.items() if tags and tags <= selected}
    layers = {iid for iid, base in play_base.items()
              if base in releases and iid not in used and tags_of[iid]
              and all(t.startswith('remora-local:play-') for t in tags_of[iid])}
    drop = releases | layers
    while True:
        holders = {iid for iid, tags in tags_of.items() if tags and iid not in drop} | used
        held, todo = set(), list(holders)
        while todo:
            for b in base_of.get(todo.pop(), ()):
                if b not in held:
                    held.add(b)
                    todo.append(b)
        kept = drop & (held | holders)
        if not kept:
            break
        drop -= kept
    remove = [(t, iid) for group in (layers, releases) for iid in sorted(drop & group)
              for t in sorted(tags_of[iid])]
    return decision | {'tags': tuple(sorted(t for iid in drop & releases for t in tags_of[iid])),
                       'retain': tuple(sorted(set(decision['retain']) | (releases - drop))),
                       'remove': tuple(remove)}


def read_release_receipts(cache):
    if not cache.exists():
        return []
    receipts = []
    for path in sorted(cache.iterdir()):
        require(not path.name.endswith('.receipt.pending.json'),
                'An image import receipt is pending: ' + path.name)
        if not path.name.endswith('.receipt.json'):
            continue
        r = json.loads(path.read_text())
        require(type(r) is dict and type(r.get('tag')) is str
                and path.name == r['tag'].replace(':', '_') + '.receipt.json',
                'Invalid release receipt: ' + path.name)
        receipts.append(r)
    return receipts


def cleanup_inventory(kind):
    """Collect only the identity fields the retention policy needs."""
    listed = run('docker', kind, 'ls', '--all', '--quiet', '--no-trunc').splitlines()
    image = kind == 'image'
    id_pattern = r'sha256:[0-9a-f]{64}' if image else r'[0-9a-f]{64}'
    require(all(re.fullmatch(id_pattern, iid) for iid in listed),
            'Invalid Docker ' + kind + ' inventory ID')
    # Image listings may repeat the ID for each tag of the same image.
    ids = sorted(set(listed))
    require(image or len(ids) == len(listed), 'Duplicate Docker container inventory ID')
    field = 'RepoTags' if image else 'Image'
    template = '{"Id":{{json .Id}},"' + field + '":{{json .' + field + '}}}'
    records = []
    for start in range(0, len(ids), 100):
        batch = ids[start:start + 100]
        out = run('docker', kind, 'inspect', '--format', template, *batch)
        rows = [json.loads(line) for line in out.splitlines()]
        require(len(rows) == len(batch) and all(type(row) is dict and type(row.get('Id')) is str
                                               and field in row for row in rows)
                and sorted(row['Id'] for row in rows) == batch,
                'Incomplete Docker ' + kind + ' inspection')
        if image:
            for row in rows:
                if row['RepoTags'] is None:  # The classic Docker store reports untagged images as null.
                    row['RepoTags'] = []
        records.extend(sorted(rows, key=lambda row: row['Id']))
    return records


def image_lineage(ids):
    """What release_prune reads of each image: its Parent and its labels, both part of the image's
    configuration, so they never change under the same ID. From the whole inspect record, not a
    --format template: Docker leaves Parent out of some images and Labels out of their Config, and
    a template stops at the first key a map lacks."""
    records = []
    for start in range(0, len(ids), 100):
        batch = ids[start:start + 100]
        rows = json.loads(run('docker', 'image', 'inspect', *batch))
        require(type(rows) is list and len(rows) == len(batch)
                and all(type(row) is dict and type(row.get('Config') or {}) is dict for row in rows)
                and sorted(row.get('Id') for row in rows) == sorted(batch),
                'Incomplete Docker image lineage')
        records.extend({'Id': row['Id'], 'Parent': row.get('Parent') or '',
                        'Labels': (row.get('Config') or {}).get('Labels')} for row in rows)
    return records


def release_cleanup_plan(tag, cache):
    receipts = read_release_receipts(cache)
    preflight = release_retention_receipts(tag, receipts)
    require(preflight['complete'], preflight['reason'])
    endpoint = check_local_docker()
    images = cleanup_inventory('image')
    containers = cleanup_inventory('container')
    decision = release_prune(tag, receipts, images, containers,
                             image_lineage([record['Id'] for record in images]))
    require(decision['complete'], decision['reason'])
    image_ids = {record['Id'] for record in images}
    require(all(record['Image'] in image_ids for record in containers),
            'A container image is absent from the Docker image inventory')
    require(cleanup_inventory('image') == images and cleanup_inventory('container') == containers
            and read_release_receipts(cache) == receipts and check_local_docker() == endpoint,
            'Docker inventory, context or release receipts changed during the cleanup plan')
    return decision | {'tag': tag, 'dry_run': True}


def release_cleanup(tag, cache):
    """Removes what the plan names (release_prune): each tag only while it still names the image
    the plan saw, and none after one does not. Docker deletes an image with its last tag, and with
    a Play layer the untagged build steps under it. Returns what was removed, and why it stopped
    if it did."""
    plan = release_cleanup_plan(tag, cache)
    removed = []
    try:
        for t, iid in plan['remove']:
            require(image_id(t) == iid, t + ' changed during the cleanup')
            run('docker', 'image', 'rm', t)
            removed.append(t)
    except VerificationError as e:
        return {'removed': removed, 'stopped': str(e)}
    return {'removed': removed, 'stopped': None}


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('action',choices=['list','check','fetch','rollback','cleanup-plan'])
    p.add_argument('--source',default='https://github.com/EtherAura/Remora/releases/download/android-images')
    p.add_argument('--tag')
    p.add_argument('--key',default=str(Path(__file__).with_name('image-signing-key.pem')))
    p.add_argument('--progress',action='store_true',help='fetch: print REMORA_FETCH_PROGRESS= lines')
    a = p.parse_args(argv)
    # Before the catalog is fetched: with no tag, check and fetch otherwise failed on the lookup
    # and blamed the catalog for lacking a tag nobody asked for.
    require(a.action == 'list' or a.tag, a.action.capitalize() + ' needs --tag')
    require(not a.progress or a.action == 'fetch', '--progress is only for fetch')
    cache = Path(os.environ.get('XDG_CACHE_HOME',str(Path.home()/'.cache')))/'remora'/'images'
    if a.action == 'cleanup-plan':
        result = release_cleanup_plan(a.tag, cache)
        print('REMORA_IMAGES_RESULT='+json.dumps(result,separators=(',',':')),flush=True)
        return 0
    u = urllib.parse.urlsplit(a.source)
    require(u.scheme == 'https' and u.netloc and not u.username and not u.password and not u.query and not u.fragment,
            'Source must be an HTTPS directory URL')
    base = a.source.rstrip('/')
    cache.mkdir(parents=True,exist_ok=True,mode=0o700)
    cache.chmod(0o700)
    with open(cache/'import.lock','a') as lock:
        fcntl.flock(lock,fcntl.LOCK_EX)
        if a.action == 'rollback':
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
                result = (check(selected[0]) if a.action == 'check'
                          else fetch(base,selected[0],state,cache,
                                     report_progress if a.progress else None))
    print('REMORA_IMAGES_RESULT='+json.dumps(result,separators=(',',':')),flush=True)
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (VerificationError, OSError, ValueError, KeyError, tarfile.TarError, KeyboardInterrupt) as e:
        print('Image verification/import failed: '+str(e),file=sys.stderr)
        sys.exit(1)
