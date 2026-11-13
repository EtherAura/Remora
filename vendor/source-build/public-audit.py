#!/usr/bin/env python3
"""The public-release audit: refuse an image that carries anything the public register excludes.

Run by assemble-image.sh on every public_release build (bd remora-phvx), after `docker build` and
before the tag is allowed to stand. Args:

    public-audit.py --image <tag> --product <out/target/product/<p>>
                    --compliance <out/soong/compliance-metadata/<p>> --sbom <sbom.spdx.json>
                    --aapt2 <aapt2> --out-dir <dir>

WHY THE IMAGE AND NOT THE INPUTS. Every exclusion so far has been defeated by a route nobody was
watching: the ARM bridge reached images built with arm_translate off because assembly overlaid it
whenever its directory existed, and the device tree advertised arm64-v8a with no translator at all.
So this walks the exported rootfs of the built image: whatever reached it, by whatever route, is
looked at. Names are checked, but so is content — every ELF's machine type and every APK's package
name and native ABIs — because a name is the easiest thing to get wrong.

Three kinds of finding:
  failures      the image must not be published; the exit status is 1 and assembly drops the tag
  obligations   copyleft (restricted / reciprocal) licences, whose source the release must offer
  review        files the licence metadata does not explain; listed, not fatal, so they are read

The report is <out-dir>/<image id>.audit.json: the full file inventory with SHA-256s, the
property values that were checked, and the three lists. The inventory's path list is what a fresh
and a reused-output build are compared on.
"""

import argparse
import csv
import datetime as dt
import hashlib
import json
import os
import re
import subprocess
import sys
import tarfile
import tempfile
import zipfile

csv.field_size_limit(1 << 30)

# --- excluded payloads, by path ----------------------------------------------------------------
#
# docs/IMAGE_MIRROR.md excludes every proprietary, root and spoofing payload from a public image.
# Each pattern names the payload it stands for, so a hit reads as a reason rather than a regex.
# Matched case-insensitively against the image path (no leading slash) and against symlink targets.
EXCLUDED_PATHS = [
    (r'ndk_translation|(^|/)libnb\.so$|berberis', 'ARM translation (proprietary ndk_translation)'),
    (r'(^|/)(bin|lib|lib64)/arm(64)?(/|$)|ld\.config\.arm(64)?\.txt$', 'ARM guest runtime'),
    (r'widevine|(^|/)libwv[^/]*\.so|liboemcrypto', 'Widevine DRM (proprietary)'),
    (r'(^|/)(priv-)?app/(GmsCore|PrebuiltGmsCore[^/]*|Phonesky|GoogleServicesFramework|'
     r'Google[^/]*|Velvet|MicroGmsCore|MicroGCompanion|FakeStore|AuroraStore)(/|$)',
     'Google or microG apps'),
    # Scoped to the directories a GApps package drops its XML into. Unscoped, this matched AOSP's
    # own media_codecs_google_*.xml — Google's open-source software codecs, in frameworks/av.
    (r'(^|/)etc/(permissions|sysconfig|default-permissions|preferred-apps)/[^/]*google[^/]*\.xml$'
     r'|privapp-permissions-google', 'Google apps permission/sysconfig'),
    (r'(^|/)apex/com\.google\.', 'Google mainline module'),
    (r'magisk|(^|/)busybox$|(^|/)sbin(/|$)', 'Magisk / root'),
    (r'(^|/)xbin/su$|(^|/)bin/su$', 'su binary'),
    (r'remora-spoof|zygisk|playintegrity|shamiko|(^|/)pif\.json$|nb_sigfix',
     'Play Integrity spoofing stack'),
]
EXCLUDED_PATHS = [(re.compile(p, re.I), why) for p, why in EXCLUDED_PATHS]

# AOSP files a pattern above names but that are AOSP's own, each with where it comes from. Kept as
# short as possible: an entry is a promise that someone read the file.
ALLOWED_PATHS = [
    (re.compile(r'^system/etc/sysconfig/com\.google\.android\.mainline\.patchlevel\.[0-9]+\.xml$'),
     'AOSP system/apex/patchlevel (Apache-2.0): declares the mainline patch level, no Google code'),
]

