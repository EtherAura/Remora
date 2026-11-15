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


if __name__ == '__main__':
    unittest.main()
