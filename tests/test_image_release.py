"""Offline tests for the weekly public image job and the catalog changes it relies on
(bd remora-47jx). No network, no Docker daemon, no Android tree.

What these pin: a refreshed pin is only built when it is a pin every later build can sync; a
patch the last good build applied may not go missing silently; the fallback never publishes an
older Android than the one released; and the catalog can carry a second release at all.
"""
import datetime as dt
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]


def load(name, rel):
    spec = importlib.util.spec_from_file_location(name, ROOT / rel)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


weekly = load('weekly_release', 'vendor/release/weekly-release.py')
pkg = load('package_release', 'vendor/release/package-release.py')

SHA_A, SHA_B, SHA_C = 'a' * 40, 'b' * 40, 'c' * 40


def manifest(projects):
    rows = ''.join('  <project name="%s" path="%s" revision="%s"/>\n' % (p, p, r)
                   for p, r in projects.items())
    return ('<?xml version="1.0" encoding="UTF-8"?>\n<manifest>\n%s</manifest>\n' % rows).encode()


class Pins(unittest.TestCase):
    def test_compare(self):
        old = {'a': SHA_A, 'b': SHA_B, 'gone': SHA_C}
        new = {'a': SHA_A, 'b': SHA_C, 'new': SHA_A}
        d = weekly.compare_pins(old, new)
        self.assertEqual(d['changed'], [('b', SHA_B, SHA_C)])
        self.assertEqual(d['added'], ['new'])
        self.assertEqual(d['removed'], ['gone'])

    def test_parse(self):
        self.assertEqual(weekly.pin_projects(manifest({'x/y': SHA_A})), {'x/y': SHA_A})

    def test_a_snapshot_that_drops_a_project_is_refused(self):
        d = weekly.compare_pins({'a': SHA_A, 'b': SHA_B}, {'a': SHA_A})
        self.assertIn('lacks 1 project', weekly.refresh_refusal(d, {'a': SHA_A}))

    def test_a_snapshot_naming_a_branch_is_refused(self):
        new = {'a': 'refs/heads/lineage-24.0'}
        d = weekly.compare_pins({'a': SHA_A}, new)
        self.assertIn('not commit ids', weekly.refresh_refusal(d, new))

    def test_moving_mesa_or_libdrm_is_refused(self):
        for proj in weekly.MESA_PROJECTS:
            old, new = {proj: SHA_A, 'x': SHA_A}, {proj: SHA_B, 'x': SHA_A}
            self.assertIn('Mesa payload', weekly.refresh_refusal(weekly.compare_pins(old, new), new))

    def test_an_ordinary_move_is_accepted(self):
        old, new = {'bionic': SHA_A}, {'bionic': SHA_B, 'added': SHA_C}
        self.assertIsNone(weekly.refresh_refusal(weekly.compare_pins(old, new), new))


LOG = """\
=== source patches ===
WARN patch SKIPPED (conflicts), tree left clean: statusbar_hide_list
  ***WARN could not apply 0003-init-container.patch — SKIPPED (tree left clean)
WARN backport patch SKIPPED (conflicts): Add-toggle-to-enable-ADB-root
some other WARN line that is not a skip
"""


class Skips(unittest.TestCase):
    def test_every_skip_form_is_seen(self):
        self.assertEqual(weekly.skipped_patches(LOG), {
            'patch:statusbar_hide_list', 'container:0003-init-container.patch',
            'backport:Add-toggle-to-enable-ADB-root'})

    def test_a_clean_log_skips_nothing(self):
        self.assertEqual(weekly.skipped_patches('=== BUILD EXIT 0 ===\n'), set())

    def test_new_skips_against_the_baseline(self):
        self.assertEqual(weekly.new_skips({'patch:a', 'patch:b'}, ['patch:a']), ['patch:b'])
        self.assertEqual(weekly.new_skips({'patch:a'}, ['patch:a', 'patch:z']), [])

    def test_without_a_baseline_every_skip_is_new(self):
        # A refreshed pin has to prove it applies everything before a baseline exists.
        self.assertEqual(weekly.new_skips({'patch:a'}, None), ['patch:a'])


