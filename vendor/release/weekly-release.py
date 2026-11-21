#!/usr/bin/env python3
"""The weekly public Android image: refresh, build, check, publish (bd remora-47jx).

    weekly-release.py run [--config FILE] [--plan] [--no-publish] [--no-refresh] [--force]
    weekly-release.py seed-mesa --from DIR [--config FILE]
    weekly-release.py install-timer [--config FILE] [--calendar 'Sun *-*-* 01:30:00'] [--remove]
    weekly-release.py status [--config FILE]

The owner's call (2026-11-21, asked): every week, move Android 17 to the LineageOS branch heads,
and fall back to the pin the published image was built from when the move fails; publish on its
own once every check has passed. `package-release.py` stays the one tool that packages, signs
and uploads, and this program is the schedule around it.

ONE RUN, in order. Each step either passes or ends the attempt with a reason:

 1. The PUBLIC repository's main, in a clone of its own (never the development checkout): the
    image is built from exactly the source the release then publishes as Remora's corresponding
    source, and a public tree that cannot build the public image is found here, not by a user.
 2. The live catalog, verified with the key the clone ships. Its provenance records the digest of
    the inputs that built the listed image, which is how "nothing changed" is decided.
 3. The refresh: the tree is synced to default.xml plus Remora's local manifests and snapshotted
    with `repo manifest -r` BEFORE any patch pass, which is the documented way to make a pin (a
    pin taken after the patch pass names commits no remote has). A snapshot that drops a project,
    names a revision that is not a commit id, or moves Mesa or libdrm (the Mesa payload is built
    against both) is not used.
 4. Up to two candidates: the refreshed pin when it moved, then the BASE pin (the one the
    published image was built from) when the refresh failed or did not move and Remora's own
    inputs did. The base is never the repository's older pin once a refresh has shipped, because
    falling back to it would publish an older Android than the one users already run. Each is a
    pinned `remora build --source` of a sandboxed public_release profile, followed by these gates:
    the build's own verdict (the release audit runs inside it, in assemble-image.sh); no patch
    skipped that the last good build applied; a headless boot of the image (boot_completed,
    Android 17, x86_64 only, the agent running, system_server steady, no fatal system crash).
 5. package-release.py build, publish, a check that GitHub serves the new catalog, and prune.
 6. With nothing to build, the catalog is re-signed when it expires within resign_within_days.

A failure publishes nothing and goes to the tracker as ONE open bead per kind (label
image-release), noted again on each later failure rather than filed again. The last line printed
is JSON: {"state": "published" | "resigned" | "nothing" | "deferred" | "failed", ...}.
"""

import argparse
import configparser
import datetime as dt
import fcntl
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import signal
import subprocess
import sys
import time
import xml.etree.ElementTree as ET

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('catalog', HERE / 'catalog.py')
catalog = importlib.util.module_from_spec(spec)
spec.loader.exec_module(catalog)

TAG = 'remora24:x86_64-public'
BUILT_TAG = TAG + '-src'          # sourceBuiltTag() for a public_release Android 17 build
PROFILE = 'weekly'                 # the sandboxed remorarc's one instance
PIN = 'vendor/source-build/pinned-manifest-a17.xml'
MESA_PAYLOAD = 'vendor/source-build/features/mesa_source/lib64'
# The payload is a host-side build against these two projects (build-mesa.sh): when either moves,
# the cached payload no longer matches the tree, and it has to be rebuilt by hand.
MESA_PROJECTS = ('external/mesa3d', 'external/libdrm')
# What goes into the image from Remora's side. A change anywhere else (the desktop client, the
# docs) does not make a new image.
IMAGE_INPUTS = ('agent', 'vendor/source-build', 'vendor/source-patches',
                'vendor/host-prereqs/mesa-android')
SMOKE = 'remora-image-release-smoke'

DEFAULTS = {
    'source_url': 'https://github.com/EtherAura/Remora.git',
    'branch': 'main',
    'repo': 'EtherAura/Remora',
    'release': 'android-images',
    'tree': '',
    'key': '',
    'work': '~/.local/share/remora/image-release',
    'releases': '',
    'lineage_url': 'https://github.com/LineageOS/android.git',
    'lineage_branch': 'lineage-24.0',
    'refresh': 'yes',
    'keep_previous': '1',
    'keep_local': '2',
    'resign_within_days': '21',
    'min_free_gib': '80',
    'boot_timeout': '900',
    'defer_while': '',
    'defer_max_hours': '12',
    'lock_command': '',
    'notify': 'yes',
    'tracker': '',
    'bd': 'bd',
}
PROFILE_DEFAULTS = {
    'android_version': '17',
    'features': 'camera_v4l2 container_compat_patches hw_video_decode lineage_frameworks '
                'mesa_source mirror_agent public_release sensors system_updater usb_audio',
    'source_ref': 'lineage-24.0',
}


class Failed(Exception):
    """One attempt (or the run) cannot continue. `stage` names the gate that refused."""

    def __init__(self, stage, detail):
        super().__init__('%s: %s' % (stage, detail))
        self.stage, self.detail = stage, detail


# ---------------------------------------------------------------------------------------------
# pure parts (tests/test_image_release.py)
# ---------------------------------------------------------------------------------------------

def pin_projects(xml_bytes):
    """{path: revision} of a repo manifest."""
    root = ET.fromstring(xml_bytes)
    return {p.get('path') or p.get('name'): p.get('revision') or '' for p in root.iter('project')}


def compare_pins(old, new):
    changed = sorted((p, old[p], new[p]) for p in old.keys() & new.keys() if old[p] != new[p])
    return {'changed': changed, 'added': sorted(new.keys() - old.keys()),
            'removed': sorted(old.keys() - new.keys())}


