"""Cached Make stamps must not hide a changed source lock."""
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
COMPONENTS = [('u-boot', 'UBOOT'), ('linux', 'LINUX'), ('buildroot', 'BUILDROOT'),
              ('embeddedsw', 'EMBEDDEDSW'), ('device-tree-xlnx', 'DEVICE_TREE')]


def git(path, *args):
    return subprocess.check_output(['git', '-C', str(path), *args], text=True,
                                   stderr=subprocess.STDOUT).strip()


class SourceLockGateTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.base = Path(self.tmp.name)
        self.upstream = self.base / 'upstream'
        self.upstream.mkdir()
        git(self.upstream, 'init', '-q')
        git(self.upstream, 'config', 'user.name', 'Source Test')
        git(self.upstream, 'config', 'user.email', 'test@example.invalid')
        (self.upstream / 'value').write_text('first\n')
        git(self.upstream, 'add', 'value')
        git(self.upstream, 'commit', '-qm', 'first')
        self.first = git(self.upstream, 'rev-parse', 'HEAD')
        (self.upstream / 'value').write_text('second\n')
        git(self.upstream, 'commit', '-qam', 'second')
        self.second = git(self.upstream, 'rev-parse', 'HEAD')
        self.firmware = self.base / 'firmware'
        self.firmware.mkdir()
        (self.base / 'app').mkdir()
        (self.firmware / 'buildroot-external/package').mkdir(parents=True)
        for name in ['Makefile', 'board/common.mk', 'board/e200/board.mk',
                     'scripts/validate_sources.py', 'scripts/fetch_sources.py']:
            source = ROOT / 'firmware' / name
            if source.exists():
                target = self.firmware / name
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(source, target)
        stamps = self.firmware / '.source-stamps'
        stamps.mkdir()
        values = []
        self.lock = {'schema': 1, 'components': {}}
        for name, prefix in COMPONENTS:
            subprocess.run(['git', 'clone', '-q', self.upstream.as_uri(),
                            str(self.firmware / name)], check=True,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            git(self.firmware / name, 'checkout', '-q', '--detach', self.first)
            values.extend([f'{prefix}_REPO_URL ?= {self.upstream.as_uri()}',
                           f'{prefix}_REPO_REF ?= release'])
            self.lock['components'][name] = {'url': self.upstream.as_uri(),
                                            'ref': 'release', 'commit': self.first,
                                            'directory': name}
            (stamps / f'{name}-release.stamp').touch()
        (self.firmware / 'versions.mk').write_text('\n'.join(values) + '\n')
        self.write_lock()

    def write_lock(self):
        (self.firmware / 'sources.lock.json').write_text(json.dumps(self.lock))

    def make_repos(self):
        return subprocess.run(['make', '-s', '-C', str(self.firmware), 'repos'],
                              capture_output=True, text=True)

    def test_changed_lock_is_checked_even_with_existing_fetch_stamps(self):
        self.lock['components']['linux']['commit'] = self.second
        self.write_lock()
        result = self.make_repos()
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(git(self.firmware / 'linux', 'rev-parse', 'HEAD'), self.first)

    def test_expected_board_patches_do_not_require_refetch(self):
        value = self.firmware / 'linux/value'
        value.write_text('board patch\n')
        result = self.make_repos()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(value.read_text(), 'board patch\n')


if __name__ == '__main__':
    unittest.main()
