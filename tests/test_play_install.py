"""Offline security/transaction tests. No Google binaries or Docker daemon required."""
import copy
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import struct
import tarfile
import tempfile
import unittest
from unittest.mock import patch
import zipfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('play', ROOT / 'vendor/play/install.py')
play = importlib.util.module_from_spec(spec)
spec.loader.exec_module(play)
MANIFEST = json.loads((ROOT / 'vendor/play/package.json').read_text())


def axml(package, utf8=True, typed=False):
    strings = ['manifest', 'package', package]
    pool, offsets = b'', []
    for s in strings:
        offsets.append(len(pool))
        b = s.encode('utf-8' if utf8 else 'utf-16le')
        pool += (bytes([len(s), len(b)]) + b + b'\0') if utf8 else struct.pack('<H', len(s)) + b + b'\0\0'
    start = 28 + 4 * len(strings)
    sp = struct.pack('<HHIIIIII', 1, 28, start + len(pool), len(strings), 0, 0x100 if utf8 else 0, start, 0)
    sp += struct.pack('<III', *offsets) + pool
    node = struct.pack('<HHIII', 0x102, 16, 56, 0, 0xffffffff)
    node += struct.pack('<IIHHHHHH', 0xffffffff, 0, 20, 20, 1, 0, 0, 0)
    node += struct.pack('<IIIHBBI', 0xffffffff, 1, 0xffffffff if typed else 2, 8, 0, 3, 2)
    return struct.pack('<HHI', 3, 8, 8 + len(sp) + len(node)) + sp + node


def fixture(directory, extra=None, omit=False):
    payload = b'<permissions/>'
    name = 'system/product/etc/permissions/test.xml'
    m = {'files': {name: {'bytes': len(payload), 'sha256': hashlib.sha256(payload).hexdigest()}},
         'apks': [], 'tar_entry': 'files/files.tar.xz', 'tar_prefix': 'x86_64/37/'}
    b = io.BytesIO()
    with tarfile.open(fileobj=b, mode='w:xz') as tar:
        if not omit:
            i = tarfile.TarInfo(m['tar_prefix'] + name)
            i.size = len(payload)
            tar.addfile(i, io.BytesIO(payload))
        if extra:
            tar.addfile(extra, io.BytesIO(b''))
    archive = directory / 'package.zip'
    with zipfile.ZipFile(archive, 'w') as z:
        z.writestr(m['tar_entry'], b.getvalue())
        z.writestr('install.sh', 'exit 99')  # deliberately never extracted/executed
    m['archive_bytes'], m['archive_sha256'] = play.digest_file(archive)
    return archive, m