# --- excluded payloads, by provenance -----------------------------------------------------------
# Where the compliance metadata says a file was BUILT FROM, which no renaming changes: the staged
# directories of every payload feature the public profile refuses.
EXCLUDED_ORIGINS = re.compile(
    r'^vendor/(gapps|microg|ndk_translation|magisk|widevine|zygisk_pif|google)(/|$)')

# --- excluded payloads, by APK package --------------------------------------------------------
# The package name is what Android installs by, whatever the file is called. com.google.* covers
# every Google app; microG deliberately reuses com.google.android.gms, so it is caught here too.
EXCLUDED_PACKAGES = re.compile(
    r'^(com\.google\.|com\.android\.vending$|org\.microg\.|com\.aurora\.store$|'
    r'com\.topjohnwu\.magisk$|io\.github\.huskydg\.magisk$)')

# ELF machines an x86_64 image may carry: x86_64, the four 32-bit x86 bionic stubs AOSP installs
# even on a 64-bit-only product, and eBPF objects under /system/etc/bpf. Anything else is foreign
# code — arm/arm64/riscv is a translator's guest payload, whatever its name.
ELF_ALLOWED = {62: 'x86_64', 3: 'x86', 247: 'bpf'}
ELF_NAMES = {40: 'arm', 183: 'aarch64', 243: 'riscv'}

# ABI properties. Every occurrence in every prop file is checked rather than the effective value,
# because a later partition overrides an earlier one and reproducing init's load order here would
# be a second opinion that could drift from the first.
ABILIST = re.compile(r'^ro\.(?:[a-z_]+\.)?product\.cpu\.abilist(32|64)?$')
PROP_FILES = re.compile(r'(^|/)(build\.prop|default\.prop|prop\.default)$')

# Licence conditions (soong's license_kind vocabulary). PROPRIETARY is fatal outright. The two
# REVIEW conditions mean "needs a licence review" in AOSP's own taxonomy, not "cannot be
# redistributed" — stock AOSP ships the CA store, frameworks/av and the OFL fonts under them — so a
# licence carrying one passes only once it is named, with its reason, in public-licence-review.json.
FATAL_CONDITIONS = {'proprietary'}
REVIEW_CONDITIONS = {'by_exception_only', 'not_allowed'}
SOURCE_CONDITIONS = {'restricted', 'restricted_if_statically_linked', 'reciprocal'}
REVIEWED = {k: v for k, v in json.load(open(os.path.join(
    os.path.dirname(os.path.abspath(__file__)), 'public-licence-review.json'))).items()
            if not k.startswith('_')}

# Written by `docker create` into every container, so they are in the export but not the image.
DOCKER_INJECTED = {'.dockerenv', 'dev/console', 'etc/hostname', 'etc/hosts', 'etc/resolv.conf'}


def run(*argv, **kw):
    return subprocess.run(argv, check=True, capture_output=True, text=True, **kw).stdout


def image_path(installed):
    """out/target/product/<p>/root/init -> init; .../system/bin/sh -> system/bin/sh."""
    rel = installed.split('/', 4)[4] if installed.startswith('out/target/product/') else installed
    return rel[5:] if rel.startswith('root/') else rel


