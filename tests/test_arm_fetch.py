"""Exercise the ARM downloader's delivery fallback without a device, account or network."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import textwrap
import unittest


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / 'vendor/guest-scripts/remora-arm-fetch.sh'
PACKAGE = 'software.sonar.sonar_mobile'
MOCK = r'''
import json, os, pathlib, sys
name = pathlib.Path(sys.argv[0]).name
args = sys.argv[1:]
with open(os.environ['MOCK_CALLS'], 'a') as f:
    f.write(json.dumps([name, args]) + '\n')
if name == 'sqlite3':
    query = args[-1]
    if 'authtoken' in query: print('fixture-play-token')
    elif 'password' in query: print('aas_et/fixture-token')
elif name == 'docker':
    script = args[-1]
    if 'dumpsys account' in script:
        if not os.environ.get('MOCK_NO_ACCOUNT'):
            print('Account {name=fixture@example.test, type=com.google}')
    elif script.startswith('cat '): print('fixture database')
    elif 'getprop ro.sf.lcd_density' in script: print('320')
    elif 'pm install-create' in script: print('Success: created install session [42]')
    elif 'pm install-write' in script:
        sys.stdin.buffer.read()
        print('Success')
    elif 'versionName' in script: print('0.16.12')
    elif 'primaryCpuAbi' in script: print('arm64-v8a')
    elif 'updateOwnerPackageName' in script: print('com.android.shell')
    elif 'pm install-commit' in script or 'pm uninstall' in script: print('Success')
elif name == 'apkeep':
    opts = args[args.index('-o') + 1]
    if os.environ.get('MOCK_AUTH_ERROR'):
        print('Could not log in: credentials rejected')
        sys.exit(1)
    if os.environ.get('MOCK_EMPTY_ALL'): sys.exit(0)
    if 'device=px_tablet' in opts and not os.environ.get('MOCK_TABLET_OK'):
        # gpapi can leave an empty package directory, then refuse to reuse it.
        (pathlib.Path(args[-1]) / args[args.index('-a') + 1]).mkdir()
        sys.exit(0)
    package = args[args.index('-a') + 1]
    out = pathlib.Path(args[-1]) / package
    if out.exists(): sys.exit(0)
    out.mkdir()
    (out / (package + '.apk')).write_bytes(b'fixture base')
    abi = 'x86_64' if os.environ.get('MOCK_X86_ONLY') else 'arm64_v8a'
    (out / (package + '.config.' + abi + '.apk')).write_bytes(b'fixture split')
'''


class ArmFetchTests(unittest.TestCase):
    def run_fetch(self, **settings):
        with tempfile.TemporaryDirectory() as directory:
            tmp = Path(directory)
            commands = tmp / 'bin'
            commands.mkdir()
            for name in ['docker', 'sqlite3', 'apkeep']:
                executable = commands / name
                executable.write_text(f"#!{sys.executable}" + textwrap.dedent(MOCK))
                executable.chmod(0o755)
            calls = tmp / 'calls.jsonl'
            env = {k: v for k, v in os.environ.items()
                   if not k.startswith(('REMORA_', 'MOCK_'))}
            env.update(HOME=str(tmp), TMPDIR=str(tmp), MOCK_CALLS=str(calls),
                       PATH=str(commands) + os.pathsep + os.defpath,
                       REMORA_FETCH_TRIES='1', REMORA_FETCH_DELAY='0')
            env.update(settings)
            result = subprocess.run(['sh', str(SCRIPT), 'fixture-container', PACKAGE],
                                    env=env, text=True, capture_output=True, timeout=15)
            self.assertTrue(calls.is_file(), result.stdout + result.stderr)
            history = [json.loads(line) for line in calls.read_text().splitlines()]
            return result, history

    def profiles(self, history):
        return [args[args.index('-o') + 1] for name, args in history if name == 'apkeep']

    def assert_no_install(self, history):
        self.assertFalse(any('pm uninstall' in args[-1] or 'pm install-' in args[-1]
                             for name, args in history if name == 'docker'))

    def test_empty_tablet_delivery_falls_back_before_install(self):
        result, history = self.run_fetch()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.profiles(history),
                         ['split_apk=1,device=px_tablet', 'split_apk=1,device=px_9a'])
        downloads = [i for i, (name, _) in enumerate(history) if name == 'apkeep']
        install = next(i for i, (name, args) in enumerate(history)
                       if name == 'docker' and 'pm install-create' in args[-1])
        self.assertGreater(install, downloads[-1])

    def test_successful_density_profile_does_not_fall_back(self):
        result, history = self.run_fetch(MOCK_TABLET_OK='1')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.profiles(history), ['split_apk=1,device=px_tablet'])

    def test_explicit_profile_is_respected(self):
        result, history = self.run_fetch(REMORA_APKEEP_DEVICE='px_tablet')
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.profiles(history), ['split_apk=1,device=px_tablet'])
        self.assert_no_install(history)

    def test_empty_fallback_stops_without_removing_installed_app(self):
        result, history = self.run_fetch(MOCK_EMPTY_ALL='1')
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(len(self.profiles(history)), 2)
        self.assert_no_install(history)

    def test_auth_failure_is_not_a_profile_fallback(self):
        result, history = self.run_fetch(MOCK_AUTH_ERROR='1')
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(len(self.profiles(history)), 1)
        self.assert_no_install(history)

    def test_non_arm_delivery_does_not_replace_installed_app(self):
        result, history = self.run_fetch(MOCK_TABLET_OK='1', MOCK_X86_ONLY='1')
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(len(self.profiles(history)), 1)
        self.assert_no_install(history)

    def test_missing_account_diagnostic_does_not_print_available_tokens(self):
        result, history = self.run_fetch(MOCK_NO_ACCOUNT='1')
        self.assertNotEqual(result.returncode, 0)
        output = result.stdout + result.stderr
        self.assertIn('account found : <none>', output)
        self.assertNotIn('aas_et/fixture-token', output)
        self.assertNotIn('fixture-play-token', output)
        self.assertEqual(self.profiles(history), [])
        self.assert_no_install(history)


if __name__ == '__main__':
    unittest.main()