class Plan(unittest.TestCase):
    def test_refreshed_first_then_base_when_remora_moved(self):
        self.assertEqual(weekly.plan_candidates(True, 'new', 'old'), ['refreshed', 'base'])

    def test_unchanged_base_is_not_rebuilt(self):
        # A failed refresh over an unchanged base has nothing new to publish.
        self.assertEqual(weekly.plan_candidates(True, 'same', 'same'), ['refreshed'])
        self.assertEqual(weekly.plan_candidates(False, 'same', 'same'), [])

    def test_unrecorded_published_inputs_mean_changed(self):
        self.assertEqual(weekly.plan_candidates(False, 'x', None), ['base'])

    def test_force(self):
        self.assertEqual(weekly.plan_candidates(False, 'same', 'same', force=True), ['base'])


class Base(unittest.TestCase):
    def test_a_repin_in_the_repository_wins(self):
        pin, why = weekly.choose_base({'committed_pin_sha256': 'old'}, 'new', b'committed',
                                      b'published', b'stored')
        self.assertEqual(pin, b'committed')
        self.assertIn('repinned', why)

    def test_the_published_build_wins_over_an_older_repository_pin(self):
        # Falling back to the repository's pin after a refresh shipped would publish an OLDER
        # Android than the one users already run. The release itself says which pin that was,
        # so a run that died after uploading (its local copy stale) cannot get this wrong.
        pin, _ = weekly.choose_base({'committed_pin_sha256': 'same'}, 'same', b'committed',
                                    b'published', b'stale local copy')
        self.assertEqual(pin, b'published')

    def test_a_release_without_a_manifest_uses_the_local_copy(self):
        pin, _ = weekly.choose_base({'committed_pin_sha256': 'same'}, 'same', b'committed',
                                    None, b'stored')
        self.assertEqual(pin, b'stored')

    def test_lost_state_recovers_from_the_release(self):
        pin, _ = weekly.choose_base({}, 'same', b'committed', b'published', None)
        self.assertEqual(pin, b'published')

    def test_first_run(self):
        pin, _ = weekly.choose_base({}, 'same', b'committed', None, None)
        self.assertEqual(pin, b'committed')


class Digest(unittest.TestCase):
    trees = {'agent': 't1', 'vendor/source-build': 't2'}
    profile = {'features': 'mesa_source public_release'}

    def test_stable(self):
        a = weekly.inputs_digest({'x': SHA_A}, self.trees, self.profile)
        self.assertEqual(a, weekly.inputs_digest({'x': SHA_A}, dict(self.trees), dict(self.profile)))

    def test_each_input_counts(self):
        a = weekly.inputs_digest({'x': SHA_A}, self.trees, self.profile)
        self.assertNotEqual(a, weekly.inputs_digest({'x': SHA_B}, self.trees, self.profile))
        self.assertNotEqual(a, weekly.inputs_digest({'x': SHA_A}, self.trees | {'agent': 't9'},
                                                    self.profile))
        self.assertNotEqual(a, weekly.inputs_digest({'x': SHA_A}, self.trees,
                                                    {'features': 'public_release'}))

    def test_mesa_key_follows_mesa_and_libdrm_only(self):
        k = weekly.mesa_key({'external/mesa3d': SHA_A, 'external/libdrm': SHA_B, 'x': SHA_A})
        self.assertEqual(k, weekly.mesa_key({'external/mesa3d': SHA_A, 'external/libdrm': SHA_B}))
        self.assertNotEqual(k, weekly.mesa_key({'external/mesa3d': SHA_C,
                                                'external/libdrm': SHA_B}))


class Resign(unittest.TestCase):
    def test_window(self):
        now = dt.datetime(2027, 1, 1, tzinfo=dt.timezone.utc)
        soon = {'expires_at': (now + dt.timedelta(days=10)).isoformat()}
        later = {'expires_at': (now + dt.timedelta(days=60)).isoformat()}
        self.assertTrue(weekly.needs_resign(soon, now, 21))
        self.assertFalse(weekly.needs_resign(later, now, 21))


