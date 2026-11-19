"""Exercise the public XSA boundary without Vivado or private FPGA sources."""
import hashlib
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / 'firmware/scripts/hardware_input.py'
sys.path.insert(0, str(SCRIPT.parent))
from hardware_input import load_hardware, validate_xsa


def hardware_fixture(extra=None, bits=None, board='e200'):
    members = {f'{board}_fpv.bit': bits or b'fixture-bitstream\x00',
               'system.hwh': b'<SYSTEM/>', 'xsa.xml': b'<XSA/>'}
    members.update(extra or {})
    data = io.BytesIO()
    with zipfile.ZipFile(data, 'w', zipfile.ZIP_DEFLATED) as archive:
        for name, content in members.items():
            archive.writestr(name, content)
    xsa = data.getvalue()
    bit = members[f'{board}_fpv.bit']
    manifest = {
        'format': f'{board}-fpv-xsa-v1', 'board': board,
        'xsa': {'file': f'{board}_fpv.xsa', 'sha256': hashlib.sha256(xsa).hexdigest(), 'bytes': len(xsa)},
        'bitstream': {'member': f'{board}_fpv.bit', 'sha256': hashlib.sha256(bit).hexdigest(), 'bytes': len(bit)},
        'capture': {'register_version': 6, 'packet_version': 2, 'channels': 4, 'dma_count': 1},
        'ethernet': {'phy_mode': 'rgmii-id', 'phy_address': 1, 'converter_address': 8, 'forced_phy_id': False},
    }
    if board == 'e310':
        manifest['ethernet'] = {'interface': 'ps-gem-mio', 'phy_mode': 'rgmii-id',
                                'phy_address': 0, 'converter_address': None,
                                'forced_phy_id': False}
    return xsa, manifest


