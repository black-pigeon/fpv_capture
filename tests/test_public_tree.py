"""Use real Git indexes to check private source exclusion, even after force-add."""
from pathlib import Path
import subprocess
import tempfile
import unittest
import json
from test_hardware_input import hardware_fixture

SCRIPT = Path(__file__).resolve().parents[1] / 'tools/check_public_tree.py'


class PublicTreeTest(unittest.TestCase):
    def check_content(self, name, content, extra=None):
        with tempfile.TemporaryDirectory(prefix='fpv-public-index-') as temporary:
            root = Path(temporary)
            subprocess.run(['git', 'init', '-q', str(root)], check=True)
            target = root / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(content)
            if extra:
                for private, data in extra.items():
                    path = root / private
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_bytes(data)
            subprocess.run(['git', '-C', str(root), 'add', '--', name], check=True)
            public_manifest = 'firmware/board/e200/hardware/manifest.json'
            if extra and public_manifest in extra:
                subprocess.run(['git', '-C', str(root), 'add', '--', public_manifest], check=True)
            return subprocess.run(['python3', str(SCRIPT), '--root', str(root)], capture_output=True, text=True)

    def test_application_interface_and_public_patch_allowed(self):
        for name, data in (
            ('app/include/linux/fpv_capture.h', b'struct capture_request { unsigned channel; };\n'),
            ('firmware/board/e200/linux/system-user.dtsi', b'/ { model = "E200"; };\n'),
            ('firmware/board/e200/patches/linux/board.patch', b'+CONFIG_REALTEK_PHY=y\n'),
            ('firmware/board/e200/linux/fpv_capture/fpv_capture.c', b'MODULE_' + b'LICENSE("GPL");\n'),
        ):
            with self.subTest(name=name):
                result = self.check_content(name, data)
                self.assertEqual(result.returncode, 0, result.stderr)

    def test_only_validated_xsa_is_allowed(self):
        xsa, manifest = hardware_fixture()
        name = 'firmware/board/e200/hardware/e200_fpv.xsa'
        extra = {'firmware/board/e200/hardware/manifest.json': json.dumps(manifest).encode()}
        result = self.check_content(name, xsa, extra)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotEqual(self.check_content(name, xsa).returncode, 0)
        self.assertNotEqual(self.check_content(name, xsa + b'tampered', extra).returncode, 0)
        xsa, manifest = hardware_fixture({'design.v': b'private source'})
        extra['firmware/board/e200/hardware/manifest.json'] = json.dumps(manifest).encode()
        self.assertNotEqual(self.check_content(name, xsa, extra).returncode, 0)
        self.assertNotEqual(self.check_content('app/renamed.xsa', xsa).returncode, 0)

    def test_private_paths_and_archives_rejected(self):
        for name in (
            'fpga/example.txt', 'hardware/example.txt',
            'firmware/board/e200/linux/axi_dma/driver.c',
            'firmware/board/e200/linux/fpv_capture/driver.c',
            'firmware/board/e200/patches/autosave/source.patch',
            'docs/superpowers/specs/internal.md', 'app/private.tar.gz',
            'build/board-passwords.txt', '.vscode/settings.json',
        ):
            with self.subTest(name=name):
                self.assertNotEqual(self.check_content(name, b'private\n').returncode, 0)

    def test_renamed_implementation_and_patch_rejected(self):
        # Split markers so the test file itself contains no embedded driver.
        kernel = b'MODULE_' + b'LICENSE("test");\n'
        rtl = b'mod' + b'ule fpv_private(input clk); endmodule\n'
        for name, data in (
            ('app/copied.txt', kernel),
            ('firmware/board/e200/patches/linux/copied.patch', b'+' + kernel),
            ('app/renamed.txt', rtl),
            ('app/key.txt', b'-----BEGIN ' + b'PRIVATE KEY-----\nsecret\n'),
            ('app/not-an-archive.txt', b'PK\x03\x04binary'),
        ):
            with self.subTest(name=name):
                self.assertNotEqual(self.check_content(name, data).returncode, 0)
        copied = b'// local private RTL marker\n'
        result = self.check_content('app/renamed-driver.txt', copied,
            {'fpga/e200_fpv/src/fpv/original.v': copied})
        self.assertNotEqual(result.returncode, 0)


if __name__ == '__main__':
    unittest.main()
