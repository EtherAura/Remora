"""Offline tests for the public-release audit's policy (bd remora-phvx). No Docker, no AOSP tree.

The walk itself was validated against a real private image, which it refuses for exactly its
payloads; these pin the rules that walk applies, so an edit to a pattern or a property name cannot
quietly turn a refusal into a pass.
"""
import csv
import importlib.util
import os
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('audit', ROOT / 'vendor/source-build/public-audit.py')
audit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(audit)

ELF_HEAD = b'\x7fELF\x02\x01\x01' + b'\0' * 11  # e_machine follows at offset 18


class Paths(unittest.TestCase):
    # Every one of these was found in the private deployed image by the real walk.
    EXCLUDED = [
        'system/bin/arm64/linker64', 'system/lib64/arm64/libc.so', 'system/lib64/libnb.so',
        'system/bin/ndk_translation_program_runner_binfmt_misc_arm64',
        'system/lib64/libberberis_exec_region.so', 'system/etc/ld.config.arm64.txt',
        'vendor/lib64/libwvaidl.so', 'vendor/bin/hw/android.hardware.drm-service-lazy.widevine',
        'system/product/priv-app/GmsCore/GmsCore.apk', 'system/product/priv-app/Phonesky',
        'system/product/etc/permissions/com.google.android.gms.xml',
        'system/etc/init/magisk/magisk', 'system/etc/init/magisk/busybox', 'system/xbin/su',
        'system/etc/remora-spoof/pif.json', 'system/etc/remora-spoof/rez-mod/lib64/libzygisk.so',
    ]
    # Stock AOSP an x86_64 public image legitimately carries.
    CLEAN = [
        'system/bin/sh', 'system/lib64/libc.so', 'system/lib/libc.so', 'system/apex/com.android.media.apex',
        'system/etc/security/cacerts/01419da9.0', 'system/fonts/Roboto-Regular.ttf',
        'vendor/lib64/dri/libgallium_dri.so', 'vendor/lib64/hw/vulkan.intel.so',
        'system/etc/bpf/netd.o', 'system/system_ext/priv-app/Launcher3QuickStep/Launcher3QuickStep.apk',
        'system/etc/permissions/android.hardware.location.gps.xml',
        # AOSP's own, despite "google" in the name (both were false positives on the first public
        # build): Google's open-source software codecs, and the mainline patch level declaration.
        'vendor/etc/media_codecs_google_video.xml',
        'system/etc/sysconfig/com.google.android.mainline.patchlevel.2.xml',
    ]

    def test_excluded_payloads_are_named(self):
        for p in self.EXCLUDED:
            with self.subTest(p):
                self.assertTrue(audit.path_findings(p), p)

    def test_stock_aosp_is_not(self):
        for p in self.CLEAN:
            with self.subTest(p):
                self.assertEqual(audit.path_findings(p), [])

    def test_payload_origins(self):
        for pkg in ('vendor/gapps', 'vendor/ndk_translation/guest', 'vendor/widevine', 'vendor/magisk'):
            self.assertTrue(audit.EXCLUDED_ORIGINS.match(pkg), pkg)
        for pkg in ('vendor/lineage', 'vendor/remora', 'vendor/mesa_source', 'external/bash'):
            self.assertFalse(audit.EXCLUDED_ORIGINS.match(pkg), pkg)

    def test_a_link_is_judged_by_its_target_too(self):
        self.assertTrue(audit.path_findings('system/lib64/libinnocent.so', 'libnb.so'))

    def test_excluded_packages(self):
        for pkg in ('com.google.android.gms', 'com.android.vending', 'com.google.android.gsf',
                    'org.microg.gms', 'com.aurora.store', 'com.topjohnwu.magisk'):
            self.assertTrue(audit.EXCLUDED_PACKAGES.match(pkg), pkg)
        for pkg in ('com.android.settings', 'org.lineageos.jelly', 'com.android.vending.billing'):
            self.assertFalse(audit.EXCLUDED_PACKAGES.match(pkg), pkg)


class Elf(unittest.TestCase):
    def elf(self, machine):
        return ELF_HEAD + machine.to_bytes(2, 'little')

    def test_machines(self):
        self.assertEqual(audit.elf_machine(self.elf(62)), ('x86_64', True))
        self.assertEqual(audit.elf_machine(self.elf(3)), ('x86', True))
        self.assertEqual(audit.elf_machine(self.elf(247)), ('bpf', True))
        self.assertEqual(audit.elf_machine(self.elf(183)), ('aarch64', False))
        self.assertEqual(audit.elf_machine(self.elf(40)), ('arm', False))
        self.assertEqual(audit.elf_machine(self.elf(243)), ('riscv', False))
        self.assertIsNone(audit.elf_machine(b'#!/system/bin/sh\n'))


