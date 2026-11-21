"""Offline tests for the public image catalog verifier (bd remora-qksj). No network, no Docker daemon.

The acceptance line these pin: a corrupt, truncated, stale or unauthorized artifact fails safely,
and every check precedes `docker load`.
"""
import contextlib
import datetime as dt
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import tarfile
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('catalog', ROOT / 'vendor/release/catalog.py')
catalog = importlib.util.module_from_spec(spec)
spec.loader.exec_module(catalog)

TAG = 'remora24:x86_64-public'
VTAG = 'remora24:x86_64-public-20261113.012458'


def keypair(d):
    priv, pub = Path(d) / 'k.pem', Path(d) / 'k.pub'
    subprocess.run(['openssl', 'genpkey', '-algorithm', 'ED25519', '-out', str(priv)], check=True,
                   capture_output=True)
    subprocess.run(['openssl', 'pkey', '-in', str(priv), '-pubout', '-out', str(pub)], check=True,
                   capture_output=True)
    return priv, pub


def sign(priv, data, d):
    src, sig = Path(d) / 'm', Path(d) / 'm.sig'
    src.write_bytes(data)
    subprocess.run(['openssl', 'pkeyutl', '-sign', '-inkey', str(priv), '-rawin', '-in', str(src),
                    '-out', str(sig)], check=True)
    return sig.read_bytes()


def add(t, name, data):
    info = tarfile.TarInfo(name)
    info.size = len(data)
    t.addfile(info, io.BytesIO(data))


def docker_archive(path, labels=None, repo_tags=None, index_name=None, config_override=None,
                   legacy=False, manifest_config=None, manifest_missing=False):
    """A minimal `docker save` archive in the OCI layout Docker 25+ writes (or the legacy one).
    Returns the config digest; `.manifest_digest` holds the digest the index points at."""
    config = json.dumps({'architecture': 'amd64', 'os': 'linux', 'config': {'Labels': (
        {'remora.public_release': 'true'} if labels is None else labels)},
        'rootfs': {'type': 'layers', 'diff_ids': []}}).encode()
    digest = hashlib.sha256(config).hexdigest()
    cname = digest + '.json' if legacy else 'blobs/sha256/' + digest
    manifest = json.dumps({'schemaVersion': 2, 'config': {
        'digest': manifest_config or 'sha256:' + digest, 'size': len(config)}, 'layers': []}).encode()
    mdigest = hashlib.sha256(manifest).hexdigest()
    with tarfile.open(path, 'w') as t:
        add(t, cname, config_override or config)
        if not legacy and not manifest_missing:
            add(t, 'blobs/sha256/' + mdigest, manifest)
        add(t, 'manifest.json', json.dumps([{'Config': cname, 'Layers': [],
                                             'RepoTags': repo_tags or [VTAG]}]).encode())
        if not legacy:
            add(t, 'index.json', json.dumps({'manifests': [{'digest': 'sha256:' + mdigest,
                'annotations': {
                    'io.containerd.image.name': index_name or 'docker.io/library/' + VTAG,
                    'org.opencontainers.image.ref.name': VTAG.split(':')[1]}}]}).encode())
    docker_archive.manifest_digest = 'sha256:' + mdigest
    return 'sha256:' + digest


