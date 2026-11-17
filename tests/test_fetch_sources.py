"""Exercise source pinning and preservation using real local Git repositories."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

FETCH = Path(__file__).resolve().parents[1] / 'firmware/scripts/fetch_sources.py'


def git(directory, *args):
    return subprocess.check_output(['git', '-C', str(directory), *args], text=True,
                                   stderr=subprocess.STDOUT).strip()


class FetchSourcesTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.base = Path(self.tmp.name)
        self.remote = self.base / 'upstream'
        self.remote.mkdir()
        git(self.remote, 'init', '-q')
        git(self.remote, 'config', 'user.name', 'Source Test')
        git(self.remote, 'config', 'user.email', 'test@example.invalid')
        (self.remote / 'version.txt').write_text('version one\n')
        git(self.remote, 'add', 'version.txt')
        git(self.remote, 'commit', '-qm', 'first')
        git(self.remote, 'tag', 'release')
        self.first = git(self.remote, 'rev-parse', 'HEAD')
        self.root = self.base / 'firmware'
        self.root.mkdir()
        self.manifest = self.root / 'versions.mk'
        self.manifest.write_text(f'LINUX_REPO_URL ?= {self.remote.as_uri()}\n'
                                 'LINUX_REPO_REF ?= release\n')

    def fetch(self, *args, ok=True):
        result = subprocess.run([sys.executable, str(FETCH), '--manifest',
                                 str(self.manifest), '--root', str(self.root),
                                 '--component', 'linux', *args],
                                text=True, capture_output=True)
        if ok:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def lock(self):
        return json.loads((self.root / 'sources.lock.json').read_text())

    def test_fetch_pins_actual_commit(self):
        self.fetch()
        self.assertEqual(git(self.root / 'linux', 'rev-parse', 'HEAD'), self.first)
        self.assertEqual((self.root / 'linux/version.txt').read_text(), 'version one\n')
        self.assertEqual(self.lock()['components']['linux']['commit'], self.first)

    def test_dirty_checkout_is_preserved_when_fetch_refuses(self):
        self.fetch()
        path = self.root / 'linux/version.txt'
        path.write_text('local modification\n')
        before = (self.root / 'sources.lock.json').read_bytes()
        self.fetch(ok=False)
        self.assertEqual(path.read_text(), 'local modification\n')
        self.assertEqual((self.root / 'sources.lock.json').read_bytes(), before)
        self.assertEqual(git(self.root / 'linux', 'rev-parse', 'HEAD'), self.first)

    def test_wrong_origin_does_not_repoint_repository(self):
        self.fetch()
        git(self.root / 'linux', 'remote', 'set-url', 'origin', 'file:///different-origin')
        self.fetch(ok=False)
        self.assertEqual(git(self.root / 'linux', 'remote', 'get-url', 'origin'),
                         'file:///different-origin')

    def test_missing_ref_does_not_publish_lock_or_checkout(self):
        self.manifest.write_text(f'LINUX_REPO_URL ?= {self.remote.as_uri()}\n'
                                 'LINUX_REPO_REF ?= missing-ref\n')
        self.fetch(ok=False)
        self.assertFalse((self.root / 'sources.lock.json').exists())
        self.assertFalse((self.root / 'linux').exists())

    def test_locked_fetch_restores_commit_when_upstream_tag_moves(self):
        self.fetch()
        original_lock = (self.root / 'sources.lock.json').read_bytes()
        (self.remote / 'version.txt').write_text('version two\n')
        git(self.remote, 'add', 'version.txt')
        git(self.remote, 'commit', '-qm', 'second')
        git(self.remote, 'tag', '-f', 'release')
        self.fetch()
        self.assertNotEqual(git(self.root / 'linux', 'rev-parse', 'HEAD'), self.first)
        (self.root / 'sources.lock.json').write_bytes(original_lock)
        self.fetch('--locked')
        self.assertEqual(git(self.root / 'linux', 'rev-parse', 'HEAD'), self.first)
        self.assertEqual((self.root / 'linux/version.txt').read_text(), 'version one\n')

    def test_locked_fetch_requires_matching_manifest(self):
        self.fetch()
        self.manifest.write_text(f'LINUX_REPO_URL ?= {self.remote.as_uri()}\n'
                                 'LINUX_REPO_REF ?= different-version\n')
        self.fetch('--locked', ok=False)
        self.assertEqual(git(self.root / 'linux', 'rev-parse', 'HEAD'), self.first)

    def test_locked_checkout_reuses_exact_commit_without_network(self):
        self.fetch()
        self.remote.rename(self.base / 'upstream-unavailable')
        self.fetch('--locked')
        self.assertEqual(git(self.root / 'linux', 'rev-parse', 'HEAD'), self.first)

    def test_symlink_destination_cannot_modify_reference(self):
        (self.root / 'linux').symlink_to(self.remote, target_is_directory=True)
        self.fetch(ok=False)
        self.assertEqual(git(self.remote, 'rev-parse', 'HEAD'), self.first)
        self.assertFalse((self.root / 'sources.lock.json').exists())


if __name__ == '__main__':
    unittest.main()