def props(**kv):
    return {k.replace('_', '.'): [{'file': 'system/build.prop', 'value': v}] for k, v in kv.items()}


class Props(unittest.TestCase):
    def clean(self):
        p = {
            'ro.system.product.cpu.abilist': [{'file': 'system/build.prop', 'value': 'x86_64'}],
            'ro.system.product.cpu.abilist32': [{'file': 'system/build.prop', 'value': ''}],
            'ro.vendor.product.cpu.abilist64': [{'file': 'vendor/build.prop', 'value': 'x86_64'}],
            'ro.dalvik.vm.native.bridge': [{'file': 'vendor/build.prop', 'value': '0'}],
            'ro.remora.public_release': [{'file': 'system/build.prop', 'value': 'true'}],
        }
        return p

    def test_a_public_build_passes(self):
        self.assertEqual(audit.prop_findings(self.clean()), [])

    def test_every_occurrence_is_checked_not_just_the_first(self):
        # the private image: vendor says 0, a later partition re-enables the bridge
        p = self.clean()
        p['ro.dalvik.vm.native.bridge'].append({'file': 'system/product/etc/build.prop',
                                                'value': 'libnb.so'})
        self.assertEqual([f for f, _ in audit.prop_findings(p)], ['system/product/etc/build.prop'])

    def test_arm_abi_bridge_exec_and_isa_mapping_fail(self):
        for k, v in (('ro.system.product.cpu.abilist', 'x86_64,arm64-v8a'),
                     ('ro.odm.product.cpu.abilist64', 'x86_64,arm64-v8a'),
                     ('ro.system.product.cpu.abilist32', 'x86'),
                     ('ro.enable.native.bridge.exec', '1'),
                     ('ro.dalvik.vm.isa.arm64', 'x86_64')):
            p = self.clean()
            p[k] = [{'file': 'x', 'value': v}]
            with self.subTest(k):
                self.assertTrue(audit.prop_findings(p))

    def test_guards_against_checking_nothing(self):
        p = self.clean()
        for k in [k for k in p if 'abilist' in k]:
            del p[k]
        self.assertTrue(any('checked nothing' in w for _, w in audit.prop_findings(p)))
        p = self.clean()
        del p['ro.remora.public_release']
        self.assertTrue(any('not a public_release' in w for _, w in audit.prop_findings(p)))