def refresh_refusal(diff, new):
    """Why a refreshed snapshot may not be built, or None. The snapshot is the new pin, so it has
    to be one every later build can sync: complete, and naming commits only."""
    if diff['removed']:
        return ('the snapshot lacks %d project(s) the base pin has (%s): a pin that drops a '
                'project needs a person to say upstream really removed it'
                % (len(diff['removed']), ', '.join(diff['removed'][:6])))
    bad = sorted(p for p, r in new.items() if not re.fullmatch(r'[0-9a-f]{40}', r))
    if bad:
        return 'the snapshot names %d revision(s) that are not commit ids (%s)' % (
            len(bad), ', '.join(bad[:6]))
    moved = [p for p, _, _ in diff['changed'] if p in MESA_PROJECTS]
    if moved:
        return ('upstream moved %s, which the Mesa payload is built against: rebuild it with '
                'vendor/host-prereqs/mesa-android/build-mesa.sh on the refreshed tree, then '
                '`weekly-release.py seed-mesa --pin <refreshed manifest> --from <payload>`'
                % ' and '.join(moved))
    return None


SKIP_LINES = (
    (re.compile(r'WARN patch SKIPPED \(conflicts\), tree left clean: (\S+)'), 'patch'),
    (re.compile(r'WARN backport patch SKIPPED \(conflicts\): (.+?)\s*$'), 'backport'),
    (re.compile(r'\*\*\*WARN could not apply (\S+) .*SKIPPED'), 'container'),
)


def skipped_patches(log_text):
    """Every patch the patch pass skipped. Some of its skips are warnings, not failures, so a
    build can end green without a patch: this is how the job sees that."""
    out = set()
    for line in log_text.splitlines():
        for rx, kind in SKIP_LINES:
            m = rx.search(line)
            if m:
                out.add('%s:%s' % (kind, m.group(1).strip()))
    return out


def new_skips(skipped, baseline):
    """The skips the last good build did not have. With no baseline yet (the first run), only a
    build of the base pin may set one: a refreshed pin has to prove it applies everything."""
    return sorted(skipped - set(baseline or ()))


def mesa_key(pin):
    return hashlib.sha256(json.dumps({p: pin.get(p, '') for p in MESA_PROJECTS},
                                     sort_keys=True).encode()).hexdigest()[:16]


def inputs_digest(pin, trees, profile):
    """One digest over everything that decides the image's bytes: the manifest, Remora's image
    inputs at the commit, and the build profile."""
    blob = json.dumps({'pin': sorted(pin.items()), 'trees': sorted(trees.items()),
                       'profile': sorted(profile.items()), 'mesa': mesa_key(pin)},
                      sort_keys=True)
    return hashlib.sha256(blob.encode()).hexdigest()


def plan_candidates(refreshed, base_digest, published_digest, force=False):
    """The pins to try, in order, stopping at the first that passes: the refreshed one when the
    refresh moved upstream, then the base when its inputs differ from the published image's.
    A failed refresh over an unchanged base has nothing new to publish (the bead says why)."""
    out = ['refreshed'] if refreshed else []
    if force or base_digest != published_digest:
        out.append('base')
    return out


def needs_resign(index, now, within_days):
    expires = dt.datetime.fromisoformat(index['expires_at'])
    return expires - now <= dt.timedelta(days=within_days)


def choose_base(state, committed_sha, committed, published_manifest, stored_base):
    """The pin a fallback builds. The repository's pin wins when it changed since the job last
    looked (somebody repinned on purpose); otherwise the pin the PUBLISHED image was built from,
    read from the release itself, so a run that died after uploading cannot leave the job falling
    back to an older Android than the one users run. The job's own copy is for a release that
    predates the manifest asset."""
    if state.get('committed_pin_sha256') not in (None, committed_sha):
        return committed, 'the repository repinned'
    if published_manifest is not None:
        return published_manifest, 'the published release manifest'
    if stored_base is not None:
        return stored_base, 'the last published build'
    return committed, 'the repository pin'


def release_name(version_tag):
    return version_tag.replace(':', '-')


def systemd_units(python, script, config, clone, branch, calendar):
    service = (
        '[Unit]\n'
        'Description=Remora weekly public Android image (refresh, build, check, publish)\n'
        'Documentation=file://%(clone)s/docs/IMAGE_MIRROR.md\n'
        'After=network-online.target\n\n'
        '[Service]\n'
        'Type=oneshot\n'
        'Nice=10\n'
        'IOSchedulingClass=idle\n'
        'TimeoutStartSec=20h\n'
        '# The job runs the published version of itself: the clone is the public main.\n'
        'ExecStartPre=/usr/bin/git -C %(clone)s fetch -q origin %(branch)s\n'
        'ExecStartPre=/usr/bin/git -C %(clone)s checkout -q --detach -f FETCH_HEAD\n'
        'ExecStart=%(python)s -I %(script)s run --config %(config)s\n'
    ) % {'clone': clone, 'branch': branch, 'python': python, 'script': script, 'config': config}
    timer = (
        '[Unit]\n'
        'Description=Remora weekly public Android image\n\n'
        '[Timer]\n'
        'OnCalendar=%s\n'
        'Persistent=true\n\n'
        '[Install]\n'
        'WantedBy=timers.target\n'
    ) % calendar
    return service, timer


# ---------------------------------------------------------------------------------------------
# config and processes
# ---------------------------------------------------------------------------------------------

def default_config_path():
    base = os.environ.get('XDG_CONFIG_HOME') or str(Path.home() / '.config')
    return Path(base) / 'remora' / 'image-release.conf'


def load_config(path):
    cp = configparser.ConfigParser(interpolation=None)
    cp.optionxform = str
    if not cp.read(path):
        raise Failed('config', 'no config at %s (see docs/IMAGE_MIRROR.md, "Weekly releases")' % path)
    r = dict(DEFAULTS)
    r.update(cp['release'] if cp.has_section('release') else {})
    prof = dict(PROFILE_DEFAULTS)
    prof.update(cp['profile'] if cp.has_section('profile') else {})
    for k in ('tree', 'key', 'work', 'releases', 'tracker'):
        r[k] = os.path.expanduser(r[k]) if r[k] else ''
    for k in ('tree', 'key'):
        if not r[k]:
            raise Failed('config', '%s= is required in [release] of %s' % (k, path))
    r['work'] = Path(r['work'])
    r['releases'] = Path(r['releases']) if r['releases'] else r['work'] / 'releases'
    r['profile'] = prof
    r['config_path'] = Path(path)
    return r