class Licences:
    """Installed file -> licence names -> kinds -> conditions, resolved the way gen_sbom.py does.

    That is the tree's own SBOM generator (build/make/tools/sbom), so the audit and the published
    SBOM cannot classify the same file two ways: a soong module's own `licenses`, else its
    package's default_applicable_licenses; a make module's package default, else its notice text.
    """

    def __init__(self, compliance_dir):
        self.kind_conditions, self.licence_kinds = {}, {}
        self.package_default, self.module_licences = {}, {}
        self.soong_installed = {}
        with open(os.path.join(compliance_dir, 'modules.csv'), newline='') as f:
            for r in csv.DictReader(f):
                t = r['module_type']
                if t == 'license_kind':
                    self.kind_conditions[r['name']] = set(r['lk_conditions'].split())
                elif t == 'license':
                    self.licence_kinds[r['name']] = r['lic_license_kinds'].split()
                elif t == 'package':
                    self.package_default[r['name'].lstrip('/')] = \
                        r['pkg_default_applicable_licenses'].split()
                if r['licenses']:
                    self.module_licences[(r['name'], r['package'])] = r['licenses'].split()
                for f_ in r['installed_files'].split():
                    self.soong_installed.setdefault(f_, (r['name'], r['package']))
        # Repo projects, longest first, for the licence-text fallback below. Absent outside a
        # real tree (the tests), where every package simply has no project.
        try:
            with open(os.path.join(TREE_ROOT, '.repo/project.list')) as f:
                self.projects = sorted(f.read().split(), key=len, reverse=True)
        except OSError:
            self.projects = []
        self.make = {}
        with open(os.path.join(compliance_dir, 'make_metadata.csv'), newline='') as f:
            for r in csv.DictReader(f):
                self.make[image_path(r['installed_file'])] = r
        self.soong = {image_path(k): v for k, v in self.soong_installed.items()}

    def conditions(self, names):
        kinds = [k for n in names for k in self.licence_kinds.get(n, [])]
        conds = set()
        for k in kinds:
            # A kind soong does not define (legacy_* spelled in a make module, say) is unknown,
            # and unknown is treated as needing review rather than as permissive.
            conds |= self.kind_conditions.get(k, {'unknown'})
        return kinds, conds

    def project_of(self, package):
        for p in self.projects:
            if package == p or package.startswith(p + '/'):
                return p
        return None

    def from_project(self, module, package):
        """The fallback for a module that declares no licence: its project's OWN licence files.

        LineageOS's extra projects — bash, nano, vim, htop, rsync, libncurses, ntfs-3g — declare no
        licence in their Android.bp, so soong's metadata (and with it the image NOTICE and the SBOM)
        knows nothing about them, while each ships a COPYING. Read that, or GPL binaries pass as
        unexplained files and their source is never published. Remora's own staged trees are in
        no repo project; they are Remora's GPL-3.0, published as the public tree."""
        # A subpackage that declares nothing usually sits under one that does: soong does not
        # inherit default_applicable_licenses across directories, the metadata does not either,
        # but the ancestor's declaration is the project's own statement about that code.
        parent = package
        while '/' in parent:
            parent = parent.rsplit('/', 1)[0]
            names = self.package_default.get(parent)
            if names:
                return (module, package, names) + self.conditions(names)
        proj = self.project_of(package)
        if proj is None:
            if re.match(r'^(device|vendor|hardware)/remora|^vendor/(remora_|camera_external|usb_audio'
                        r'|mesa_source|device_compat|hw_features)', package):
                return (module, package, ['Remora GPL-3.0'], [], {'restricted'})
            return None
        root = os.path.join(TREE_ROOT, proj)
        try:
            names = sorted(n for n in os.listdir(root)
                           if re.match(r'(COPYING|LICEN[CS]E|NOTICE|MODULE_LICENSE_)', n))
        except OSError:
            return None
        # A REUSE-style LICENSES/ directory holds one text per licence; read each.
        expanded = []
        for n in names:
            if os.path.isdir(os.path.join(root, n)):
                expanded += [os.path.join(n, f) for f in sorted(os.listdir(os.path.join(root, n)))]
            else:
                expanded.append(n)
        names = expanded
        if not names:
            return None
        conds = set()
        for n in names:
            if n.startswith('MODULE_LICENSE_'):
                conds |= {'restricted'} if re.match(r'MODULE_LICENSE_(L?GPL)', n) else {'notice'}
        texts = [os.path.join(proj, n) for n in names if not n.startswith('MODULE_LICENSE_')]
        if texts:
            conds |= text_conditions(' '.join(texts))
        return (module, proj, ['(project text) ' + ' '.join(names)], [], conds)

    def classify(self, path):
        """-> (module, package, licence names, kinds, conditions) or None if nothing explains it."""
        module = package = None
        if path in self.soong:
            module, package = self.soong[path]
            names = self.module_licences.get((module, package)) or self.package_default.get(package, [])
            if names:
                return (module, package, names) + self.conditions(names)
        meta = self.make.get(path)
        if meta:
            if meta['is_platform_generated']:
                return ('(generated)', '', [], [], {'notice'})
            package = package or meta['module_path']
            names = self.package_default.get(package, [])
            if names:
                return ('(make)', package, names) + self.conditions(names)
            if meta['license_text']:
                return ('(make)', package, [], [], text_conditions(meta['license_text']))
            if meta['product_copy_files'] and not package:
                package = os.path.dirname(meta['product_copy_files'].split(':')[0])
        if package:
            return self.from_project(module or '(make)', package)
        return None