class Licences(unittest.TestCase):
    """A two-module compliance tree in the format soong writes (modules.csv, make_metadata.csv)."""

    def setUp(self):
        self.d = tempfile.TemporaryDirectory()
        d = Path(self.d.name)
        cols = ['id', 'name', 'package', 'module_type', 'installed_files', 'licenses',
                'pkg_default_applicable_licenses', 'lic_license_kinds', 'lk_conditions']
        rows = [
            dict(name='SPDX-license-identifier-MIT', module_type='license_kind', lk_conditions='notice'),
            dict(name='SPDX-license-identifier-GPL-2.0', module_type='license_kind',
                 lk_conditions='restricted'),
            dict(name='legacy_proprietary', module_type='license_kind',
                 lk_conditions='by_exception_only proprietary'),
            dict(name='legacy_by_exception_only', module_type='license_kind',
                 lk_conditions='by_exception_only'),
            dict(name='mit_lic', module_type='license', lic_license_kinds='SPDX-license-identifier-MIT'),
            dict(name='gpl_lic', module_type='license', lic_license_kinds='SPDX-license-identifier-GPL-2.0'),
            dict(name='blob_lic', module_type='license', lic_license_kinds='legacy_proprietary'),
            dict(name='frameworks_av_license', module_type='license',
                 lic_license_kinds='legacy_by_exception_only'),
            dict(name='novel_lic', module_type='license', lic_license_kinds='legacy_by_exception_only'),
            dict(name='//external/iptables', module_type='package', pkg_default_applicable_licenses='gpl_lic'),
            dict(name='iptables', package='external/iptables', module_type='cc_binary',
                 installed_files='out/target/product/p/system/bin/iptables'),
            dict(name='libfoo', package='external/foo', module_type='cc_library', licenses='mit_lic',
                 installed_files='out/target/product/p/system/lib64/libfoo.so'),
            dict(name='blob', package='vendor/blob', module_type='cc_prebuilt', licenses='blob_lic',
                 installed_files='out/target/product/p/vendor/lib64/blob.so'),
            dict(name='media', package='frameworks/av', module_type='cc_binary',
                 licenses='frameworks_av_license', installed_files='out/target/product/p/system/bin/mediaserver'),
            dict(name='novel', package='external/novel', module_type='cc_binary', licenses='novel_lic',
                 installed_files='out/target/product/p/system/bin/novel'),
            # LineageOS's extras declare NO licence at all; only their COPYING says GPL
            dict(name='bash', package='external/bash', module_type='cc_binary',
                 installed_files='out/target/product/p/system/system_ext/bin/bash'),
            dict(name='remora_agent', package='vendor/remora_agent', module_type='android_app',
                 installed_files='out/target/product/p/system/system_ext/framework/remora-agent.jar'),
        ]
        with open(d / 'modules.csv', 'w', newline='') as f:
            w = csv.DictWriter(f, cols)
            w.writeheader()
            for i, r in enumerate(rows):
                w.writerow({c: '' for c in cols} | r | {'id': str(i)})
        (d / 'NOTICE.gpl').write_text('GNU GENERAL PUBLIC LICENSE\nVersion 2\n')
        (d / '.repo').mkdir()
        (d / '.repo/project.list').write_text('external/bash\nexternal/foo\n')
        (d / 'external/bash').mkdir(parents=True)
        (d / 'external/bash/COPYING').write_text('GNU GENERAL PUBLIC LICENSE\nVersion 3\n')
        with open(d / 'make_metadata.csv', 'w', newline='') as f:
            cols = ['installed_file', 'module_path', 'is_soong_module', 'is_prebuilt_make_module',
                    'product_copy_files', 'kernel_module_copy_files', 'is_platform_generated',
                    'static_libs', 'whole_static_libs', 'license_text']
            w = csv.DictWriter(f, cols)
            w.writeheader()
            w.writerow({c: '' for c in cols} | {'installed_file': 'out/target/product/p/system/build.prop',
                                                'is_platform_generated': 'Y'})
            w.writerow({c: '' for c in cols} | {'installed_file': 'out/target/product/p/vendor/lib64/mesa.so',
                                                'module_path': 'vendor/mesa_source',
                                                'license_text': 'NOTICE.gpl'})
        audit.TREE_ROOT = str(d)
        audit.TEXT_CACHE.clear()
        self.lic = audit.Licences(str(d))

    def tearDown(self):
        self.d.cleanup()

    def verdict(self, path, reviewed=()):
        module, package, names, kinds, conds = self.lic.classify(path)
        return audit.licence_verdict(self.lic, names, kinds, conds, set(reviewed)), conds

    def test_package_default_and_module_licences(self):
        self.assertEqual(self.lic.classify('system/bin/iptables')[4], {'restricted'})
        self.assertEqual(self.lic.classify('system/lib64/libfoo.so')[4], {'notice'})
        self.assertIsNone(self.lic.classify('system/bin/unknown'))

    def test_proprietary_fails_even_when_reviewed(self):
        v, _ = self.verdict('vendor/lib64/blob.so', reviewed={'blob_lic'})
        self.assertEqual(v[0], 'fail')

    def test_by_exception_only_needs_an_explicit_review(self):
        self.assertEqual(self.verdict('system/bin/novel')[0][0], 'fail')
        self.assertIsNone(self.verdict('system/bin/novel', reviewed={'novel_lic'})[0])

    def test_make_module_text_and_generated_files(self):
        v, conds = self.verdict('vendor/lib64/mesa.so')
        self.assertIsNone(v)
        self.assertIn('restricted', conds)  # a GPL text is an obligation, never permissive
        self.assertEqual(self.lic.classify('system/build.prop')[4], {'notice'})

    def test_undeclared_licences_fall_back_to_the_projects_own_text(self):
        module, package, names, kinds, conds = self.lic.classify('system/system_ext/bin/bash')
        self.assertEqual(package, 'external/bash')
        self.assertIn('restricted', conds)  # a source obligation, not an unexplained file
        self.assertTrue(names[0].startswith('(project text)'))

    def test_licence_texts_are_recognised_by_their_grant(self):
        d = Path(self.d.name)
        cases = {
            'gpl': ('GNU GENERAL PUBLIC LICENSE\n... into proprietary programs ...', {'restricted'}),
            'bsd': ('All rights reserved.\nRedistribution and use in source and binary forms', {'notice'}),
            'vim': ('VIM LICENSE\n\nI) There are no restrictions', {'reciprocal'}),
            'odd': ('This file is CONFIDENTIAL. Do not share.', {'unknown'}),
        }
        for name, (text, want) in cases.items():
            (d / name).write_text(text)
            with self.subTest(name):
                self.assertEqual(audit.text_conditions(name), want)

    def test_remora_staged_trees_are_remoras_own_gpl(self):
        c = self.lic.classify('system/system_ext/framework/remora-agent.jar')
        self.assertEqual(c[2], ['Remora GPL-3.0'])
        self.assertIn('restricted', c[4])

    def test_every_reviewed_licence_has_a_reason(self):
        self.assertIn('frameworks_av_license', audit.REVIEWED)
        for name, why in audit.REVIEWED.items():
            self.assertGreater(len(why), 40, name)


if __name__ == '__main__':
    unittest.main()
