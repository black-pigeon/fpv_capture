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
            for public_path in extra or {}:
                if public_path.startswith(('firmware/board/e200/hardware/',
                                           'firmware/board/e310/hardware/')):
                    subprocess.run(['git', '-C', str(root), 'add', '--', public_path], check=True)
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

    def test_only_pinned_branding_image_is_allowed(self):
        name = 'app/web/microphase-logo.png'
        logo = (SCRIPT.parents[1] / name).read_bytes()
        result = self.check_content(name, logo)
        self.assertEqual(result.returncode, 0, result.stderr)
        for path, content in ((name, logo + b'tampered'),
                              (name, b'private source under image filename'),
                              ('app/web/other.png', logo)):
            with self.subTest(path=path):
                self.assertNotEqual(self.check_content(path, content).returncode, 0)

    def test_e310_pair_and_simultaneous_board_pairs_allowed(self):
        xsa, manifest = hardware_fixture(board='e310')
        name = 'firmware/board/e310/hardware/e310_fpv.xsa'
        extra = {'firmware/board/e310/hardware/manifest.json': json.dumps(manifest).encode()}
        result = self.check_content(name, xsa, extra)
        self.assertEqual(result.returncode, 0, result.stderr)
        old_xsa, old_manifest = hardware_fixture()
        extra.update({'firmware/board/e200/hardware/e200_fpv.xsa': old_xsa,
                      'firmware/board/e200/hardware/manifest.json': json.dumps(old_manifest).encode()})
        result = self.check_content(name, xsa, extra)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_e310_pair_must_be_complete_valid_and_board_matched(self):
        prefix = 'firmware/board/e310/hardware/'
        xsa, manifest = hardware_fixture(board='e310')
        extra = {prefix + 'manifest.json': json.dumps(manifest).encode()}
        self.assertNotEqual(self.check_content(prefix + 'e310_fpv.xsa', xsa).returncode, 0)
        self.assertNotEqual(self.check_content(prefix + 'manifest.json',
                                             json.dumps(manifest).encode()).returncode, 0)
        self.assertNotEqual(self.check_content(prefix + 'e310_fpv.xsa',
                                             xsa + b'tampered', extra).returncode, 0)
        for board, other in (('e200', 'e310'), ('e310', 'e200')):
            with self.subTest(board=board):
                wrong_xsa, wrong_manifest = hardware_fixture(board=other)
                base = f'firmware/board/{board}/hardware/'
                result = self.check_content(base + f'{board}_fpv.xsa', wrong_xsa,
                    {base + 'manifest.json': json.dumps(wrong_manifest).encode()})
                self.assertNotEqual(result.returncode, 0)
        bad_xsa, bad_manifest = hardware_fixture({'source.tcl': b'private'}, board='e310')
        self.assertNotEqual(self.check_content(prefix + 'e310_fpv.xsa', bad_xsa,
            {prefix + 'manifest.json': json.dumps(bad_manifest).encode()}).returncode, 0)

    def test_e310_hardware_allowlist_stays_narrow(self):
        for name in ('e310_fpv.bit', 'other.xsa', 'system.hwh', 'source.tcl',
                     'source.txt', 'nested/manifest.json'):
            with self.subTest(name=name):
                result = self.check_content('firmware/board/e310/hardware/' + name, b'private')
                self.assertNotEqual(result.returncode, 0)
        for name in ('app/renamed-export.txt', 'firmware/board/e310/renamed-export.txt'):
            xsa, _ = hardware_fixture(board='e310')
            self.assertNotEqual(self.check_content(name, xsa).returncode, 0)
        for suffix in ('v', 'sv', 'vhd', 'vhdl', 'xdc', 'bit', 'xsa'):
            self.assertNotEqual(self.check_content('app/copied.' + suffix, b'private').returncode, 0)
        self.assertNotEqual(self.check_content('fpga/e310_fpv/README.md', b'private').returncode, 0)

    def test_both_private_project_trees_fingerprint_renamed_sources(self):
        for board in ('e200', 'e310'):
            for source in ('rtl/original.v', 'scripts/build.tcl', 'constraints/pins.xdc',
                           'src/vendor/entity.vhd'):
                with self.subTest(board=board, source=source):
                    copied = b'# local source fingerprint ' + source.encode() + b'\n'
                    result = self.check_content('app/renamed-source.txt', copied,
                        {f'fpga/{board}_fpv/{source}': copied})
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn('copy of local private source', result.stderr)

    def test_private_source_renamed_inside_xsa_is_rejected(self):
        for board in ('e200', 'e310'):
            with self.subTest(board=board):
                copied = b'# private board reconstruction source fingerprint\n'
                xsa, manifest = hardware_fixture({'ps7_init.tcl': copied}, board=board)
                base = f'firmware/board/{board}/hardware/'
                result = self.check_content(base + f'{board}_fpv.xsa', xsa,
                    {base + 'manifest.json': json.dumps(manifest).encode(),
                     f'fpga/{board}_fpv/scripts/build.tcl': copied})
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('copy of local private source', result.stderr)

    def test_fpga_tcl_renamed_or_in_patch_rejected(self):
        tcl = b'create_' + b'project e310_fpv . -part xc7z020clg400-2\n'
        for name, content in (('app/renamed.txt', tcl),
                              ('firmware/board/e310/scripts/build.tcl', tcl),
                              ('firmware/board/e310/patches/private.patch', b'+' + tcl)):
            with self.subTest(name=name):
                self.assertNotEqual(self.check_content(name, content).returncode, 0)
        result = self.check_content('firmware/scripts/boot.tcl',
                                    b'proc initialize {} { puts "boot" }\n')
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_e310_rtl_module_renamed_or_in_patch_rejected(self):
        rtl = b'mod' + b'ule e310_fpv(input clk); endmodule\n'
        for name, content in (('app/renamed.txt', rtl),
                              ('firmware/board/e310/patches/private.patch', b'+' + rtl)):
            with self.subTest(name=name):
                self.assertNotEqual(self.check_content(name, content).returncode, 0)

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
            'images/e200-fpv-20261119-bootfiles.tar.gz',
            'docs/public-repository.md', 'tools/fpv_receiver/README.md',
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
