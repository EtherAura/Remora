"""Offline tests for the public image catalog verifier (bd remora-qksj). No network, no Docker daemon.

The acceptance line these pin: a corrupt, truncated, stale or unauthorized artifact fails safely,
and every check precedes `docker load`.
"""
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
                   legacy=False):
    """A minimal `docker save` archive in the OCI layout Docker 25+ writes (or the legacy one)."""
    config = json.dumps({'architecture': 'amd64', 'os': 'linux', 'config': {'Labels': (
        {'remora.public_release': 'true'} if labels is None else labels)},
        'rootfs': {'type': 'layers', 'diff_ids': []}}).encode()
    digest = hashlib.sha256(config).hexdigest()
    cname = digest + '.json' if legacy else 'blobs/sha256/' + digest
    with tarfile.open(path, 'w') as t:
        add(t, cname, config_override or config)
        add(t, 'manifest.json', json.dumps([{'Config': cname, 'Layers': [],
                                             'RepoTags': repo_tags or [VTAG]}]).encode())
        if not legacy:
            add(t, 'index.json', json.dumps({'manifests': [{'annotations': {
                'io.containerd.image.name': index_name or 'docker.io/library/' + VTAG,
                'org.opencontainers.image.ref.name': VTAG.split(':')[1]}}]}).encode())
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


if __name__ == '__main__':
    unittest.main()