TEXT_CACHE = {}

# The operative grant of every permissive licence the tree carries — MIT/X11, BSD, Apache, ISC,
# zlib, Unicode, public-domain dedications, the vim charityware licence and the OFL.
PERMISSIVE_GRANT = (r'permission is hereby granted|redistribution and use in source and binary|'
                    r'licensed under the apache license|apache license,? version 2|'
                    r'permission to use, copy, modify|provided .as-is.|public domain|'
                    r'unicode, inc\. license|unlicense|creative commons|open font license|'
                    r'vim is charityware|may be distributed freely|permission is granted')


def text_conditions(paths):
    """A make module with only a notice text: classify the text itself. Conservative by design —
    a GPL/LGPL/MPL text is an obligation, an unreadable one is unknown, never permissive."""
    conds = set()
    for p in paths.split():
        if p not in TEXT_CACHE:
            try:
                with open(os.path.join(TREE_ROOT, p), errors='replace') as f:
                    t = f.read(400000)
            except OSError:
                TEXT_CACHE[p] = {'unknown'}
                continue
            c = set()
            if re.search(r'GNU (LESSER |LIBRARY |AFFERO )?GENERAL PUBLIC LICENSE', t):
                c.add('restricted')
            if re.search(r'Mozilla Public License|Eclipse Public License|Common Development and '
                         r'Distribution License', t):
                c.add('reciprocal')
            # The Vim licence: its text must travel with every copy (II.1), and a MODIFIED Vim — which
            # an Android port is — ships with all its changes as source (II.2.c). That is a source
            # obligation like any reciprocal licence, met the same way.
            if re.match(r'\s*VIM LICENSE', t):
                c.add('reciprocal')
            # A text that grants nothing recognisable goes to review rather than passing as
            # permissive. Recognition is by the grant, not by scary words: the GPL itself says
            # "proprietary programs", and "All rights reserved" heads half the BSD texts here.
            if not c and not re.search(PERMISSIVE_GRANT, t, re.I):
                c.add('unknown')
            TEXT_CACHE[p] = c or {'notice'}
        conds |= TEXT_CACHE[p]
    return conds


def path_findings(path, linkname=''):
    """Why <path> (or the target it links to) is an excluded payload, if it is one."""
    if any(rx.search(path) for rx, _ in ALLOWED_PATHS):
        return []
    return [why for rx, why in EXCLUDED_PATHS if rx.search(path) or (linkname and rx.search(linkname))]


def elf_machine(head):
    """-> (name, allowed) for an ELF header's e_machine, or None when <head> is not ELF."""
    if head[:4] != b'\x7fELF':
        return None
    mach = int.from_bytes(head[18:20], 'little')
    return ELF_ALLOWED.get(mach) or ELF_NAMES.get(mach, str(mach)), mach in ELF_ALLOWED


def prop_findings(props):
    """[(file, reason)] for every ABI or native-bridge property a public image must not carry.

    <props> maps a key to every {'file', 'value'} it was set with, across all prop files."""
    out, checked = [], 0
    for k, vals in props.items():
        for v in vals:
            m = ABILIST.match(k)
            if m:
                checked += 1
                want = '' if m.group(1) == '32' else 'x86_64'
                if v['value'] != want:
                    out.append((v['file'], '%s=%s (a public image advertises x86_64 only)'
                                % (k, v['value'])))
            elif k == 'ro.dalvik.vm.native.bridge' and v['value'] != '0':
                out.append((v['file'], '%s=%s (no native bridge may be configured)'
                            % (k, v['value'])))
            elif k == 'ro.enable.native.bridge.exec' and v['value'] != '0':
                out.append((v['file'], '%s=%s' % (k, v['value'])))
            elif k.startswith('ro.dalvik.vm.isa.arm'):
                out.append((v['file'], '%s=%s (ARM ISA mapping)' % (k, v['value'])))
    # Guard the guard: a renamed property would make the loop above check nothing and pass.
    if not checked:
        out.append(('(props)', 'no ro.*product.cpu.abilist property found — the ABI check '
                    'checked nothing'))
    if not any(v['value'] == 'true' for v in props.get('ro.remora.public_release', [])):
        out.append(('(props)', 'ro.remora.public_release=true is absent — not a public_release '
                    'build'))
    return out