def entry(stamp, tag='remora24:x86_64-public'):
    name = 'remora24-x86_64-public-%s' % stamp
    ref = lambda suffix: {'file': name + suffix, 'bytes': 1, 'sha256': '0' * 64}  # noqa: E731
    return {'tag': tag, 'version_tag': 'remora24:x86_64-public-%s' % stamp,
            'image_id': 'sha256:' + '1' * 64, 'archive': ref('.tar.xz'), 'sbom': ref('.spdx.json'),
            'notices': ref('-notices.tar'), 'audit': ref('-audit.json.gz'),
            'provenance': ref('-provenance.json'), 'sources': [ref('-src-remora.tar.xz')],
            'android_sdk': 37, 'abi': ['x86_64'], 'arch_variant': 'x86_64',
            'public_audit_passed': True, 'google_payloads': False}


class CatalogMerge(unittest.TestCase):
    def test_a_new_release_replaces_its_tag(self):
        old, new = entry('20261113.035540'), entry('20261122.063000')
        self.assertEqual(pkg.merged_images(new, {'images': [old]}), [new])

    def test_other_tags_are_kept(self):
        other = entry('20261101.000000', tag='remora24:x86_64-public-lts')
        new = entry('20261122.063000')
        self.assertEqual(pkg.merged_images(new, {'images': [other]}), [new, other])

    def test_the_second_release_signs_and_verifies(self):
        """The bug this fixes: listing the old entry beside the new one made sign_catalog's own
        verification refuse it (duplicate tag), so no release after the first could be made."""
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            priv, pub = d / 'k.pem', d / 'image-signing-key.pem'
            subprocess.run(['openssl', 'genpkey', '-algorithm', 'ED25519', '-out', str(priv)],
                           check=True, capture_output=True)
            subprocess.run(['openssl', 'pkey', '-in', str(priv), '-pubout', '-out', str(pub)],
                           check=True, capture_output=True)
            out = d / 'out'
            out.mkdir()
            previous = {'sequence': 1, 'images': [entry('20261113.035540')]}
            new = entry('20261122.063000')
            with patch.object(pkg, 'HERE', d):
                pkg.sign_catalog(out, priv, pkg.merged_images(new, previous), 2)
            index = json.loads((out / 'index.json').read_text())
            self.assertEqual([im['version_tag'] for im in index['images']], [new['version_tag']])
            self.assertEqual(index['sequence'], 2)


class Prune(unittest.TestCase):
    def test_only_what_neither_the_catalog_nor_a_kept_release_names(self):
        live, prev, old = entry('20261122.063000'), '20261115.063000', '20261108.063000'
        assets = (sorted(pkg.release_files(live)) + ['index.json', 'index.json.sig']
                  + ['remora24-x86_64-public-%s%s' % (prev, s) for s in ('.tar.xz', '-src-x.tar.xz')]
                  + ['remora24-x86_64-public-%s%s' % (old, s) for s in ('.tar.xz', '-audit.json.gz')])
        doomed = pkg.prunable(assets, {'images': [live]}, keep=['remora24-x86_64-public-' + prev])
        self.assertEqual(doomed, ['remora24-x86_64-public-%s-audit.json.gz' % old,
                                  'remora24-x86_64-public-%s.tar.xz' % old])

    def test_a_kept_name_does_not_match_a_longer_stamp(self):
        doomed = pkg.prunable(['remora24-x86_64-public-2026111.tar.xz'], {'images': []},
                              keep=['remora24-x86_64-public-202611'])
        self.assertEqual(doomed, ['remora24-x86_64-public-2026111.tar.xz'])


class Units(unittest.TestCase):
    def test_the_service_runs_the_published_script(self):
        service, timer = weekly.systemd_units('/usr/bin/python3', '/w/src/vendor/release/'
                                              'weekly-release.py', '/c/image-release.conf',
                                              '/w/src', 'main', 'Sun *-*-* 01:30:00')
        self.assertIn('ExecStartPre=/usr/bin/git -C /w/src fetch -q origin main', service)
        self.assertIn('ExecStart=/usr/bin/python3 -I /w/src/vendor/release/weekly-release.py run '
                      '--config /c/image-release.conf', service)
        self.assertIn('OnCalendar=Sun *-*-* 01:30:00', timer)
        self.assertIn('Persistent=true', timer)

    def test_lock_wraps_heavy_steps_only_when_configured(self):
        self.assertEqual(weekly.locked({'lock_command': ''}, ['m']), ['m'])
        self.assertEqual(weekly.locked({'lock_command': '/usr/local/bin/lock --'}, ['m']),
                         ['/usr/local/bin/lock', '--', 'm'])


if __name__ == '__main__':
    unittest.main()