def ref(name, data=b'x'):
    return {'file': name, 'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}


def image_entry(image_id, archive_ref):
    return {'tag': TAG, 'version_tag': VTAG, 'image_id': image_id, 'archive': archive_ref,
            'sbom': ref('a.spdx.json'), 'notices': ref('a-notices.tar'),
            'audit': ref('a-audit.json.gz'), 'provenance': ref('a-provenance.json'),
            'sources': [ref('a-src-remora.tar.xz')], 'android_sdk': 37, 'abi': ['x86_64'],
            'arch_variant': 'x86_64', 'public_audit_passed': True, 'google_payloads': False}


def index(images, sequence=1, published=None, days=90):
    published = published or dt.datetime.now(dt.timezone.utc).replace(microsecond=0)
    return json.dumps({'schema': 1, 'channel': 'remora-public-android', 'sequence': sequence,
                       'published_at': published.isoformat(),
                       'expires_at': (published + dt.timedelta(days=days)).isoformat(),
                       'images': images}).encode()


class Catalog(unittest.TestCase):
    def setUp(self):
        self.d = tempfile.TemporaryDirectory()
        self.priv, self.pub = keypair(self.d.name)
        self.entry = image_entry('sha256:' + '1' * 64, ref('a.tar.xz'))

    def tearDown(self):
        self.d.cleanup()

    def verify(self, data, sig=None, key=None, last=None):
        return catalog.verify_catalog(data, sig or sign(self.priv, data, self.d.name),
                                      key or self.pub, last)

    def test_valid_catalog_verifies(self):
        m, state = self.verify(index([self.entry]))
        self.assertEqual(m['images'][0]['tag'], TAG)
        self.assertEqual(state['sequence'], 1)

    def test_tampered_or_foreign_signature_fails(self):
        data = index([self.entry])
        sig = sign(self.priv, data, self.d.name)
        with self.assertRaises(catalog.VerificationError):
            catalog.verify_catalog(data.replace(b'"sequence": 1', b'"sequence": 2'), sig, self.pub)
        other = Path(self.d.name) / 'other'
        other.mkdir()
        _, other_pub = keypair(other)
        with self.assertRaises(catalog.VerificationError):
            catalog.verify_catalog(data, sig, other_pub)

    def test_expired_future_and_overlong_catalogs_fail(self):
        old = dt.datetime.now(dt.timezone.utc) - dt.timedelta(days=100)
        for data in (index([self.entry], published=old),
                     index([self.entry], published=dt.datetime.now(dt.timezone.utc)
                           + dt.timedelta(days=2)),
                     index([self.entry], days=200)):
            with self.assertRaises(catalog.VerificationError):
                self.verify(data)

    def test_stale_or_rewritten_sequence_fails(self):
        _, state = self.verify(index([self.entry], sequence=5))
        with self.assertRaises(catalog.VerificationError):
            self.verify(index([self.entry], sequence=4), last=state)
        with self.assertRaises(catalog.VerificationError):  # same sequence, different bytes
            self.verify(index([self.entry], sequence=5, days=89), last=state)

    def test_unauthorized_images_fail(self):
        for change in ({'tag': 'remora24:x86_64-gapps'}, {'google_payloads': True},
                       {'public_audit_passed': False}, {'abi': ['x86_64', 'arm64-v8a']},
                       {'arch_variant': 'alderlake'},
                       {'archive': ref('../escape.tar.xz')},
                       {'archive': {'file': 'a.tar.xz', 'bytes': 3 * 1024**3,
                                    'sha256': '0' * 64}}):
            with self.subTest(change=change), self.assertRaises(catalog.VerificationError):
                self.verify(index([self.entry | change]))


class Check(unittest.TestCase):
    """`check` (the in-Android updater's question): is the catalog's release newer than what
    this host runs under the tag? Nothing is downloaded, and a host that built something newer
    itself must not be offered an older release."""
    SIGNED = 'sha256:' + '1' * 64
    LOCAL = 'sha256:' + '2' * 64

    def ask(self, ids, tags=(), created='2026-11-13T06:39:39.1Z'):
        def run(*argv):
            return json.dumps(list(tags)) if '{{json .RepoTags}}' in argv else created
        with patch.object(catalog, 'check_local_docker', lambda: None), \
                patch.object(catalog, 'image_id', lambda r: ids.get(r)), \
                patch.object(catalog, 'run', run):
            return catalog.check(image_entry(self.SIGNED, ref('a.tar.xz')))

    def test_the_signed_image_is_current(self):
        r = self.ask({TAG: self.SIGNED})
        self.assertEqual((r['current'], r['update'], r['version']), (True, False, '20261113.012458'))

    def test_containerd_names_the_same_release_by_another_id(self):
        # The containerd store gives a loaded image its manifest digest; the version name the
        # fetch imported under points at the same local ID, so the release is still current.
        r = self.ask({TAG: self.LOCAL, VTAG: self.LOCAL})
        self.assertEqual((r['current'], r['update']), (True, False))

    def test_a_newer_release_is_offered_and_an_older_one_is_not(self):
        older = TAG + '-20261101.000000'
        newer = TAG + '-20261201.000000'
        self.assertTrue(self.ask({TAG: self.LOCAL}, tags=[TAG, older])['update'])
        self.assertFalse(self.ask({TAG: self.LOCAL}, tags=[TAG, newer])['update'])

    def test_a_local_build_is_ranked_by_when_it_was_made(self):
        # No version name (never fetched): the release counts as newer only when it was stamped
        # after this image was assembled.
        self.assertTrue(self.ask({TAG: self.LOCAL}, created='2026-11-12T00:00:00Z')['update'])
        self.assertFalse(self.ask({TAG: self.LOCAL}, created='2026-11-14T00:00:00Z')['update'])

    def test_nothing_installed_is_an_update(self):
        r = self.ask({})
        self.assertEqual((r['installed'], r['current'], r['update']), (None, False, True))
        self.assertEqual(r['bytes'], 1)

    def test_a_missing_tag_is_named_before_the_catalog_is_fetched(self):
        # Checked after the download, a missing tag read as one the catalog lacked.
        def get(*a):
            raise AssertionError('the catalog was fetched')
        with tempfile.TemporaryDirectory() as d, patch.dict('os.environ', {'XDG_CACHE_HOME': d}), \
                patch.object(catalog, 'get', get):
            for action in ('check', 'fetch', 'rollback'):
                with self.subTest(action=action), self.assertRaisesRegex(
                        catalog.VerificationError, '^' + action.capitalize() + ' needs --tag$'):
                    catalog.main([action])


class Archive(unittest.TestCase):
    def setUp(self):
        self.d = tempfile.TemporaryDirectory()
        self.path = Path(self.d.name) / 'a.tar'

    def tearDown(self):
        self.d.cleanup()

    def entry(self, image_id):
        return {'image_id': image_id, 'version_tag': VTAG}

    def test_oci_and_legacy_layouts_verify(self):
        for legacy in (False, True):
            with self.subTest(legacy=legacy):
                iid = docker_archive(self.path, legacy=legacy)
                ids = catalog.archive_identity(self.path, self.entry(iid))
                # The containerd image store names the image by the manifest the index points
                # at, the classic store by its config: both are the signed image.
                self.assertEqual(ids, {iid} if legacy else {iid, docker_archive.manifest_digest})

    def test_index_must_point_at_the_signed_image(self):
        other = 'sha256:' + hashlib.sha256(b'another image').hexdigest()
        for kw in (dict(manifest_config=other), dict(manifest_missing=True)):
            with self.subTest(**kw):
                iid = docker_archive(self.path, **kw)
                with self.assertRaises(catalog.VerificationError):
                    catalog.archive_identity(self.path, self.entry(iid))

    def test_wrong_identity_tags_and_labels_fail(self):
        cases = [
            dict(config_override=b'{"architecture":"amd64"}'),        # digest mismatch
            dict(repo_tags=[VTAG, 'remora24:x86_64-gapps']),            # extra tag
            dict(index_name='docker.io/library/ubuntu:latest'),       # OCI index re-tags
            dict(labels={}),                                           # not a public build
            dict(labels={'remora.public_release': 'true',
                         'org.remora.play.local_only': 'true'}),        # private Play overlay
        ]
        for kw in cases:
            with self.subTest(**{k: str(v)[:30] for k, v in kw.items()}):
                iid = docker_archive(self.path, **kw)
                if 'config_override' in kw:
                    iid = 'sha256:' + hashlib.sha256(b'other').hexdigest()
                with self.assertRaises(catalog.VerificationError):
                    catalog.archive_identity(self.path, self.entry(iid))

    def test_links_and_traversal_fail(self):
        iid = docker_archive(self.path)
        with tarfile.open(self.path, 'a') as t:
            link = tarfile.TarInfo('blobs/sha256/evil')
            link.type, link.linkname = tarfile.SYMTYPE, '/etc/passwd'
            t.addfile(link)
        with self.assertRaises(catalog.VerificationError):
            catalog.archive_identity(self.path, self.entry(iid))
        iid = docker_archive(self.path)
        with tarfile.open(self.path, 'a') as t:
            add(t, '../outside', b'x')
        with self.assertRaises(catalog.VerificationError):
            catalog.archive_identity(self.path, self.entry(iid))


class Download(unittest.TestCase):
    def setUp(self):
        self.d = tempfile.TemporaryDirectory()
        self.cache = Path(self.d.name)
        self.body = b'archive-bytes' * 1000
        self.item = ref('a.tar.xz', self.body)

    def tearDown(self):
        self.d.cleanup()

    def serve(self, body):
        class Opener:
            def open(_, url, timeout):
                return io.BytesIO(body)
        return patch.object(catalog.urllib.request, 'build_opener', lambda *a: Opener())

    def test_good_download_lands_under_its_digest(self):
        with self.serve(self.body):
            p = catalog.download('https://h/x', self.item, self.cache)
        self.assertEqual(p.read_bytes(), self.body)
        self.assertEqual(p.name, self.item['sha256'])

    def test_truncated_corrupt_and_oversize_downloads_leave_nothing(self):
        for body in (self.body[:-1], self.body[:-1] + b'?', self.body + b'extra'):
            with self.subTest(n=len(body)), self.serve(body), \
                    self.assertRaises(catalog.VerificationError):
                catalog.download('https://h/x', self.item, self.cache)
            self.assertEqual(list(self.cache.iterdir()), [])

    def test_corrupt_cache_is_discarded_and_refetched(self):
        (self.cache / self.item['sha256']).write_bytes(b'rotted')
        with self.serve(self.body):
            p = catalog.download('https://h/x', self.item, self.cache)
        self.assertEqual(p.read_bytes(), self.body)

    def test_fetch_never_loads_an_unverified_archive(self):
        iid = docker_archive(self.cache / 'src.tar')
        good = (self.cache / 'src.tar').read_bytes()
        bad = bytearray(good)
        bad[-600] ^= 0xFF  # inside the tar body: the size matches, the digest cannot
        image = image_entry(iid, ref('a.tar', good))
        calls = []
        with self.serve(bytes(bad)), \
                patch.object(catalog, 'check_local_docker', lambda: None), \
                patch.object(catalog, 'image_id', lambda r: None), \
                patch.object(catalog, 'run', lambda *a: calls.append(a) or ''):
            with self.assertRaises(catalog.VerificationError):
                catalog.fetch('https://h/x', image, {'sequence': 1}, self.cache)
        self.assertFalse([c for c in calls if 'load' in c], calls)

    def test_fetch_tags_by_the_id_the_daemon_assigned(self):
        """A fresh Docker 29 install (containerd image store) names the loaded image by its
        manifest digest; the import must accept that and tag by it, not refuse."""
        iid = docker_archive(self.cache / 'src.tar')
        local = docker_archive.manifest_digest
        body = (self.cache / 'src.tar').read_bytes()
        image = image_entry(iid, ref('a.tar', body))
        loaded, calls = [], []
        ids = lambda r: local if loaded and r == VTAG else None

        def run(*a):
            calls.append(a)
            if 'load' in a:
                loaded.append(True)
            return ''
        with self.serve(body), \
                patch.object(catalog, 'check_local_docker', lambda: None), \
                patch.object(catalog, 'image_id', ids), patch.object(catalog, 'run', run):
            receipt = catalog.fetch('https://h/x', image, {'sequence': 1}, self.cache)
        self.assertIn(('docker', 'tag', local, TAG), calls)
        self.assertEqual((receipt['image'], receipt['signed_image_id']), (local, iid))


class FakeDocker:
    """Tags and the calls that move them, enough for fetch's import and promotion. A step named
    in `fail` exits non-zero the way `run` reports it."""
    def __init__(self, local, fail=()):
        self.tags, self.local, self.fail, self.calls = {}, local, set(fail), []

    def image_id(self, r):
        return self.tags.get(r)

    def run(self, *a):
        self.calls.append(a)
        if a[1] in self.fail:
            raise catalog.VerificationError('docker ' + a[1] + ' failed')
        if a[1] == 'load':
            self.tags[VTAG] = self.local
        elif a[1] == 'tag':
            self.tags[a[3]] = self.tags.get(a[2], a[2])
        elif a[1:3] == ('image', 'rm'):
            self.tags.pop(a[-1], None)
        return ''


class ImportCache(unittest.TestCase):
    """The downloaded archive is cache only until its image is imported and promoted: then the
    fetch releases it, whichever caller ran the fetch. Until then it stays, so a failed import can
    be retried from it, and nothing else in the cache is ever this fetch's to remove."""
    def setUp(self):
        self.d = tempfile.TemporaryDirectory()
        self.cache = Path(self.d.name) / 'images'
        self.cache.mkdir()
        src = Path(self.d.name) / 'src.tar'
        self.iid = docker_archive(src)
        self.body = src.read_bytes()
        self.image = image_entry(self.iid, ref('a.tar', self.body))
        self.archive = self.cache / self.image['archive']['sha256']
        # Another release's archive, the catalog state, another tag's receipt and the lock.
        self.others = {'f' * 64: b'another release', 'catalog-' + 'e' * 64 + '.json': b'{}',
                       'remora24_x86_64-public-other.receipt.json': b'{}', 'import.lock': b''}
        for name, data in self.others.items():
            (self.cache / name).write_bytes(data)
        self.downloads = 0

    def tearDown(self):
        self.d.cleanup()

    def fetch(self, docker, body=None):
        test = self

        class Opener:
            def open(_, url, timeout):
                test.downloads += 1
                return io.BytesIO(test.body if body is None else body)
        with patch.object(catalog.urllib.request, 'build_opener', lambda *a: Opener()), \
                patch.object(catalog, 'check_local_docker', lambda: None), \
                patch.object(catalog, 'image_id', docker.image_id), \
                patch.object(catalog, 'run', docker.run):
            return catalog.fetch('https://h/x', self.image, {'sequence': 1}, self.cache)

    def assertOthersKept(self):
        for name, data in self.others.items():
            self.assertEqual((self.cache / name).read_bytes(), data, name)

    def test_a_successful_import_releases_its_archive(self):
        docker = FakeDocker(self.iid)
        receipt = self.fetch(docker)
        self.assertEqual(docker.tags[TAG], self.iid)
        self.assertTrue(Path(receipt['receipt']).exists())
        self.assertFalse(self.archive.exists())
        self.assertOthersKept()

    def test_a_failed_verification_loads_nothing_and_caches_nothing(self):
        bad = bytearray(self.body)
        bad[-600] ^= 0xFF
        docker = FakeDocker(self.iid)
        with self.assertRaises(catalog.VerificationError):
            self.fetch(docker, bytes(bad))
        self.assertFalse([c for c in docker.calls if 'load' in c], docker.calls)
        self.assertFalse(self.archive.exists())
        self.assertOthersKept()

    def test_a_refused_identity_loads_nothing_and_leaves_the_cache_as_it_was(self):
        self.image['image_id'] = 'sha256:' + '2' * 64
        docker = FakeDocker(self.iid)
        with self.assertRaises(catalog.VerificationError):
            self.fetch(docker)
        self.assertFalse([c for c in docker.calls if 'load' in c], docker.calls)
        self.assertEqual(self.archive.read_bytes(), self.body)
        self.assertOthersKept()

    def test_a_failed_import_keeps_the_archive_for_the_retry(self):
        for step in ('load', 'tag'):
            with self.subTest(step=step):
                docker = FakeDocker(self.iid, fail={step})
                with self.assertRaises(catalog.VerificationError):
                    self.fetch(docker)
                self.assertNotIn(TAG, docker.tags)
                self.assertEqual(catalog.sha(self.archive), self.image['archive']['sha256'])
                # The retry imports from the kept archive without downloading it, then releases it.
                before = self.downloads
                docker.fail.clear()
                self.fetch(docker)
                self.assertEqual(self.downloads, before)
                self.assertEqual(docker.tags[TAG], self.iid)
                self.assertFalse(self.archive.exists())
        self.assertOthersKept()

    def test_a_repeated_fetch_succeeds_and_keeps_the_first_rollback_boundary(self):
        replaced = 'sha256:' + '9' * 64
        docker = FakeDocker(self.iid)
        docker.tags[TAG] = replaced
        first = self.fetch(docker)
        second = self.fetch(docker)
        # Released after the first import, so the repeat downloads and verifies it again.
        self.assertEqual(self.downloads, 2)
        self.assertFalse(self.archive.exists())
        self.assertEqual((first['previous'], second['previous']), (replaced, replaced))
        self.assertEqual(docker.tags['remora-rollback:public-' + '9' * 64], replaced)
        self.assertEqual(docker.tags[TAG], self.iid)
        self.assertOthersKept()

    def test_a_failure_to_release_the_archive_does_not_fail_the_import(self):
        real = Path.unlink

        def unlink(p, *a, **k):
            if p == self.archive:
                raise PermissionError('read-only cache')
            return real(p, *a, **k)
        docker = FakeDocker(self.iid)
        with patch.object(catalog.Path, 'unlink', unlink), \
                contextlib.redirect_stderr(io.StringIO()) as err:
            receipt = self.fetch(docker)
        self.assertEqual(docker.tags[TAG], self.iid)
        self.assertTrue(Path(receipt['receipt']).exists())
        self.assertIn('a.tar', err.getvalue())
        self.assertIn('read-only cache', err.getvalue())


PROGRESS = 'REMORA_FETCH_PROGRESS='


class Stdout:
    """A stdout that records what each flush made visible, so a test can tell a line written from
    one a reader could already have seen."""
    def __init__(self):
        self.text, self.visible = '', ''

    def write(self, s):
        self.text += s
        return len(s)

    def flush(self):
        self.visible = self.text

    def lines(self, flushed=True):
        return (self.visible if flushed else self.text).splitlines()

    def progress(self, flushed=True):
        return [l[len(PROGRESS):] for l in self.lines(flushed) if l.startswith(PROGRESS)]


class Body:
    """An HTTP body served in fixed chunks, then a read that stalls (raises) instead of ending.
    Before every read it records which progress lines were already flushed."""
    def __init__(self, chunks, out, stall=None):
        self.chunks, self.out, self.stall, self.seen = list(chunks), out, stall, []

    def __enter__(self):
        return self

    def __exit__(self, *a):
        return False

    def read(self, n):
        self.seen.append(self.out.progress())
        if self.chunks:
            return self.chunks.pop(0)
        if self.stall:
            raise self.stall
        return b''


class FetchProgress(unittest.TestCase):
    """`fetch --progress`: the helper's own progress events on stdout, one flushed line each, in
    the fixed REMORA_FETCH_PROGRESS= protocol. This only proves what the helper emits and when it
    is flushed; nothing here writes state.json, publishes over ssh or bounds how soon a consumer
    shows it. Without the flag the output is exactly what it was."""
    def setUp(self):
        self.d = tempfile.TemporaryDirectory()
        self.cache = Path(self.d.name) / 'images'
        self.cache.mkdir()
        src = Path(self.d.name) / 'src.tar'
        self.iid = docker_archive(src)
        self.body = src.read_bytes()
        self.image = image_entry(self.iid, ref('a.tar', self.body))
        self.archive = self.cache / self.image['archive']['sha256']
        self.size = len(self.body)
        self.opened = 0

    def tearDown(self):
        self.d.cleanup()

    def chunks(self, n=4):
        step = -(-self.size // n)
        return [self.body[i:i + step] for i in range(0, self.size, step)]

    def fetch(self, docker, out, body=None, progress=True, cache=None):
        test = self

        class Opener:
            def open(_, url, timeout):
                test.opened += 1
                return body if body is not None else Body(test.chunks(), out)
        with patch.object(catalog.urllib.request, 'build_opener', lambda *a: Opener()), \
                patch.object(catalog, 'check_local_docker', lambda: None), \
                patch.object(catalog, 'image_id', docker.image_id), \
                patch.object(catalog, 'run', docker.run), \
                patch.object(catalog.sys, 'stdout', out):
            kw = {'progress': catalog.report_progress} if progress else {}
            return catalog.fetch('https://h/x', self.image, {'sequence': 1}, cache or self.cache, **kw)

    def download_lines(self, counts):
        return ['download %d %d' % (c, self.size) for c in counts]

    def test_each_count_is_flushed_before_the_next_read_even_when_that_read_stalls(self):
        out = Stdout()
        burst = [self.body[:100], self.body[100:300], self.body[300:301]]
        body = Body(burst, out, stall=TimeoutError('the read stalled'))
        with self.assertRaisesRegex(TimeoutError, 'stalled'):
            self.fetch(FakeDocker(self.iid), out, body)
        # What a reader could see as each read began: the latest count, without another chunk.
        self.assertEqual([s[-1] for s in body.seen],
                         self.download_lines([0, 100, 300, 301]))
        # Nothing was written and left unflushed, and a failed download names no later phase.
        self.assertEqual(out.progress(), out.progress(flushed=False))
        self.assertEqual(out.progress(), self.download_lines([0, 100, 300, 301]))
        self.assertFalse(self.archive.exists())

    def test_a_successful_fetch_reports_download_check_and_import_in_order(self):
        out, docker = Stdout(), FakeDocker(self.iid)
        loaded_after = []
        run = docker.run

        def recording_run(*a):
            if a[1] == 'load':
                loaded_after.append(list(out.progress()))
            return run(*a)
        docker.run = recording_run
        receipt = self.fetch(docker, out)
        counts = [0] + [min(self.size, (i + 1) * len(self.chunks()[0])) for i in range(4)]
        self.assertEqual(out.progress(), self.download_lines(counts) + ['check', 'import'])
        # import is announced when it starts, before docker load runs, not after it.
        self.assertEqual(loaded_after[0][-1], 'import')
        self.assertEqual(out.progress(), out.progress(flushed=False))
        self.assertEqual(docker.tags[TAG], self.iid)
        self.assertTrue(Path(receipt['receipt']).exists())
        self.assertFalse(self.archive.exists())

    def test_check_is_announced_before_the_archive_is_inspected(self):
        out = Stdout()
        real = catalog.archive_identity
        seen = []

        def identity(*a):
            seen.append(list(out.progress()))
            return real(*a)
        with patch.object(catalog, 'archive_identity', identity):
            self.fetch(FakeDocker(self.iid), out)
        self.assertEqual(seen[0][-1], 'check')

    def test_without_the_flag_the_output_and_receipt_are_unchanged(self):
        plain, flagged = Stdout(), Stdout()
        first = self.fetch(FakeDocker(self.iid), plain, progress=False)
        self.assertEqual(plain.lines(flushed=False),
                         ['Downloading verified artifact a.tar', 'Importing authenticated image…'])
        self.assertFalse(self.archive.exists())
        other = Path(self.d.name) / 'other'
        other.mkdir()
        second = self.fetch(FakeDocker(self.iid), flagged, cache=other)
        # The flag only adds its own lines: the rest of the output and the receipt are the same.
        self.assertEqual([l for l in flagged.lines() if not l.startswith(PROGRESS)], plain.lines())
        self.assertEqual({k: v for k, v in first.items() if k != 'receipt'},
                         {k: v for k, v in second.items() if k != 'receipt'})
        self.assertEqual(json.loads(Path(first['receipt']).read_text()),
                         json.loads(Path(second['receipt']).read_text()))

    def test_a_cached_archive_is_not_reported_as_downloading(self):
        self.archive.write_bytes(self.body)
        out = Stdout()
        self.fetch(FakeDocker(self.iid), out)
        self.assertEqual(self.opened, 0)
        self.assertEqual(out.progress(), ['cached %d' % self.size, 'check', 'import'])

    def test_a_corrupt_cache_is_reported_then_downloaded_again(self):
        self.archive.write_bytes(b'rotted')
        out = Stdout()
        self.fetch(FakeDocker(self.iid), out)
        self.assertEqual(self.opened, 1)
        self.assertEqual(out.progress()[:2], ['cached %d' % self.size, 'download 0 %d' % self.size])
        self.assertEqual(out.progress()[-3:], ['download %d %d' % (self.size, self.size),
                                               'check', 'import'])

    def test_a_failed_phase_names_no_later_phase(self):
        bad = bytearray(self.body)
        bad[-600] ^= 0xFF
        cases = {
            # The digest of the whole body is wrong: refused at the end of the download.
            'download': (lambda: Body([bytes(bad)], out), {}, 'download %d %d' % (self.size, self.size)),
            # More bytes than the catalog signed: refused before the extra count is reported.
            'oversize': (lambda: Body([self.body, b'extra'], out), {}, 'download %d %d' % (self.size, self.size)),
            'check': (lambda: None, {'image_id': 'sha256:' + '2' * 64}, 'check'),
            'import': (lambda: None, {}, 'import'),
        }
        for name, (body, change, last) in cases.items():
            with self.subTest(phase=name):
                for p in self.cache.iterdir():
                    p.unlink()
                out = Stdout()
                self.image.update(change or {'image_id': self.iid})
                docker = FakeDocker(self.iid, fail={'load'} if name == 'import' else ())
                with self.assertRaises(catalog.VerificationError):
                    self.fetch(docker, out, body())
                self.assertEqual(out.progress()[-1], last)
                self.assertEqual(out.progress(), out.progress(flushed=False))
                self.assertTrue(all(int(n) <= self.size for l in out.progress() for n in l.split()[1:]))
                self.assertNotIn(TAG, docker.tags)

    def test_the_protocol_carries_only_known_phases_and_whole_counts(self):
        out = Stdout()
        with patch.object(catalog.sys, 'stdout', out):
            catalog.report_progress('download', 0, 5)
            catalog.report_progress('cached', 5)
            catalog.report_progress('check')
            catalog.report_progress('import')
            for bad in (('restart',), ('download', 1), ('download', 1, 2, 3), ('download', -1, 5),
                        ('download', 6, 5), ('download', True, 5), ('download', '1', 5),
                        ('download', 1.0, 5), ('cached', 0), ('check', 1), ('import\nrestart',),
                        ('Downloading https://h/x',)):
                with self.subTest(event=bad), self.assertRaises(catalog.VerificationError):
                    catalog.report_progress(*bad)
        self.assertEqual(out.lines(), [PROGRESS + 'download 0 5', PROGRESS + 'cached 5',
                                       PROGRESS + 'check', PROGRESS + 'import'])

    def test_the_flag_reaches_fetch_through_the_command_line(self):
        key = Path(self.d.name) / 'key.pem'
        key.write_bytes(b'not a real key; the catalog check is replaced')
        m = {'images': [self.image]}
        state = {'sequence': 1, 'sha256': '0' * 64}
        for flag in (True, False):
            with self.subTest(progress=flag):
                xdg = Path(self.d.name) / ('xdg-%s' % flag)
                out, docker = Stdout(), FakeDocker(self.iid)
                with patch.dict('os.environ', {'XDG_CACHE_HOME': str(xdg)}), \
                        patch.object(catalog, 'get', lambda url, limit: b''), \
                        patch.object(catalog, 'verify_catalog', lambda *a: (m, state)), \
                        patch.object(catalog.urllib.request, 'build_opener',
                                     lambda *a: type('O', (), {'open': lambda _, u, timeout: Body(self.chunks(), out)})()), \
                        patch.object(catalog, 'check_local_docker', lambda: None), \
                        patch.object(catalog, 'image_id', docker.image_id), \
                        patch.object(catalog, 'run', docker.run), \
                        patch.object(catalog.sys, 'stdout', out):
                    self.assertEqual(catalog.main(['fetch', '--tag', TAG, '--key', str(key)]
                                                  + (['--progress'] if flag else [])), 0)
                self.assertEqual(out.progress()[-2:] if flag else out.progress(),
                                 ['check', 'import'] if flag else [])
                self.assertTrue(out.lines()[-1].startswith('REMORA_IMAGES_RESULT='))
                self.assertEqual(docker.tags[TAG], self.iid)

    def test_the_flag_is_refused_outside_fetch(self):
        with tempfile.TemporaryDirectory() as d, patch.dict('os.environ', {'XDG_CACHE_HOME': d}), \
                patch.object(catalog, 'get', lambda *a: self.fail('the catalog was fetched')):
            for action in ('list', 'check', 'rollback'):
                with self.subTest(action=action), self.assertRaisesRegex(
                        catalog.VerificationError, '--progress'):
                    catalog.main([action, '--tag', TAG, '--progress'])


class ReleaseReceiptRetention(unittest.TestCase):
    """The receipt preflight a prune decision needs first: which images the receipts hold for
    rollback, or why that cannot be known. It selects no tag to drop; it only keeps a receipt
    with missing rollback fields from passing as a first install. The fixtures are the receipts
    fetch itself writes for releases A, B and C, imported in turn over the fake Docker."""
    @classmethod
    def setUpClass(cls):
        d = tempfile.TemporaryDirectory()
        cache = Path(d.name) / 'images'
        cache.mkdir()
        docker = FakeDocker(None)
        cls.ids, cls.receipts = {}, {}
        for name in 'ABC':
            src = Path(d.name) / (name + '.tar')
            iid = docker_archive(src, labels={'remora.public_release': 'true', 'n': name})
            body = src.read_bytes()
            image = image_entry(iid, ref(name + '.tar', body))
            # Each release has its own version tag; the fake only knows one, so it is cleared.
            docker.local = iid
            docker.tags.pop(VTAG, None)

            class Opener:
                def open(_, url, timeout):
                    return io.BytesIO(body)
            with patch.object(catalog.urllib.request, 'build_opener', lambda *a: Opener()), \
                    patch.object(catalog, 'check_local_docker', lambda: None), \
                    patch.object(catalog, 'image_id', docker.image_id), \
                    patch.object(catalog, 'run', docker.run):
                path = catalog.fetch('https://h/x', image, {'sequence': 1}, cache)['receipt']
            cls.ids[name] = iid
            cls.receipts[name] = json.loads(Path(path).read_text())
        d.cleanup()

    def check(self, receipts, tag=TAG):
        return catalog.release_retention_receipts(tag, receipts)

    def assertIncomplete(self, result):
        self.assertFalse(result['complete'], result)
        self.assertIsNone(result['retain'])
        self.assertTrue(result['reason'])

    def test_the_fixtures_are_the_receipts_fetch_writes(self):
        a, b, c = (self.receipts[n] for n in 'ABC')
        self.assertEqual((a['previous'], a['backup']), (None, None))
        self.assertEqual((b['previous'], b['backup']),
                         (self.ids['A'], 'remora-rollback:public-' + self.ids['A'][7:]))
        self.assertEqual((c['image'], c['previous']), (self.ids['C'], self.ids['B']))

    def test_missing_rollback_fields_are_unknown_not_a_first_install(self):
        complete = self.check([self.receipts['C']])
        self.assertTrue(complete['complete'], complete)
        self.assertIsNone(complete['reason'])
        self.assertEqual(complete['retain'], {self.ids['C'], self.ids['B']})
        for keys in (['previous'], ['backup'], ['previous', 'backup']):
            with self.subTest(missing=keys):
                c = {k: v for k, v in self.receipts['C'].items() if k not in keys}
                self.assertIncomplete(self.check([c]))

    def test_a_first_install_with_explicit_nulls_retains_its_image(self):
        result = self.check([self.receipts['A']])
        self.assertTrue(result['complete'], result)
        self.assertEqual(result['retain'], {self.ids['A']})

    def test_an_update_retains_the_image_it_replaced_and_the_signed_alias(self):
        local = 'sha256:' + '7' * 64  # the containerd store's manifest-digest name
        b = dict(self.receipts['B'], image=local)
        self.assertEqual(self.check([b])['retain'], {local, self.ids['B'], self.ids['A']})

    def test_an_import_over_the_same_image_records_it_as_previous_without_a_backup(self):
        same = dict(self.receipts['C'], previous=self.ids['C'], backup=None)
        self.assertEqual(self.check([same])['retain'], {self.ids['C']})

    def test_current_and_signed_ids_must_be_real_image_ids(self):
        for key in ('image', 'signed_image_id'):
            for bad in (None, '', 'f' * 64, 'sha256:' + 'F' * 64, 'sha256:' + 'f' * 63, 1):
                with self.subTest(key=key, value=bad):
                    self.assertIncomplete(self.check([dict(self.receipts['C'], **{key: bad})]))
            with self.subTest(key=key, value='missing'):
                c = {k: v for k, v in self.receipts['C'].items() if k != key}
                self.assertIncomplete(self.check([c]))

    def test_a_malformed_or_contradictory_rollback_pair_is_refused(self):
        b_backup = 'remora-rollback:public-' + self.ids['B'][7:]
        a_backup = 'remora-rollback:public-' + self.ids['A'][7:]
        for previous, backup in ((None, b_backup), (self.ids['B'], None), (self.ids['B'], a_backup),
                                 (self.ids['C'], b_backup), ('sha256:short', None),
                                 (self.ids['B'][7:], b_backup), (self.ids['B'], self.ids['B'])):
            with self.subTest(previous=previous, backup=backup):
                c = dict(self.receipts['C'], previous=previous, backup=backup)
                self.assertIncomplete(self.check([c]))

    def test_the_requested_channel_must_have_a_receipt(self):
        other = dict(self.receipts['C'], tag=TAG + '-beta')
        self.assertIncomplete(self.check([other]))
        self.assertIncomplete(self.check([]))
        self.assertIncomplete(self.check([self.receipts['C']], tag='remora24:other'))

    def test_every_channel_is_retained_because_rollback_tags_are_shared(self):
        other = dict(self.receipts['B'], tag=TAG + '-beta')
        result = self.check([self.receipts['C'], other])
        self.assertEqual(result['retain'], {self.ids['C'], self.ids['B'], self.ids['A']})
        # An invalid record on another channel makes the whole answer unknown.
        broken = {k: v for k, v in other.items() if k != 'backup'}
        self.assertIncomplete(self.check([self.receipts['C'], broken]))

    def test_duplicate_or_malformed_collections_are_refused(self):
        c = self.receipts['C']
        for receipts in ([c, c], [c, dict(c, previous=self.ids['A'],
                                          backup='remora-rollback:public-' + self.ids['A'][7:])],
                         {TAG: c}, c, None, 'receipts', [c, None], [c, [c]],
                         [dict(c, tag=None)], [dict(c, tag='other:tag')]):
            with self.subTest(receipts=receipts):
                self.assertIncomplete(self.check(receipts))

    def test_the_preflight_touches_no_docker_and_no_files(self):
        def refuse(*a, **k):
            raise AssertionError('impure call')
        with patch.object(catalog, 'run', refuse), patch.object(catalog, 'image_id', refuse), \
                patch.object(catalog.subprocess, 'run', refuse), patch('builtins.open', refuse), \
                patch.object(catalog.Path, 'read_text', refuse):
            self.assertTrue(self.check([self.receipts['C']])['complete'])


def rollback_alias(iid):
    return 'remora-rollback:public-' + iid[7:]


class ReleaseRetention(unittest.TestCase):
    """The offline decision of which release tags a prune may drop: the rollback alias and the
    version tags of an image fetch replaced, which no receipt and no container holds. The receipts
    and the tags are what fetch itself leaves after importing releases A, B and C in turn over the
    fake Docker, each under its own version tag; nothing is removed by the decision."""
    VERSIONS = {n: TAG + '-00000000.00000' + str(i) for i, n in enumerate('ABC', 1)}

    @classmethod
    def setUpClass(cls):
        d = tempfile.TemporaryDirectory()
        cache = Path(d.name) / 'images'
        cache.mkdir()
        docker = FakeDocker(None)
        cls.ids, cls.receipts = {}, {}
        for name in 'ABC':
            src = Path(d.name) / (name + '.tar')
            iid = docker_archive(src, labels={'remora.public_release': 'true', 'n': name})
            body = src.read_bytes()
            image = image_entry(iid, ref(name + '.tar', body))
            docker.local = iid

            class Opener:
                def open(_, url, timeout):
                    return io.BytesIO(body)
            with patch.object(catalog.urllib.request, 'build_opener', lambda *a: Opener()), \
                    patch.object(catalog, 'check_local_docker', lambda: None), \
                    patch.object(catalog, 'image_id', docker.image_id), \
                    patch.object(catalog, 'run', docker.run):
                path = catalog.fetch('https://h/x', image, {'sequence': 1}, cache)['receipt']
            # The fake loads every archive under the one fixture version tag; each release keeps
            # its own, as the real ones do.
            docker.tags[cls.VERSIONS[name]] = docker.tags.pop(VTAG)
            cls.ids[name] = iid
            cls.receipts[name] = json.loads(Path(path).read_text())
        cls.tags = dict(docker.tags)
        d.cleanup()

    def setUp(self):
        self.a_tags = (rollback_alias(self.ids['A']), self.VERSIONS['A'])
        self.other = 'sha256:' + 'd' * 64

    def inventory(self, tags=None, extra=()):
        by = {}
        for t, iid in (self.tags if tags is None else tags).items():
            by.setdefault(iid, []).append(t)
        return [{'Id': i, 'RepoTags': sorted(ts)} for i, ts in sorted(by.items())] + list(extra)

    def decide(self, receipts=None, images=None, containers=(), tag=TAG):
        receipts = [self.receipts['C']] if receipts is None else receipts
        images = self.inventory() if images is None else images
        return catalog.obsolete_release_tags(tag, receipts, images, list(containers))

    def assertIncomplete(self, result):
        self.assertFalse(result['complete'], result)
        self.assertEqual(result['tags'], ())
        self.assertIsNone(result['retain'])
        self.assertTrue(result['reason'])

    def test_the_fixtures_are_what_three_fetches_leave(self):
        a, b, c = (self.ids[n] for n in 'ABC')
        self.assertEqual(self.tags, {TAG: c, rollback_alias(a): a, rollback_alias(b): b,
                                     self.VERSIONS['A']: a, self.VERSIONS['B']: b,
                                     self.VERSIONS['C']: c})

    def test_only_the_release_two_updates_back_is_selected(self):
        result = self.decide()
        self.assertTrue(result['complete'], result)
        self.assertIsNone(result['reason'])
        self.assertEqual(result['tags'], tuple(sorted(self.a_tags)))
        # C is current and B is what images rollback restores; every tag of both stays.
        self.assertEqual(set(result['retain']), {self.ids['B'], self.ids['C']})

    def test_a_staged_version_only_image_is_never_selected(self):
        staged = {'Id': self.other, 'RepoTags': [TAG + '-00000000.000009']}
        result = self.decide(images=self.inventory(extra=[staged]))
        self.assertTrue(result['complete'], result)
        self.assertEqual(result['tags'], tuple(sorted(self.a_tags)))
        self.assertIn(self.other, result['retain'])

    def test_a_version_tag_without_its_images_own_alias_is_not_ownership(self):
        # A's version tag alone, its alias gone: nothing shows fetch replaced it.
        tags = {t: i for t, i in self.tags.items() if t != rollback_alias(self.ids['A'])}
        result = self.decide(images=self.inventory(tags))
        self.assertTrue(result['complete'], result)
        self.assertEqual(result['tags'], ())
        self.assertIn(self.ids['A'], result['retain'])

    def test_a_rollback_alias_on_the_wrong_image_stops_the_decision(self):
        e = 'sha256:' + 'e' * 64
        for where in (self.other, self.ids['C'], self.ids['B'], self.ids['A']):
            with self.subTest(image=where):
                tags = dict(self.tags, **{rollback_alias(e): where})
                self.assertIncomplete(self.decide(images=self.inventory(tags)))
        # Any tag in the reserved repository counts, not only the public- form.
        self.assertIncomplete(self.decide(images=self.inventory(
            dict(self.tags, **{'remora-rollback:latest': self.ids['C']}))))

    def test_a_foreign_tag_holds_the_whole_image(self):
        for foreign in ('user/keep:1', TAG + '-gamma-00000000.000007', TAG + '-custom',
                        'remora24:x86_64-public-0000.00'):
            with self.subTest(tag=foreign):
                result = self.decide(images=self.inventory(dict(self.tags, **{foreign: self.ids['A']})))
                self.assertTrue(result['complete'], result)
                self.assertEqual(result['tags'], ())
                self.assertIn(self.ids['A'], result['retain'])

    def test_a_container_running_or_stopped_holds_its_image(self):
        for running in (True, False):
            with self.subTest(running=running):
                c = {'Id': 'c' * 64, 'Image': self.ids['A'], 'State': {'Running': running}}
                result = self.decide(containers=[c])
                self.assertTrue(result['complete'], result)
                self.assertEqual(result['tags'], ())
                self.assertIn(self.ids['A'], result['retain'])

    def test_every_channel_and_the_signed_alias_are_retained(self):
        # Another channel that replaced A holds it, since rollback aliases are shared.
        beta = dict(self.receipts['B'], tag=TAG + '-beta')
        tags = dict(self.tags, **{TAG + '-beta': self.ids['B']})
        result = self.decide([self.receipts['C'], beta], self.inventory(tags))
        self.assertEqual((result['complete'], result['tags']), (True, ()), result)
        # Another channel at C: its version tags on A go with A's, being that channel's own.
        beta = dict(self.receipts['C'], tag=TAG + '-beta')
        tags = dict(self.tags, **{TAG + '-beta': self.ids['C'],
                                  TAG + '-beta-00000000.000007': self.ids['A']})
        result = self.decide([self.receipts['C'], beta], self.inventory(tags))
        self.assertEqual(result['tags'], tuple(sorted(self.a_tags + (TAG + '-beta-00000000.000007',))))
        # A receipt whose signed ID names A, as a containerd store's local ID can differ.
        signed = dict(self.receipts['C'], signed_image_id=self.ids['A'])
        result = self.decide([signed])
        self.assertEqual((result['complete'], result['tags']), (True, ()), result)
        self.assertIn(self.ids['A'], result['retain'])

    def test_the_current_image_and_its_active_tag_must_match_the_receipt(self):
        c = self.receipts['C']
        without_tag = {t: i for t, i in self.tags.items() if t != TAG}
        cases = {
            'null current, tag absent': ([dict(c, image=None)], self.inventory(without_tag)),
            'null current, tag present': ([dict(c, image=None)], self.inventory()),
            'missing current': ([{k: v for k, v in c.items() if k != 'image'}], self.inventory()),
            'malformed current': ([dict(c, image=self.ids['C'][7:])], self.inventory()),
            'active tag absent': ([c], self.inventory(without_tag)),
            'active tag on another image': ([c], self.inventory(dict(self.tags, **{TAG: self.ids['B']}))),
            'malformed signed ID': ([dict(c, signed_image_id='sha256:' + 'F' * 64)], self.inventory()),
            'null signed ID': ([dict(c, signed_image_id=None)], self.inventory()),
        }
        for name, (receipts, images) in cases.items():
            with self.subTest(name):
                self.assertIncomplete(self.decide(receipts, images))

    def test_missing_rollback_fields_stand_cleanup_down(self):
        self.assertIn(self.ids['B'], self.decide()['retain'])
        for keys in (['previous'], ['backup'], ['previous', 'backup']):
            with self.subTest(missing=keys):
                c = {k: v for k, v in self.receipts['C'].items() if k not in keys}
                self.assertIncomplete(self.decide([c]))

    def test_explicit_null_rollback_fields_stand_cleanup_down(self):
        c = self.receipts['C']
        # The preflight accepts both shapes as fetch writes them; cleanup does not.
        for previous, backup in ((None, None), (self.ids['C'], None)):
            with self.subTest(previous=previous, backup=backup):
                r = dict(c, previous=previous, backup=backup)
                self.assertTrue(catalog.release_retention_receipts(TAG, [r])['complete'])
                self.assertIncomplete(self.decide([r]))
        for previous, backup in ((None, rollback_alias(self.ids['B'])), (self.ids['B'], None)):
            with self.subTest(previous=previous, backup=backup):
                self.assertIncomplete(self.decide([dict(c, previous=previous, backup=backup)]))
        # On every channel, not only the requested one; A's own first-install receipt included.
        beta = dict(self.receipts['A'], tag=TAG + '-beta')
        tags = dict(self.tags, **{TAG + '-beta': self.ids['A']})
        self.assertIncomplete(self.decide([c, beta], self.inventory(tags)))
        self.assertIncomplete(self.decide([self.receipts['A']]))

    def test_the_backup_alias_must_be_on_the_replaced_image(self):
        b_alias = rollback_alias(self.ids['B'])
        self.assertIncomplete(self.decide(images=self.inventory(
            {t: i for t, i in self.tags.items() if t != b_alias})))

    def test_missing_inventory_is_not_an_empty_one(self):
        self.assertIncomplete(self.decide(images='inventory'))
        self.assertIncomplete(catalog.obsolete_release_tags(TAG, [self.receipts['C']], None, []))
        self.assertIncomplete(catalog.obsolete_release_tags(TAG, [self.receipts['C']],
                                                            self.inventory(), None))
        # An empty image inventory cannot hold the active tag; an empty container one is real.
        self.assertIncomplete(self.decide(images=[]))
        self.assertTrue(self.decide(containers=[])['complete'])

    def test_malformed_duplicate_or_ambiguous_inventory_is_refused(self):
        good = self.inventory()
        a = next(im for im in good if im['Id'] == self.ids['A'])
        for images in ({im['Id']: im for im in good}, good + [None], good + [{'RepoTags': []}],
                       good + [{'Id': 'd' * 64, 'RepoTags': []}],
                       good + [{'Id': self.other, 'RepoTags': None}],
                       good + [{'Id': self.other, 'RepoTags': 'user/keep:1'}],
                       good + [{'Id': self.other, 'RepoTags': [1]}],
                       good + [{'Id': self.other, 'RepoTags': ['']}],
                       good + [dict(a)], good + [{'Id': self.other, 'RepoTags': [self.VERSIONS['A']]}],
                       [im for im in good if im is not a] + [dict(a, RepoTags=a['RepoTags'] * 2)]):
            with self.subTest(images=images):
                self.assertIncomplete(self.decide(images=images))
        c = {'Id': 'c' * 64, 'Image': self.other}
        for containers in ([None], [{'Id': 'c' * 64}], [dict(c, Image=self.other[7:])],
                           [dict(c, Id='C' * 64)], [dict(c, Id=None)], [c, c]):
            with self.subTest(containers=containers):
                self.assertIncomplete(self.decide(containers=containers))
        self.assertIncomplete(catalog.obsolete_release_tags(TAG, [self.receipts['C']], good,
                                                            {'c': c}))

    def test_nothing_obsolete_selects_nothing(self):
        tags = {t: i for t, i in self.tags.items() if i != self.ids['A']}
        result = self.decide(images=self.inventory(tags))
        self.assertEqual((result['complete'], result['tags']), (True, ()), result)

    def test_the_answer_is_deterministic(self):
        first = self.decide()
        beta = dict(self.receipts['C'], tag=TAG + '-beta')
        tags = dict(self.tags, **{TAG + '-beta': self.ids['C']})
        containers = [{'Id': str(i) * 64, 'Image': self.ids[n]} for i, n in ((1, 'B'), (2, 'C'))]
        forward = self.decide([self.receipts['C'], beta], self.inventory(tags), containers)
        images = [dict(im, RepoTags=im['RepoTags'][::-1]) for im in self.inventory(tags)[::-1]]
        backward = self.decide([beta, self.receipts['C']], images, containers[::-1])
        self.assertEqual(forward, backward)
        self.assertEqual(first, self.decide())
        self.assertEqual(forward['tags'], first['tags'])

    def test_dropping_the_selected_tags_leaves_nothing_more_to_drop(self):
        first = self.decide()
        tags = {t: i for t, i in self.tags.items() if t not in first['tags']}
        dangling = [{'Id': self.ids['A'], 'RepoTags': []}]
        again = self.decide(images=self.inventory(tags, dangling))
        self.assertEqual((again['complete'], again['tags']), (True, ()), again)
        self.assertEqual(again['retain'], first['retain'])

    def test_the_decision_touches_no_docker_files_processes_or_network(self):
        def refuse(*a, **k):
            raise AssertionError('impure call')
        with patch.object(catalog, 'run', refuse), patch.object(catalog, 'image_id', refuse), \
                patch.object(catalog.subprocess, 'run', refuse), \
                patch.object(catalog.subprocess, 'Popen', refuse), patch('builtins.open', refuse), \
                patch.object(catalog.Path, 'read_text', refuse), \
                patch.object(catalog.Path, 'exists', refuse), \
                patch.object(catalog.Path, 'unlink', refuse), \
                patch.object(catalog.os, 'replace', refuse), patch.object(catalog.os, 'remove', refuse), \
                patch.object(catalog.urllib.request, 'build_opener', refuse), \
                patch.object(catalog.urllib.request, 'urlopen', refuse):
            self.assertEqual(self.decide()['tags'], tuple(sorted(self.a_tags)))


class CleanupPlan(unittest.TestCase):
    def setUp(self):
        d = tempfile.TemporaryDirectory()
        self.addCleanup(d.cleanup)
        self.xdg = Path(d.name) / 'cache'
        self.cache = self.xdg / 'remora/images'
        self.cache.mkdir(parents=True)
        self.receipt = self.cache / (TAG.replace(':', '_') + '.receipt.json')
        self.a, self.b, self.c = ('sha256:' + n * 64 for n in 'abc')
        self.a_tags = [rollback_alias(self.a), TAG + '-20261113.012458']
        self.receipt.write_text(json.dumps({'tag': TAG, 'image': self.c, 'signed_image_id': self.c,
                                           'previous': self.b, 'backup': rollback_alias(self.b)}))
        self.images = {self.a: {'Id': self.a, 'RepoTags': self.a_tags.copy()},
                       self.b: {'Id': self.b, 'RepoTags': [rollback_alias(self.b)]},
                       self.c: {'Id': self.c, 'RepoTags': [TAG]}}
        self.containers = {}
        self.calls = []

    def docker(self, args, **kwargs):
        self.calls.append(args)
        if args == ('docker', 'context', 'inspect'):
            out = json.dumps([{'Endpoints': {'docker': {'Host': 'unix:///var/run/docker.sock'}}}])
        else:
            self.assertEqual(args[0], 'docker')
            self.assertIn(args[1], ('image', 'container'))
            self.assertIn(args[2], ('ls', 'inspect'))
            images = args[1] == 'image'
            records = self.images if images else self.containers
            if args[2] == 'ls':
                self.assertEqual(args[3:], ('--all', '--quiet', '--no-trunc'))
                out = '\n'.join(records)
                if images:
                    out += '\n' + self.a  # Docker may list the same image for each tag.
            else:
                field = 'RepoTags' if images else 'Image'
                self.assertEqual(args[3:5], ('--format', '{"Id":{{json .Id}},"' + field
                                                        + '":{{json .' + field + '}}}'))
                out = '\n'.join(json.dumps(records[iid]) for iid in args[5:] if iid in records)
        return subprocess.CompletedProcess(args, 0, stdout=out, stderr='')

    def command(self, docker=None, failure=None):
        out = io.StringIO()
        with patch.dict('os.environ', {'XDG_CACHE_HOME': str(self.xdg), 'DOCKER_HOST': ''}), \
                patch.object(catalog.subprocess, 'run', side_effect=docker or self.docker), \
                patch.object(catalog, 'get', side_effect=AssertionError('catalog was fetched')), \
                contextlib.redirect_stdout(out):
            if failure:
                with self.assertRaisesRegex(failure[0], failure[1]):
                    catalog.main(['cleanup-plan', '--tag', TAG])
                self.assertEqual(out.getvalue(), '')
                return
            self.assertEqual(catalog.main(['cleanup-plan', '--tag', TAG]), 0)
        prefix = 'REMORA_IMAGES_RESULT='
        self.assertTrue(out.getvalue().startswith(prefix), out.getvalue())
        return json.loads(out.getvalue()[len(prefix):])

    def test_lists_candidates_without_writing_cache_or_changing_docker(self):
        original = self.receipt.read_bytes()
        self.assertEqual(self.command(), {'tag': TAG, 'dry_run': True, 'complete': True,
                                         'reason': None, 'tags': sorted(self.a_tags),
                                         'retain': [self.b, self.c]})
        self.assertEqual(list(self.cache.iterdir()), [self.receipt])
        self.assertEqual(self.receipt.read_bytes(), original)
        self.assertEqual(self.images[self.a]['RepoTags'], self.a_tags)

    def test_retains_running_and_stopped_containers_images(self):
        cid = '1' * 64
        for running in (True, False):
            with self.subTest(running=running):
                self.containers[cid] = {'Id': cid, 'Image': self.a, 'State': {'Running': running}}
                result = self.command()
                self.assertEqual(result['tags'], [])
                self.assertIn(self.a, result['retain'])

    def test_retains_foreign_or_unanchored_tags(self):
        for held in (self.a_tags + ['user/keep:latest'], self.a_tags[1:]):
            with self.subTest(tags=held):
                self.images[self.a]['RepoTags'] = held
                self.assertEqual(self.command()['tags'], [])

    def test_untagged_classic_store_image_is_not_a_candidate(self):
        iid = 'sha256:' + 'f' * 64
        self.images[iid] = {'Id': iid, 'RepoTags': None}
        self.assertEqual(self.command()['tags'], sorted(self.a_tags))

    def test_docker_error_or_partial_inspection_emits_no_plan(self):
        for rc in (0, 1):
            with self.subTest(returncode=rc):
                def inspect(args, **kwargs):
                    if args[1:3] == ('image', 'inspect'):
                        return subprocess.CompletedProcess(args, rc, stdout='',
                                                           stderr='docker unavailable' if rc else '')
                    return self.docker(args, **kwargs)

                reason = 'docker unavailable' if rc else 'Incomplete Docker image inspection'
                self.command(docker=inspect, failure=(catalog.VerificationError, reason))

    def test_inventory_change_emits_no_plan(self):
        def inspect(args, **kwargs):
            if args[1:3] == ('image', 'inspect') and args in self.calls:
                self.images[self.a]['RepoTags'].append('user/hold:1')
            return self.docker(args, **kwargs)

        self.command(docker=inspect, failure=(catalog.VerificationError, 'changed during'))

    def test_missing_container_image_emits_no_plan(self):
        cid = '1' * 64
        self.containers[cid] = {'Id': cid, 'Image': 'sha256:' + 'f' * 64}
        self.command(failure=(catalog.VerificationError, 'container image is absent'))

    def test_receipt_errors_precede_docker_queries(self):
        pending = self.receipt.with_suffix('.pending.json')
        pending.touch()
        self.command(failure=(catalog.VerificationError, 'receipt is pending'))
        pending.unlink()
        self.receipt.write_text('{')
        self.command(failure=(json.JSONDecodeError, 'Expecting property name'))
        self.assertEqual(self.calls, [])

    def test_missing_cache_fails_without_creating_it(self):
        self.xdg = self.xdg.with_name('missing')
        self.command(failure=(catalog.VerificationError, 'No verified-import receipt'))
        self.assertFalse(self.xdg.exists())
        self.assertEqual(self.calls, [])


if __name__ == '__main__':
    unittest.main()