def licence_verdict(lic, names, kinds, conds, reviewed=None):
    """-> ('fail', why), ('review', why) or None for one file's resolved licences."""
    reviewed = REVIEWED if reviewed is None else reviewed
    unreviewed = [n for n in names
                  if n not in reviewed and lic.conditions([n])[1] & REVIEW_CONDITIONS]
    if conds & FATAL_CONDITIONS:
        return 'fail', 'licence %s (%s) is proprietary' % (' '.join(names) or '(text)',
                                                           ' '.join(kinds))
    if unreviewed:
        return 'fail', ('licence %s needs review (by_exception_only / not_allowed) and is not in '
                        'public-licence-review.json' % ' '.join(unreviewed))
    if 'unknown' in conds:
        return 'review', 'licence kind unknown: %s %s' % (' '.join(names), ' '.join(kinds))
    return None


def apk_facts(path, aapt2):
    out = run(aapt2, 'dump', 'packagename', path).strip()
    abis = set()
    with zipfile.ZipFile(path) as z:
        for n in z.namelist():
            m = re.match(r'lib/([^/]+)/[^/]+\.so$', n)
            if m:
                abis.add(m.group(1))
    return out, sorted(abis)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--image', required=True)
    ap.add_argument('--product', required=True)
    ap.add_argument('--compliance', required=True)
    ap.add_argument('--sbom', required=True)
    ap.add_argument('--aapt2', required=True)
    ap.add_argument('--out-dir', required=True)
    a = ap.parse_args()
    global TREE_ROOT
    TREE_ROOT = os.path.abspath(os.path.join(a.product, '../../../..'))

    failures, obligations, review = [], {}, []

    def fail(path, why):
        failures.append({'path': path, 'reason': why})

    info = json.loads(run('docker', 'image', 'inspect', a.image))[0]
    labels = (info.get('Config') or {}).get('Labels') or {}
    if labels.get('remora.public_release') != 'true':
        fail('(image config)', 'missing LABEL remora.public_release="true"')
    if 'org.remora.play.local_only' in labels:
        fail('(image config)', 'a private Play overlay can never be public')
    if info.get('Architecture') != 'amd64' or info.get('Os') != 'linux':
        fail('(image config)', 'platform is %s/%s, not linux/amd64' % (info.get('Os'),
                                                                        info.get('Architecture')))
    # Missing inputs stop the audit; a failed config check does not — the content walk below still
    # runs, so one report names everything that is wrong rather than the first thing.
    missing = [n for n in (a.sbom, os.path.join(a.compliance, 'modules.csv'), a.aapt2)
               if not os.path.exists(n)]
    for need in missing:
        fail(need, 'required input is missing — the audit cannot vouch for what it cannot read')
    if missing:
        return report(a, info, labels, failures, obligations, review, [], {})

    lic = Licences(a.compliance)
    inventory, props = [], {}
    cid = run('docker', 'create', a.image).strip()
    try:
        exp = subprocess.Popen(['docker', 'export', cid], stdout=subprocess.PIPE)
        with tarfile.open(fileobj=exp.stdout, mode='r|') as tar, \
                tempfile.TemporaryDirectory(prefix='remora-audit-') as tmp:
            for m in tar:
                path = m.name
                while path.startswith('./'):
                    path = path[2:]
                path = path.lstrip('/')
                if not path or path in DOCKER_INJECTED:
                    continue
                entry = {'path': path, 'type': 'dir' if m.isdir() else 'symlink' if m.issym()
                         else 'hardlink' if m.islnk() else 'file' if m.isfile() else 'other',
                         'mode': '%o' % m.mode}
                for why in path_findings(path, m.linkname):
                    fail(path, why)
                if m.issym() or m.islnk():
                    entry['target'] = m.linkname
                if m.isfile():
                    f = tar.extractfile(m)
                    h, head, size = hashlib.sha256(), b'', 0
                    keep = path.endswith('.apk') or bool(PROP_FILES.search(path))
                    spool = open(os.path.join(tmp, 'x'), 'wb') if keep else None
                    for chunk in iter(lambda: f.read(1 << 20), b''):
                        if not head:
                            head = chunk[:20]
                        h.update(chunk)
                        size += len(chunk)
                        if spool:
                            spool.write(chunk)
                    entry.update(size=size, sha256=h.hexdigest())
                    elf = elf_machine(head)
                    if elf:
                        entry['elf'] = elf[0]
                        if not elf[1]:
                            fail(path, 'foreign ELF machine %s' % elf[0])
                    if spool:
                        spool.close()
                        if path.endswith('.apk'):
                            pkg, abis = apk_facts(spool.name, a.aapt2)
                            entry.update(package=pkg, abis=abis)
                            if EXCLUDED_PACKAGES.match(pkg):
                                fail(path, 'excluded package %s' % pkg)
                            if abis and 'x86_64' not in abis:
                                review.append({'path': path, 'reason': 'APK has native code '
                                               'only for %s; it cannot run here' % ','.join(abis)})
                        else:
                            with open(spool.name, errors='replace') as pf:
                                for line in pf:
                                    k, sep, v = line.strip().partition('=')
                                    if sep and not k.startswith('#'):
                                        props.setdefault(k, []).append({'file': path, 'value': v})
                    c = lic.classify(path)
                    if c is None:
                        review.append({'path': path, 'reason': 'no licence metadata'})
                    else:
                        module, package, names, kinds, conds = c
                        entry.update(module=module, package=package, licences=names)
                        if EXCLUDED_ORIGINS.match(package or ''):
                            fail(path, 'built from %s, an excluded payload' % package)
                        verdict = licence_verdict(lic, names, kinds, conds)
                        if verdict and verdict[0] == 'fail':
                            fail(path, verdict[1])
                        elif verdict:
                            review.append({'path': path, 'reason': verdict[1]})
                        if conds & SOURCE_CONDITIONS:
                            o = obligations.setdefault(package or module, {
                                'package': package, 'licences': sorted(set(names)),
                                'kinds': sorted(set(kinds)), 'files': 0})
                            o['files'] += 1
                inventory.append(entry)
        if exp.wait() != 0:
            fail('(export)', 'docker export failed — the inventory is incomplete')
    finally:
        subprocess.run(['docker', 'rm', '-f', cid], capture_output=True)

    for where, why in prop_findings(props):
        fail(where, why)
    return report(a, info, labels, failures, obligations, review, inventory, props)