class InstallerTests(unittest.TestCase):
    def test_no_side_effect_before_acceptance(self):
        with patch.object(play, 'docker') as docker, patch.object(Path, 'mkdir') as mkdir:
            with self.assertRaises(play.InstallError):
                play.main(['--base', 'base:clean'])
            docker.assert_not_called()
            mkdir.assert_not_called()

    def test_manifest_utf8_utf16_and_typed_strings(self):
        for utf8 in [True, False]:
            for typed in [True, False]:
                self.assertEqual(play.axml_package(axml('com.google.android.gms', utf8, typed)), 'com.google.android.gms')
        for data in [b'', b'<manifest/>', axml('invalid'), axml('com.example')[:-1]]:
            with self.assertRaises(play.InstallError):
                play.axml_package(data)

    def test_extract_allowlist_and_permissions_under_private_umask(self):
        import os
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            archive, m = fixture(root)
            old = os.umask(0o077)
            try:
                play.extract_payload(archive, m, root / 'out')
            finally:
                os.umask(old)
            self.assertFalse((root / 'out/install.sh').exists())
            self.assertEqual((root / 'out/system/product').stat().st_mode & 0o777, 0o755)
            self.assertEqual((root / 'out' / next(iter(m['files']))).stat().st_mode & 0o777, 0o644)

    def test_reject_archive_corruption_missing_and_wrong_file(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            a, m = fixture(root)
            play.verify_archive(a, m)
            bad = copy.deepcopy(m)
            bad['files'][next(iter(bad['files']))]['sha256'] = '0' * 64
            with self.assertRaises(play.InstallError):
                play.extract_payload(a, bad, root / 'bad')
            a.write_bytes(a.read_bytes() + b'corrupt')
            with self.assertRaises(play.InstallError):
                play.verify_archive(a, m)
            a, m = fixture(root, omit=True)
            with self.assertRaises(play.InstallError):
                play.extract_payload(a, m, root / 'missing')

    def test_reject_traversal_links_devices_duplicates(self):
        for name, kind in [('../escape', tarfile.REGTYPE), ('/escape', tarfile.REGTYPE),
                           ('symlink', tarfile.SYMTYPE), ('hardlink', tarfile.LNKTYPE),
                           ('device', tarfile.CHRTYPE),
                           ('x86_64/37/system/product/etc/permissions/test.xml', tarfile.REGTYPE)]:
            with self.subTest(name=name), tempfile.TemporaryDirectory() as d:
                root = Path(d)
                entry = tarfile.TarInfo(name)
                entry.type = kind
                entry.linkname = '/outside'
                a, m = fixture(root, entry)
                with self.assertRaises(play.InstallError):
                    play.extract_payload(a, m, root / 'out')

    def test_reject_https_downgrade(self):
        with self.assertRaises(play.InstallError):
            play.HttpsOnly().redirect_request(None, None, 302, '', {}, 'http://example.com/pkg')

    def test_download_is_pinned_atomic_bounded_and_cache_is_rechecked(self):
        payload = b'pinned archive'
        m = {'archive_bytes': len(payload), 'archive_sha256': hashlib.sha256(payload).hexdigest(),
             'source_url': 'https://provider.example/package.zip'}
        with tempfile.TemporaryDirectory() as d:
            cache = Path(d)
            with patch.object(play.urllib.request, 'build_opener') as opener:
                opener.return_value.open.return_value = io.BytesIO(payload)
                downloaded = play.download(m, cache)
                self.assertEqual(downloaded.read_bytes(), payload)
                play.download(m, cache)
                self.assertEqual(opener.return_value.open.call_count, 1)
                downloaded.write_bytes(b'corrupted cache')
                with self.assertRaises(play.InstallError):
                    play.download(m, cache)
                downloaded.unlink()
                opener.return_value.open.return_value = io.BytesIO(payload + b'too many bytes')
                with self.assertRaises(play.InstallError):
                    play.download(m, cache)
                self.assertEqual(list(cache.iterdir()), [])

    def test_reject_public_names_and_option_injection(self):
        for tag in ['docker.io/user/play:latest', 'remora-local:bad\nRUN x', '-f', 'remora-local:']:
            with self.assertRaises(play.InstallError):
                play.local_tag(tag)
        with patch.object(play, 'docker') as docker:
            with self.assertRaises(play.InstallError):
                play.inspect_image('--help')
            docker.assert_not_called()

    # The signature audit is development-repository evidence: it also records comparisons against
    # private payload caches, so the public tree leaves it out and this skips there.
    AUDIT = ROOT / 'docs' / 'investigations' / 'google-play-source-terms' / 'audit.json'

    @unittest.skipUnless(AUDIT.exists(), 'signature audit evidence is kept in the development repository')
    def test_pinned_apks_match_verified_signature_audit(self):
        audit = json.loads(self.AUDIT.read_text())
        audited = {a['sha256']: a for a in audit['candidate_gapps']['base_apks']}
        for apk in MANIFEST['apks']:
            evidence = audited[apk['sha256']]
            self.assertTrue(evidence['apk_signature_verified'])
            self.assertEqual(apk['certificate_lineage_sha256'], evidence['certificate_lineage_sha256'])

    def test_base_preflight_refuses_microg_wrong_sdk_links_and_onbuild(self):
        from types import SimpleNamespace
        base = {'Id': 'sha256:base', 'Architecture': 'amd64', 'Os': 'linux', 'Config': {}}
        for scenario in ['microg', 'sdk', 'abi', 'link', 'onbuild', 'clean']:
            with self.subTest(scenario=scenario):
                image = copy.deepcopy(base)
                if scenario == 'onbuild':
                    image['Config']['OnBuild'] = ['RUN touch /bad']
                b = io.BytesIO()
                with tarfile.open(fileobj=b, mode='w') as tar:
                    sdk = 36 if scenario == 'sdk' else 37
                    abi = 'arm64-v8a' if scenario == 'abi' else 'x86_64'
                    props = f'ro.build.version.sdk={sdk}\nro.product.cpu.abilist={abi}\n'.encode()
                    entry = tarfile.TarInfo('system/build.prop')
                    entry.size = len(props)
                    tar.addfile(entry, io.BytesIO(props))
                    if scenario == 'link':
                        entry = tarfile.TarInfo('system/product')
                        entry.type = tarfile.SYMTYPE
                        entry.linkname = '/escape'
                        tar.addfile(entry)
                    if scenario == 'microg':
                        apk = io.BytesIO()
                        with zipfile.ZipFile(apk, 'w') as z:
                            z.writestr('AndroidManifest.xml', axml('com.google.android.gms'))
                        # Hidden behind an unrelated filename in baked user data.
                        entry = tarfile.TarInfo('data/app/unrelated/base.apk')
                        entry.size = len(apk.getvalue())
                        tar.addfile(entry, io.BytesIO(apk.getvalue()))
                process = SimpleNamespace(stdout=io.BytesIO(b.getvalue()), wait=lambda: 0, poll=lambda: 0)
                with patch.object(play, 'docker', return_value=SimpleNamespace(stdout='cid')), patch.object(play.subprocess, 'Popen', return_value=process):
                    if scenario == 'clean':
                        self.assertEqual(play.base_preflight(image, MANIFEST)['sdk'], 37)
                    else:
                        with self.assertRaises(play.InstallError):
                            play.base_preflight(image, MANIFEST)

    def test_companion_permission_is_denied_without_weakening_enforcement(self):
        import xml.etree.ElementTree as ET
        xml = ET.fromstring(next(iter(play.LOCAL_FILES.values())))
        self.assertEqual(xml.find('privapp-permissions').get('package'), 'com.google.android.gms')
        self.assertEqual(xml.find('.//deny-permission').get('name'), 'android.permission.ACCESS_COMPANION_INFO')
        self.assertIsNone(xml.find('.//permission'))

    def test_rollback_refuses_container_use_and_missing_base(self):
        image = {'Id': 'sha256:overlay', 'Config': {'Labels': {
            'org.remora.play.local_only': 'true', 'org.remora.play.base': 'sha256:base'}}}
        with tempfile.TemporaryDirectory() as d, patch.object(play, 'inspect_image', side_effect=[image, None]), patch.object(play, 'docker') as docker:
            with self.assertRaises(play.InstallError):
                play.rollback('remora-local:play', Path(d))
            docker.assert_not_called()
        from types import SimpleNamespace
        with tempfile.TemporaryDirectory() as d, patch.object(play, 'inspect_image', side_effect=[image, {'Id': 'sha256:base'}]), patch.object(play, 'docker') as docker:
            docker.side_effect = [SimpleNamespace(stdout='cid\n'), SimpleNamespace(stdout='[{"Image":"sha256:overlay"}]')]
            with self.assertRaises(play.InstallError):
                play.rollback('remora-local:play', Path(d))
            self.assertFalse(any(c.args[:2] == ('image', 'rm') for c in docker.call_args_list))

    def test_rollback_preserves_untagged_parent_images(self):
        from types import SimpleNamespace
        image = {'Id': 'sha256:overlay', 'Config': {'Labels': {
            'org.remora.play.local_only': 'true', 'org.remora.play.base': 'sha256:untagged-base'}}}
        with tempfile.TemporaryDirectory() as d, patch.object(play, 'inspect_image', side_effect=[image, {'Id': 'sha256:untagged-base'}]), patch.object(play, 'docker', return_value=SimpleNamespace(stdout='')) as docker:
            play.rollback('remora-local:play', Path(d))
            docker.assert_any_call('image', 'rm', '--no-prune', 'remora-local:play')


if __name__ == '__main__':
    unittest.main()