def yes(v):
    return str(v).strip().lower() in ('1', 'yes', 'true', 'on')


class Log:
    def __init__(self, path):
        self.path = path
        path.parent.mkdir(parents=True, exist_ok=True)
        self.fh = open(path, 'a', buffering=1)

    def __call__(self, msg):
        line = '[%s] %s' % (dt.datetime.now().strftime('%F %T'), msg)
        print(line, flush=True)
        self.fh.write(line + '\n')


def run(argv, cwd=None, env=None, check=True, timeout=None, input_=None):
    p = subprocess.run(argv, cwd=cwd, env=env, capture_output=True, text=True, timeout=timeout,
                       input=input_, stdin=None if input_ is not None else subprocess.DEVNULL)
    if check and p.returncode:
        tail = (p.stderr.strip() or p.stdout.strip())[-600:]
        raise Failed('command', '%s exited %d: %s' % (' '.join(argv[:4]), p.returncode, tail))
    return p


_child = None


def _forward(signum, _frame):
    if _child is not None and _child.poll() is None:
        try:
            os.killpg(_child.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
    raise SystemExit(128 + signum)


def stream(argv, logfile, cwd=None, env=None):
    """A long command in its own process group, its output appended to `logfile`. SIGTERM to this
    program reaches the whole group (remora tears its build chain down on SIGTERM)."""
    global _child
    with open(logfile, 'ab') as out:
        _child = subprocess.Popen(argv, cwd=cwd, env=env, stdout=out, stderr=subprocess.STDOUT,
                                  stdin=subprocess.DEVNULL, start_new_session=True)
        try:
            return _child.wait()
        finally:
            _child = None


def locked(cfg, argv):
    """`argv` under the host's one-build-at-a-time lock, when lock_command names one. Per heavy
    step, not for the whole run: a run waiting on defer_while or a sync must not hold every other
    build on the host."""
    return shlex.split(cfg['lock_command']) + list(argv) if cfg['lock_command'].strip() else argv


def git(repo, *args, check=True):
    return run(['git', '-C', str(repo), *args], check=check).stdout.strip()


def free_gib(path):
    st = os.statvfs(path)
    return st.f_bavail * st.f_frsize / 1024**3


def notify(cfg, summary, body):
    if yes(cfg['notify']) and shutil.which('notify-send'):
        subprocess.run(['notify-send', '--app-name=Remora', summary, body[:400]],
                       capture_output=True, timeout=10)


# ---------------------------------------------------------------------------------------------
# state
# ---------------------------------------------------------------------------------------------

def load_state(work):
    f = work / 'state.json'
    return json.loads(f.read_text()) if f.exists() else {}


def save_state(work, state):
    work.mkdir(parents=True, exist_ok=True)
    catalog.atomic_json(work / 'state.json', state)


# ---------------------------------------------------------------------------------------------
# steps
# ---------------------------------------------------------------------------------------------

def update_clone(cfg, log):
    src = cfg['work'] / 'src'
    if not (src / '.git').exists():
        log('cloning %s' % cfg['source_url'])
        src.parent.mkdir(parents=True, exist_ok=True)
        run(['git', 'clone', '-q', '--no-tags', cfg['source_url'], str(src)])
    git(src, 'fetch', '-q', '--prune', 'origin', cfg['branch'])
    git(src, 'checkout', '-q', '--detach', '-f', 'FETCH_HEAD')
    git(src, 'clean', '-fdq')  # untracked only: the build dir and the Mesa payload are ignored
    return src, git(src, 'rev-parse', 'HEAD')


def image_trees(src):
    return {p: git(src, 'rev-parse', 'HEAD:' + p, check=False) or '-' for p in IMAGE_INPUTS}


def published_release(cfg, src, run_dir):
    """The live catalog, authenticated, with the provenance and manifest of the image it lists."""
    base = 'https://github.com/%s/releases/download/%s' % (cfg['repo'], cfg['release'])
    data = catalog.get(base + '/index.json', 1024 * 1024)
    sig = catalog.get(base + '/index.json.sig', 64)
    key = src / 'vendor/release/image-signing-key.pem'
    expired = False
    try:
        index, _ = catalog.verify_catalog(data, sig, key)
    except catalog.VerificationError as e:
        if 'expired' not in str(e):
            raise
        # Still authenticate it: an expired catalog is the job's to renew, a forged one is not.
        published = dt.datetime.fromisoformat(json.loads(data)['published_at'])
        index, _ = catalog.verify_catalog(data, sig, key, now=published + dt.timedelta(seconds=1))
        expired = True
    (run_dir / 'published-index.json').write_bytes(data)
    (run_dir / 'published-index.json.sig').write_bytes(sig)
    entry = next((im for im in index['images'] if im['tag'] == TAG), None)
    prov = manifest = None
    if entry:
        prov = json.loads(fetch_asset(base, entry['provenance'], 16 * 1024 * 1024))
        ref = next((s for s in entry.get('sources', []) if s.get('project') == 'manifest'), None)
        if ref:
            manifest = fetch_asset(base, ref, 16 * 1024 * 1024)
    return {'index': index, 'entry': entry, 'provenance': prov or {}, 'manifest': manifest,
            'expired': expired}


def fetch_asset(base, ref, limit):
    data = catalog.get(base + '/' + ref['file'], limit)
    catalog.require(hashlib.sha256(data).hexdigest() == ref['sha256'],
                    '%s does not match the digest the catalog signs' % ref['file'])
    return data


def wait_until_idle(cfg, log):
    cmd = cfg['defer_while'].strip()
    if not cmd:
        return None
    deadline = time.monotonic() + float(cfg['defer_max_hours']) * 3600
    while True:
        busy = subprocess.run(['sh', '-c', cmd], capture_output=True, text=True).stdout.strip()
        if not busy:
            return None
        if time.monotonic() >= deadline:
            return busy
        log('waiting: `%s` says %s' % (cmd, ' '.join(busy.split())[:200]))
        time.sleep(600)


def preflight(cfg):
    tree = Path(cfg['tree'])
    if not (tree / '.repo').is_dir():
        raise Failed('preflight', '%s is not a repo tree' % tree)
    if not Path(cfg['key']).is_file():
        raise Failed('preflight', 'no signing key at %s' % cfg['key'])
    for where in (tree, cfg['releases']):
        Path(where).mkdir(parents=True, exist_ok=True)
        if free_gib(where) < float(cfg['min_free_gib']):
            raise Failed('preflight', '%s has %.0f GiB free, under min_free_gib=%s'
                         % (where, free_gib(where), cfg['min_free_gib']))
    busy = run(['docker', 'ps', '--filter', 'ancestor=remora-builder:latest', '--format',
                '{{.Names}}'], check=False).stdout.strip()
    if busy:
        raise Failed('preflight', 'a source build is already running (%s)' % busy)


def refresh(cfg, src, run_dir, log):
    """Sync the tree to the branch heads and snapshot them. Returns the snapshot's bytes."""
    tree = cfg['tree']
    rlog = run_dir / 'refresh.log'
    log('refresh: resetting the tree and syncing %s %s' % (cfg['lineage_url'], cfg['lineage_branch']))
    # FIRST, before a project moves: the build chain trusts its patch-state record and, when the
    # record matches the pin, re-syncs only the projects that carry patches. Every other project
    # would then stay at the heads this refresh checks out while the build claims the base pin.
    # Without the record the next build does its full pinned sync (the documented remedy).
    for f in ('.remora-patch-state', '.remora-incremental'):
        (Path(tree) / f).unlink(missing_ok=True)
    # A previous build leaves patch commits (and, after a failure, a half-applied am) in projects.
    # Detached, those commits are discarded by the sync below; an am in progress blocks it.
    steps = [
        ['repo', 'forall', '-j8', '-c', 'git am --abort >/dev/null 2>&1; git rebase --abort '
                                        '>/dev/null 2>&1; git reset -q --hard'],
        ['repo', 'init', '-q', '-u', cfg['lineage_url'], '-b', cfg['lineage_branch'], '-m',
         'default.xml'],
    ]
    for argv in steps:
        if stream(argv, rlog, cwd=tree):
            raise Failed('refresh', '`%s` failed (see %s)' % (' '.join(argv[:3]), rlog))
    local = Path(tree) / '.repo/local_manifests'
    local.mkdir(exist_ok=True)
    for f in local.glob('*.xml'):
        f.unlink()
    for f in sorted((src / 'vendor/source-build/local_manifests').glob('remora-a17*.xml')):
        shutil.copy(f, local / f.name)
    if stream(['repo', 'sync', '-c', '-d', '-j8', '--fail-fast', '--force-sync'], rlog, cwd=tree):
        raise Failed('refresh', 'repo sync to the branch heads failed (see %s)' % rlog)
    # The sync's outcome, not its exit code (bd remora-4ei.81): every project's HEAD must be the
    # revision the manifest resolves to, or the snapshot below records a half-synced tree. The
    # same check as repoSyncVerifyCommand(), with the offenders listed instead of the first.
    out = run(['repo', 'forall', '-j8', '-c', 'h=$(git rev-parse HEAD 2>/dev/null); '
               'm=$(git rev-parse "$REPO_LREV" 2>/dev/null); [ -n "$h" ] && [ "$h" = "$m" ] || '
               'echo "OFF $REPO_PATH"'], cwd=tree, check=False).stdout
    off = [ln.split(None, 1)[1] for ln in out.splitlines() if ln.startswith('OFF ')]
    if off:
        raise Failed('refresh', '%d project(s) are not at their manifest revision after the sync '
                     '(%s)' % (len(off), ', '.join(off[:6])))
    snap = run_dir / 'refreshed-manifest.xml'
    run(['repo', 'manifest', '-r', '-o', str(snap)], cwd=tree)
    return snap.read_bytes()


def mesa_cache(cfg, pin):
    return cfg['work'] / 'mesa' / mesa_key(pin)


def install_mesa(cfg, src, pin):
    cache = mesa_cache(cfg, pin) / 'lib64'
    if not cache.is_dir():
        raise Failed('mesa', 'no Mesa payload cached for %s (key %s): run `weekly-release.py '
                     'seed-mesa --from <payload dir>`' % (
                         ', '.join('%s@%s' % (p, pin.get(p, '?')[:12]) for p in MESA_PROJECTS),
                         mesa_key(pin)))
    dest = src / MESA_PAYLOAD
    shutil.rmtree(dest, ignore_errors=True)
    shutil.copytree(cache, dest, symlinks=True)


def remora_binary(cfg, src, log):
    build = src / 'build-release'
    log('building remora from the public tree')
    blog = cfg['work'] / 'remora-build.log'
    for argv in (['cmake', '-S', str(src), '-B', str(build), '-G', 'Ninja',
                  '-DCMAKE_BUILD_TYPE=Release', '-DREMORA_INSTALLED_VENDOR=OFF'],
                 ['cmake', '--build', str(build), '--target', 'remora']):
        if stream(locked(cfg, argv), blog):
            raise Failed('remora', '`%s` failed (see %s)' % (' '.join(argv[:3]), blog))
    return build / 'remora'


def sandbox_env(cfg):
    box = cfg['work'] / 'sandbox'
    env = dict(os.environ)
    for k, sub in (('XDG_CONFIG_HOME', 'config'), ('XDG_DATA_HOME', 'data'),
                   ('XDG_CACHE_HOME', 'cache')):
        (box / sub).mkdir(parents=True, exist_ok=True)
        env[k] = str(box / sub)
    prof = dict(cfg['profile'])
    prof.update(source_tree=cfg['tree'], source_pinned='true', android_version='17')
    lines = ['[Instance-%s]' % PROFILE] + ['%s=%s' % kv for kv in sorted(prof.items())]
    (box / 'config' / 'remorarc').write_text('\n'.join(lines) + '\n')
    return env


def boot_test(image_id, run_dir, label, timeout, log):
    """Boot the image headless on the default bridge with SwiftShader, and ask it the questions a
    broken image answers wrongly. Removes its container whatever happens."""
    blog = run_dir / ('boot-%s.log' % label)
    run(['docker', 'rm', '-f', SMOKE], check=False)
    run(['docker', 'run', '-d', '--privileged', '--name', SMOKE, '--label',
         'remora.image-release=smoke', image_id,
         'androidboot.remora_gpu_mode=guest', 'androidboot.remora_vulkan=pastel',
         'androidboot.use_memfd=true', 'androidboot.remora_width=1280',
         'androidboot.remora_height=720', 'androidboot.remora_dpi=240',
         'androidboot.remora_fps=30'])

    def sh(cmd):
        try:
            return run(['docker', 'exec', SMOKE, 'sh', '-c', cmd], check=False,
                       timeout=60).stdout.strip()
        except subprocess.TimeoutExpired:
            return ''  # a hung exec answers nothing; the caller's own deadline decides

    def running():
        return run(['docker', 'inspect', '-f', '{{.State.Running}}', SMOKE],
                   check=False).stdout.strip() == 'true'

    try:
        start = time.monotonic()
        while sh('getprop sys.boot_completed') != '1':
            if not running():
                raise Failed('boot', 'the container exited before boot completed')
            if time.monotonic() - start > timeout:
                raise Failed('boot', 'sys.boot_completed not set after %ds' % timeout)
            time.sleep(10)
        log('boot: completed in %ds' % (time.monotonic() - start))
        facts = {k: sh('getprop ' + k) for k in ('ro.build.version.sdk', 'ro.product.cpu.abilist',
                                                 'ro.build.version.incremental',
                                                 'ro.build.version.security_patch')}
        if facts['ro.build.version.sdk'] != '37':
            raise Failed('boot', 'SDK is %r, not 37' % facts['ro.build.version.sdk'])
        if facts['ro.product.cpu.abilist'] != 'x86_64':
            raise Failed('boot', 'abilist is %r, not x86_64 alone' % facts['ro.product.cpu.abilist'])
        deadline = time.monotonic() + 120
        while sh('getprop init.svc.remora_agent') != 'running':
            if time.monotonic() > deadline:
                raise Failed('boot', 'the Remora agent is not running (init.svc.remora_agent=%r)'
                             % sh('getprop init.svc.remora_agent'))
            time.sleep(5)
        if not sh('pm path android').startswith('package:'):
            raise Failed('boot', 'the package manager does not answer')
        first = sh('pidof system_server')
        time.sleep(30)
        if not first or sh('pidof system_server') != first:
            raise Failed('boot', 'system_server restarted after boot (%r)' % first)
        crashes = sh('logcat -d -b crash')
        (run_dir / ('crash-%s.txt' % label)).write_text(crashes)
        if 'FATAL EXCEPTION IN SYSTEM PROCESS' in crashes:
            raise Failed('boot', 'a fatal system crash after boot (crash-%s.txt)' % label)
        return facts
    finally:
        with open(blog, 'w') as f:
            subprocess.run(['docker', 'logs', '--tail', '400', SMOKE], stdout=f,
                           stderr=subprocess.STDOUT)
        run(['docker', 'rm', '-f', SMOKE], check=False)


def attempt(cfg, ctx, label, pin_bytes, log):
    """Build one candidate and put it through every gate. Returns what packaging needs."""
    src, run_dir = ctx['src'], ctx['run_dir']
    pin = pin_projects(pin_bytes)
    pin_file = run_dir / ('%s-manifest.xml' % label)
    pin_file.write_bytes(pin_bytes)
    git(src, 'checkout', '-q', '-f', '--', PIN)
    (src / PIN).write_bytes(pin_bytes)
    install_mesa(cfg, src, pin)
    preflight(cfg)
    env = sandbox_env(cfg)
    blog = run_dir / ('build-%s.log' % label)
    log('%s: remora build --source (log %s)' % (label, blog))
    rc = stream(locked(cfg, [str(ctx['remora']), 'build', '--source', PROFILE]), blog, env=env)
    chain = Path('/tmp/remora-build.log')
    if chain.exists():
        shutil.copy(chain, run_dir / ('chain-%s.log' % label))
    text = blog.read_text(errors='replace')
    if rc:
        tail = ' | '.join(ln.strip() for ln in text.strip().splitlines()[-4:])
        raise Failed('build', 'exit %d: %s' % (rc, tail[-500:]))
    skipped = skipped_patches(text)
    baseline = ctx['state'].get('skipped_patches')
    if baseline is None and label == 'base':
        baseline = sorted(skipped)  # the first good build of the base pin sets the baseline
    extra = new_skips(skipped, baseline)
    if extra:
        raise Failed('patches', '%d patch(es) skipped that the last good build applied: %s'
                     % (len(extra), ', '.join(extra[:8])))
    image_id = run(['docker', 'image', 'inspect', '--format', '{{.Id}}', BUILT_TAG]).stdout.strip()
    facts = boot_test(image_id, run_dir, label, int(cfg['boot_timeout']), log)
    log('%s: boots, %s' % (label, json.dumps(facts, sort_keys=True)))
    return {'label': label, 'image_id': image_id, 'pin_file': pin_file, 'pin_bytes': pin_bytes,
            'skipped': sorted(skipped), 'facts': facts,
            'digest': inputs_digest(pin, ctx['trees'], cfg['profile'])}


def package_and_publish(cfg, ctx, built, publish, log):
    src, run_dir = ctx['src'], ctx['run_dir']
    out = cfg['releases'] / ('%s-%s' % (ctx['stamp'], built['label']))
    tree_dir = run_dir / 'public-tree'
    pr = [sys.executable, '-I', str(src / 'vendor/release/package-release.py')]
    git(src, 'worktree', 'prune')
    git(src, 'worktree', 'add', '-q', '--detach', str(tree_dir), ctx['commit'])
    try:
        log('packaging %s into %s' % (built['image_id'][:19], out))
        plog = run_dir / 'package.log'
        argv = pr + ['build', '--image', built['image_id'], '--tree', cfg['tree'],
                     '--public-tree', str(tree_dir), '--out', str(out), '--key', cfg['key'],
                     '--previous-index', str(run_dir / 'published-index.json'),
                     '--manifest', str(built['pin_file']), '--inputs-digest', built['digest']]
        if stream(locked(cfg, argv), plog):  # xz -T0 over 2.4 GB: a build in all but name
            raise Failed('package', 'package-release.py build failed (see %s)' % plog)
    finally:
        git(src, 'worktree', 'remove', '--force', str(tree_dir), check=False)
    index = json.loads((out / 'index.json').read_text())
    entry = next(im for im in index['images'] if im['tag'] == TAG)
    if not publish:
        return {'state': 'built', 'version_tag': entry['version_tag'], 'out': str(out)}
    log('publishing %s' % entry['version_tag'])
    if stream(pr + ['publish', '--out', str(out), '--repo', cfg['repo'],
                    '--release', cfg['release']], run_dir / 'publish.log'):
        raise Failed('publish', 'package-release.py publish failed (see %s)'
                     % (run_dir / 'publish.log'))
    return {'state': 'published', 'version_tag': entry['version_tag'], 'out': str(out),
            'sequence': index['sequence'], 'entry': entry}


def after_publish(cfg, ctx, res, log):
    """Check what GitHub serves, then prune. The release is out either way, so a problem here is
    reported beside it rather than turning it into a failure."""
    src, run_dir, out = ctx['src'], ctx['run_dir'], Path(res['out'])
    pr = [sys.executable, '-I', str(src / 'vendor/release/package-release.py')]
    try:
        verify_live(cfg, src, out, res.pop('entry'))
    except (Failed, catalog.VerificationError, OSError) as e:
        return 'published, but the live catalog did not check out (%s); not pruned' % e
    keep = []
    old = ctx['published'].get('entry')
    if old and int(cfg['keep_previous']) > 0:
        keep.append(release_name(old['version_tag']))
    if stream(pr + ['prune', '--out', str(out), '--repo', cfg['repo'], '--release',
                    cfg['release']] + sum((['--keep', k] for k in keep), []),
              run_dir / 'publish.log'):
        return 'published; pruning old assets failed (see %s)' % (run_dir / 'publish.log')
    log('live catalog verified, old assets pruned')
    return None


def verify_live(cfg, src, out, entry):
    """GitHub serves the catalog just uploaded, and it verifies as a client would see it."""
    base = 'https://github.com/%s/releases/download/%s' % (cfg['repo'], cfg['release'])
    for _ in range(6):
        data = catalog.get(base + '/index.json', 1024 * 1024)
        if data == (out / 'index.json').read_bytes():
            break
        time.sleep(20)  # the download CDN can serve the old file for a little while
    else:
        raise Failed('publish', 'GitHub still serves the previous index.json')
    m, _ = catalog.verify_catalog(data, catalog.get(base + '/index.json.sig', 64),
                                  src / 'vendor/release/image-signing-key.pem')
    live = next(im for im in m['images'] if im['tag'] == TAG)
    catalog.require(live['version_tag'] == entry['version_tag'], 'the live catalog lists %s'
                    % live['version_tag'])


def resign(cfg, ctx, log):
    src, run_dir = ctx['src'], ctx['run_dir']
    out = cfg['releases'] / ('%s-resign' % ctx['stamp'])
    out.mkdir(parents=True)
    shutil.copy(run_dir / 'published-index.json', out / 'index.json')
    pr = [sys.executable, '-I', str(src / 'vendor/release/package-release.py')]
    log('re-signing the catalog (expires %s)' % ctx['published']['index']['expires_at'])
    for argv in (pr + ['resign', '--out', str(out), '--key', cfg['key']],
                 pr + ['publish', '--out', str(out), '--repo', cfg['repo'], '--release',
                       cfg['release'], '--catalog-only']):
        if stream(argv, run_dir / 'publish.log'):
            raise Failed('resign', '`%s` failed (see %s)' % (argv[3], run_dir / 'publish.log'))
    return {'state': 'resigned', 'out': str(out)}


def tidy_local(cfg, state, log):
    """Keep the newest keep_local release dirs and the version tags the job made for them."""
    keep = int(cfg['keep_local'])
    dirs = sorted((d for d in cfg['releases'].iterdir() if d.is_dir()), reverse=True)
    for d in dirs[keep:]:
        shutil.rmtree(d, ignore_errors=True)
    made = state.get('made_tags', [])
    for tag in made[:-keep] if len(made) > keep else []:
        # Untag only (never -f): docker refuses while a container uses the image, and an image
        # with another tag (the deployed public tag, a rollback) keeps its bytes.
        r = run(['docker', 'rmi', tag], check=False)
        log('local: %s %s' % ('removed' if not r.returncode else 'kept', tag))
        if not r.returncode or 'No such image' in r.stderr:
            made.remove(tag)
    state['made_tags'] = made


def file_bead(cfg, kind, title, body, log):
    """One open bead per kind: a repeat failure becomes a note on it, not a new bead."""
    if not cfg['tracker']:
        return None
    bd = [cfg['bd']]
    try:
        rows = json.loads(run(bd + ['list', '--label', 'image-release', '--status', 'open',
                                    '--json'], cwd=cfg['tracker']).stdout or '[]')
        prefix = 'Weekly image release: %s' % kind
        hit = next((r for r in rows if r.get('title', '').startswith(prefix)), None)
        if hit:
            run(bd + ['note', hit['id'], body], cwd=cfg['tracker'])
            return hit['id']
        return run(bd + ['create', '%s — %s' % (prefix, title), '-t', 'bug', '-p', '2', '-l',
                         'image-release', '-d', body, '--silent'],
                   cwd=cfg['tracker']).stdout.strip()
    except (Failed, OSError, ValueError) as e:
        log('tracker: could not file the failure: %s' % e)
        return None


# ---------------------------------------------------------------------------------------------
# commands
# ---------------------------------------------------------------------------------------------

def cmd_run(a):
    cfg = load_config(a.config)
    work = cfg['work']
    work.mkdir(parents=True, exist_ok=True)
    lock = open(work / 'run.lock', 'a')
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        return report({'state': 'failed', 'detail': 'another run holds %s' % (work / 'run.lock')})
    stamp = dt.datetime.now(dt.timezone.utc).strftime('%Y%m%dT%H%M%SZ')
    run_dir = work / 'runs' / stamp
    run_dir.mkdir(parents=True)
    log = Log(run_dir / 'run.log')
    state = load_state(work)
    result = {'state': 'failed', 'run': str(run_dir)}
    try:
        result.update(one_run(a, cfg, state, stamp, run_dir, log))
    except Failed as e:
        result.update(stage=e.stage, detail=e.detail)
        log('FAILED at %s: %s' % (e.stage, e.detail))
        if not a.plan:
            bead = file_bead(cfg, 'no release', e.stage, '%s\n\nRun: %s' % (e, run_dir), log)
            result['bead'] = bead
    except Exception as e:  # noqa: BLE001 - an unattended job reports every failure it can
        result.update(stage='run', detail='%s: %s' % (e.__class__.__name__, e))
        log('FAILED: %s' % result['detail'])
    state['last_run'] = {k: result.get(k) for k in ('state', 'stage', 'detail', 'version_tag',
                                                     'run')} | {'at': stamp}
    if not a.plan:
        save_state(work, state)
        if result['state'] in ('published', 'failed', 'resigned'):
            notify(cfg, 'Remora image: %s' % result['state'],
                   result.get('version_tag') or result.get('detail', ''))
    (run_dir / 'result.json').write_text(json.dumps(result, indent=2, sort_keys=True) + '\n')
    return report(result)


def one_run(a, cfg, state, stamp, run_dir, log):
    busy = None if a.plan else wait_until_idle(cfg, log)
    if busy:
        return {'state': 'deferred', 'detail': '`%s` still reports: %s'
                % (cfg['defer_while'], ' '.join(busy.split())[:200])}
    src, commit = update_clone(cfg, log)
    log('public main %s' % commit[:12])
    published = published_release(cfg, src, run_dir)
    entry = published['entry']
    log('published: %s (catalog sequence %d, expires %s%s)' % (
        entry['version_tag'] if entry else 'nothing', published['index']['sequence'],
        published['index']['expires_at'], ', EXPIRED' if published['expired'] else ''))
    committed = (src / PIN).read_bytes()
    committed_sha = hashlib.sha256(committed).hexdigest()
    stored = (cfg['work'] / 'base-pin.xml')
    base_bytes, why = choose_base(state, committed_sha, committed, published['manifest'],
                                  stored.read_bytes() if stored.exists() else None)
    trees = image_trees(src)
    base_pin = pin_projects(base_bytes)
    base_digest = inputs_digest(base_pin, trees, cfg['profile'])
    published_digest = published['provenance'].get('inputs_sha256')
    log('base pin: %s; inputs %s, published %s' % (why, base_digest[:12],
                                                   (published_digest or 'unrecorded')[:12]))
    ctx = {'src': src, 'commit': commit, 'run_dir': run_dir, 'state': state, 'stamp': stamp,
           'published': published, 'trees': trees}
    do_refresh = yes(cfg['refresh']) and not a.no_refresh
    if a.plan:
        cands = plan_candidates(False, base_digest, published_digest, a.force)
        return {'state': 'nothing', 'detail': 'plan: %s; then build %s' % (
            'refresh to the branch heads' if do_refresh else 'no refresh',
            ' or '.join((['the refreshed pin if it moved'] if do_refresh else []) + cands)
            or 'nothing'),
            'base_digest': base_digest, 'published_digest': published_digest,
            'commit': commit, 'needs_resign': needs_resign(
                published['index'], dt.datetime.now(dt.timezone.utc),
                int(cfg['resign_within_days']))}

    state['committed_pin_sha256'] = committed_sha
    refreshed = refresh_error = None
    if do_refresh:
        preflight(cfg)
        try:
            snap = refresh(cfg, src, run_dir, log)
            new = pin_projects(snap)
            diff = compare_pins(base_pin, new)
            (run_dir / 'refresh-diff.json').write_text(json.dumps(diff, indent=1) + '\n')
            refusal = refresh_refusal(diff, new)
            if refusal:
                raise Failed('refresh', refusal)
            if diff['changed'] or diff['added']:
                refreshed = snap
                log('refresh: %d project(s) moved, %d added' % (len(diff['changed']),
                                                                len(diff['added'])))
            else:
                log('refresh: upstream has not moved')
        except Failed as e:
            refresh_error = e
            log('refresh failed: %s' % e)
            file_bead(cfg, 'refresh', e.stage, '%s\n\nThe job fell back to the base pin. Run: %s'
                      % (e, run_dir), log)
            notify(cfg, 'Remora image: the Android refresh failed', str(e))
    cands = plan_candidates(refreshed is not None, base_digest, published_digest, a.force)
    if not cands:
        if needs_resign(published['index'], dt.datetime.now(dt.timezone.utc),
                        int(cfg['resign_within_days'])) and not a.no_publish:
            return resign(cfg, ctx, log)
        return {'state': 'nothing', 'detail': 'the published image was built from these inputs'
                + ('; the refresh failed: %s' % refresh_error if refresh_error else '')}
    ctx['remora'] = remora_binary(cfg, src, log)
    errors = []
    for label in cands:
        pin_bytes = refreshed if label == 'refreshed' else base_bytes
        try:
            built = attempt(cfg, ctx, label, pin_bytes, log)
        except Failed as e:
            errors.append(e)
            log('%s failed: %s' % (label, e))
            if label == 'refreshed':
                file_bead(cfg, 'refresh', e.stage, '%s\n\nThe job fell back to the base pin. '
                          'Run: %s' % (e, run_dir), log)
                notify(cfg, 'Remora image: the refreshed Android failed', str(e))
            continue
        res = package_and_publish(cfg, ctx, built, not a.no_publish, log)
        notes = ['after: %s' % '; '.join(str(e) for e in errors)] if errors else []
        if res['state'] == 'published':
            # Recorded the moment the upload succeeded, before anything else can fail.
            state['skipped_patches'] = built['skipped']
            stored.write_bytes(built['pin_bytes'])
            state.setdefault('made_tags', []).append(res['version_tag'])
            state.setdefault('released', []).append(
                {'version_tag': res['version_tag'], 'label': label, 'at': stamp,
                 'image_id': built['image_id'], 'commit': commit,
                 'security_patch': built['facts'].get('ro.build.version.security_patch')})
            save_state(cfg['work'], state)
            warning = after_publish(cfg, ctx, res, log)
            if warning:
                notes.append(warning)
                log(warning)
            tidy_local(cfg, state, log)
        res.pop('entry', None)
        res['label'] = label
        res['security_patch'] = built['facts'].get('ro.build.version.security_patch')
        if notes:
            res['detail'] = '; '.join(notes)
        return res
    raise Failed(errors[-1].stage, '; '.join(str(e) for e in errors))


def report(result):
    print(json.dumps(result, sort_keys=True), flush=True)
    return 0 if result['state'] != 'failed' else 1


def cmd_seed_mesa(a):
    """Cache a Mesa payload for a pin, after checking it is that pin's Mesa. The payload's
    libgallium_dri.so reports the Mesa commit it was built from ("Mesa 26.1.0-devel (git-<rev>)")."""
    cfg = load_config(a.config)
    src = Path(getattr(a, 'from'))
    if (src / 'lib64').is_dir():
        src = src / 'lib64'
    pin_bytes = Path(a.pin).read_bytes() if a.pin else (
        (cfg['work'] / 'base-pin.xml').read_bytes() if (cfg['work'] / 'base-pin.xml').exists()
        else (cfg['work'] / 'src' / PIN).read_bytes())
    pin = pin_projects(pin_bytes)
    gallium = src / 'dri' / 'libgallium_dri.so'
    if not gallium.is_file():
        raise SystemExit('seed-mesa: %s has no dri/libgallium_dri.so' % src)
    rev = pin.get('external/mesa3d', '')
    if ('git-' + rev[:10]).encode() not in gallium.read_bytes():
        raise SystemExit('seed-mesa: %s was not built from external/mesa3d %s' % (gallium, rev[:12]))
    dest = mesa_cache(cfg, pin)
    shutil.rmtree(dest, ignore_errors=True)
    shutil.copytree(src, dest / 'lib64', symlinks=True)
    print('seeded %s (mesa3d %s, libdrm %s)' % (dest, rev[:12], pin.get('external/libdrm', '')[:12]))
    return 0


def cmd_install_timer(a):
    cfg = load_config(a.config)
    units = Path(os.environ.get('XDG_CONFIG_HOME') or Path.home() / '.config') / 'systemd/user'
    names = ('remora-image-release.service', 'remora-image-release.timer')
    if a.remove:
        subprocess.run(['systemctl', '--user', 'disable', '--now', names[1]], capture_output=True)
        for n in names:
            (units / n).unlink(missing_ok=True)
        subprocess.run(['systemctl', '--user', 'daemon-reload'])
        print('removed %s' % ', '.join(names))
        return 0
    src = cfg['work'] / 'src'
    if not (src / '.git').exists():
        update_clone(cfg, print)
    script = src / 'vendor/release/weekly-release.py'
    service, timer = systemd_units(shutil.which('python3') or '/usr/bin/python3', script,
                                   cfg['config_path'].resolve(), src, cfg['branch'], a.calendar)
    units.mkdir(parents=True, exist_ok=True)
    (units / names[0]).write_text(service)
    (units / names[1]).write_text(timer)
    subprocess.run(['systemctl', '--user', 'daemon-reload'], check=True)
    subprocess.run(['systemctl', '--user', 'enable', '--now', names[1]], check=True)
    print('installed %s (%s)' % (', '.join(names), a.calendar))
    if not script.exists():
        print('NOTE: the public main does not carry %s yet: the timer fails until it does'
              % script.relative_to(src))
    return 0


def cmd_status(a):
    cfg = load_config(a.config)
    state = load_state(cfg['work'])
    print(json.dumps({k: state.get(k) for k in ('last_run', 'released', 'skipped_patches',
                                                 'made_tags')}, indent=2, sort_keys=True))
    return 0


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    sub = p.add_subparsers(dest='cmd', required=True)
    r = sub.add_parser('run', help='one weekly run')
    r.add_argument('--plan', action='store_true',
                   help='say what a run would build, without syncing, building or publishing')
    r.add_argument('--no-publish', action='store_true', help='build and package, upload nothing')
    r.add_argument('--no-refresh', action='store_true', help='build the base pin only')
    r.add_argument('--force', action='store_true', help='build even when nothing changed')
    s = sub.add_parser('seed-mesa', help='cache the Mesa payload a pin builds with')
    s.add_argument('--from', required=True, help='a mesa_source payload (its lib64 directory)')
    s.add_argument('--pin', help='the manifest it belongs to (default: the base pin)')
    t = sub.add_parser('install-timer', help='a systemd --user timer for weekly runs')
    t.add_argument('--calendar', default='Sun *-*-* 01:30:00')
    t.add_argument('--remove', action='store_true')
    sub.add_parser('status', help="the last run and the job's releases")
    for q in (r, s, t, sub.choices['status']):
        q.add_argument('--config', default=str(default_config_path()))
    a = p.parse_args(argv)
    signal.signal(signal.SIGTERM, _forward)
    try:
        return {'run': cmd_run, 'seed-mesa': cmd_seed_mesa, 'install-timer': cmd_install_timer,
                'status': cmd_status}[a.cmd](a)
    except Failed as e:
        print('weekly-release: %s' % e, file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