def report(a, info, labels, failures, obligations, review, inventory, props):
    image_id = info['Id']
    out = {
        'schema': 1,
        'audited_at': dt.datetime.now(dt.timezone.utc).isoformat(timespec='seconds'),
        'image': a.image, 'image_id': image_id, 'labels': labels,
        'passed': not failures,
        'failures': failures,
        'copyleft_obligations': sorted(obligations.values(), key=lambda o: o['package']),
        'review': review,
        'checked_props': {k: v for k, v in props.items()
                          if ABILIST.match(k) or 'native.bridge' in k or k.startswith('ro.dalvik.vm.isa')
                          or k.startswith('ro.remora.')},
        'component_inventory_sha256': hashlib.sha256(
            '\n'.join(sorted(e['path'] for e in inventory)).encode()).hexdigest(),
        'files': inventory,
    }
    os.makedirs(a.out_dir, exist_ok=True)
    dest = os.path.join(a.out_dir, image_id.replace(':', '-') + '.audit.json')
    with open(dest, 'w') as f:
        json.dump(out, f, indent=1, sort_keys=True)
    print('public audit: %s %s — %d files, %d failures, %d copyleft packages, %d to review -> %s'
          % (a.image, 'PASSED' if not failures else 'FAILED', len(inventory), len(failures),
             len(obligations), len(review), dest))
    for x in failures[:40]:
        print('  FAIL %s: %s' % (x['path'], x['reason']), file=sys.stderr)
    if len(failures) > 40:
        print('  … %d more in the report' % (len(failures) - 40), file=sys.stderr)
    return 0 if not failures else 1


TREE_ROOT = '.'

if __name__ == '__main__':
    try:
        sys.exit(main())
    except subprocess.CalledProcessError as e:
        # A docker or aapt2 call that fails is a refusal, never a pass: the audit only passes an
        # image it has actually read end to end.
        print('public audit: FAILED — %s exited %d: %s' % (
            ' '.join(e.cmd[:3]), e.returncode, (e.stderr or '').strip()[:300]), file=sys.stderr)
        sys.exit(1)