class HardwareInputTest(unittest.TestCase):
    def run_input(self, xsa, manifest, output=True, board=None):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / 'input.xsa').write_bytes(xsa)
            (root / 'manifest.json').write_text(json.dumps(manifest))
            args = ['python3', str(SCRIPT), '--xsa', str(root / 'input.xsa'),
                    '--manifest', str(root / 'manifest.json')]
            if board is not None:
                args += ['--board', board]
            if output:
                args += ['--output-bit', str(root / 'out.bit')]
            result = subprocess.run(args, capture_output=True, text=True)
            bit = (root / 'out.bit').read_bytes() if (root / 'out.bit').exists() else None
            return result, bit

    def test_e310_api_and_cli_extract_matching_bitstream(self):
        xsa, manifest = hardware_fixture(board='e310')
        self.assertEqual(validate_xsa(xsa, manifest, board='e310'), b'fixture-bitstream\x00')
        result, bit = self.run_input(xsa, manifest, board='e310')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('E310', result.stdout)
        self.assertEqual(bit, b'fixture-bitstream\x00')
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / 'input.xsa').write_bytes(xsa)
            (root / 'manifest.json').write_text(json.dumps(manifest))
            self.assertEqual(load_hardware(root / 'input.xsa', root / 'manifest.json',
                                           board='e310'), (manifest, bit))

    def test_board_selects_default_paths(self):
        with tempfile.TemporaryDirectory() as temporary:
            firmware = Path(temporary) / 'firmware'
            script = firmware / 'scripts/hardware_input.py'
            script.parent.mkdir(parents=True)
            script.write_bytes(SCRIPT.read_bytes())
            for board in ('e200', 'e310'):
                hardware = firmware / 'board' / board / 'hardware'
                hardware.mkdir(parents=True)
                xsa, manifest = hardware_fixture(board=board)
                (hardware / f'{board}_fpv.xsa').write_bytes(xsa)
                (hardware / 'manifest.json').write_text(json.dumps(manifest))
            for args, board in (([], 'e200'), (['--board', 'e200'], 'e200'),
                                (['--board', 'e310'], 'e310')):
                with self.subTest(args=args):
                    result = subprocess.run(['python3', str(script), *args],
                                            capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertIn(board.upper(), result.stdout)

    def test_wrong_board_pairing_is_rejected(self):
        for board, other in (('e200', 'e310'), ('e310', 'e200')):
            with self.subTest(board=board):
                xsa, manifest = hardware_fixture(board=other)
                with self.assertRaises(ValueError):
                    validate_xsa(xsa, manifest, board=board)
                result, bit = self.run_input(xsa, manifest, board=board)
                self.assertNotEqual(result.returncode, 0)
                self.assertIsNone(bit)
        xsa, manifest = hardware_fixture(board='e310')
        with self.assertRaises(ValueError):
            validate_xsa(xsa, manifest)
        result, _ = self.run_input(xsa, manifest, board='unknown')
        self.assertNotEqual(result.returncode, 0)
        with self.assertRaises(ValueError):
            validate_xsa(xsa, manifest, board='unknown')

    def test_board_specific_mmi_and_generated_ps_initialization_are_allowed(self):
        for board in ('e200', 'e310'):
            with self.subTest(board=board):
                xsa, manifest = hardware_fixture({
                    f'{board}_fpv.mmi': b'<MemInfo/>',
                    'ps7_init.tcl': b'proc ps7_init {} { mwr 0xF8000008 0x0000DF0D }\n',
                }, board=board)
                self.assertEqual(validate_xsa(xsa, manifest, board=board),
                                 b'fixture-bitstream\x00')

    def test_e310_requires_exact_manifest_and_member_names(self):
        for section, key, value in (
            ('format', None, 'e200-fpv-xsa-v1'), ('board', None, 'e200'),
            ('xsa', 'file', 'e200_fpv.xsa'), ('bitstream', 'member', 'e200_fpv.bit'),
            ('capture', 'register_version', 5), ('capture', 'packet_version', 1),
            ('capture', 'channels', 2), ('capture', 'dma_count', 2),
            ('ethernet', 'phy_address', 1), ('ethernet', 'converter_address', 8),
            ('ethernet', 'interface', 'ps-gem-emio'), ('ethernet', 'phy_mode', 'rgmii'),
            ('ethernet', 'forced_phy_id', True),
        ):
            with self.subTest(section=section, key=key):
                xsa, manifest = hardware_fixture(board='e310')
                if key is None:
                    manifest[section] = value
                else:
                    manifest[section][key] = value
                with self.assertRaises(ValueError):
                    validate_xsa(xsa, manifest, board='e310')
        for name in ('e200_fpv.bit', 'e200_fpv.mmi', 'renamed.hwh', 'source.v',
                     'source.tcl', 'source.bit', 'dir/system.hwh', '../system.hwh'):
            with self.subTest(name=name):
                xsa, manifest = hardware_fixture({name: b'private'}, board='e310')
                with self.assertRaises(ValueError):
                    validate_xsa(xsa, manifest, board='e310')

    def test_rejects_source_renamed_as_handoff_metadata(self):
        for board in ('e200', 'e310'):
            for name, payload in (
                ('system.hwh', b'mod' + b'ule private_design(input clk); endmodule\n'),
                ('ps7_init.tcl', b'create_' + b'project secret . -part xc7z020clg400-2\n'),
            ):
                with self.subTest(board=board, name=name):
                    xsa, manifest = hardware_fixture({name: payload}, board=board)
                    with self.assertRaises(ValueError):
                        validate_xsa(xsa, manifest, board=board)

    def test_extracts_the_embedded_bitstream(self):
        xsa, manifest = hardware_fixture()
        result, bit = self.run_input(xsa, manifest)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(bit, b'fixture-bitstream\x00')

    def test_rejects_bad_xsa_and_bit_hashes_and_abi(self):
        for section, key, value in [('xsa', 'sha256', '0' * 64),
                                    ('bitstream', 'sha256', '0' * 64),
                                    ('capture', 'register_version', 5),
                                    ('ethernet', 'phy_mode', 'rgmii-rxid')]:
            with self.subTest(section=section, key=key):
                xsa, manifest = hardware_fixture()
                manifest[section][key] = value
                result, bit = self.run_input(xsa, manifest)
                self.assertNotEqual(result.returncode, 0)
                self.assertIsNone(bit)

    def test_rejects_source_debug_and_unsafe_archive_members(self):
        for name in ('design.v', 'design.tcl', 'capture.ltx', '../secret', '/secret', 'other.bit'):
            with self.subTest(name=name):
                xsa, manifest = hardware_fixture({name: b'not a hardware handoff'})
                result, bit = self.run_input(xsa, manifest)
                self.assertNotEqual(result.returncode, 0)
                self.assertIsNone(bit)

    def test_rejects_private_metadata_in_public_manifest(self):
        xsa, manifest = hardware_fixture()
        manifest['local_sources'] = {'private.v': 'source-hash'}
        result, bit = self.run_input(xsa, manifest)
        self.assertNotEqual(result.returncode, 0)
        self.assertIsNone(bit)

    def test_make_hardware_needs_only_xsa_and_manifest(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            xsa, manifest = hardware_fixture()
            (root / 'e200_fpv.xsa').write_bytes(xsa)
            (root / 'manifest.json').write_text(json.dumps(manifest))
            output = root / 'output'
            result = subprocess.run(['make', '-s', '-C', str(ROOT / 'firmware'), 'hardware',
                f'HW_FILE={root / "e200_fpv.xsa"}', f'HW_MANIFEST={root / "manifest.json"}',
                f'BUILD_DIR={output}'], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual((output / 'e200_fpv.bit').read_bytes(), b'fixture-bitstream\x00')
            self.assertEqual((output / 'e200_fpv.xsa').read_bytes(), xsa)


if __name__ == '__main__':
    unittest.main()
