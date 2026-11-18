"""Exercise the public XSA boundary without Vivado or private FPGA sources."""
import hashlib
import io
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / 'firmware/scripts/hardware_input.py'


def hardware_fixture(extra=None, bits=None):
    members = {'e200_fpv.bit': bits or b'fixture-bitstream\x00',
               'system.hwh': b'<SYSTEM/>', 'xsa.xml': b'<XSA/>'}
    members.update(extra or {})
    data = io.BytesIO()
    with zipfile.ZipFile(data, 'w', zipfile.ZIP_DEFLATED) as archive:
        for name, content in members.items():
            archive.writestr(name, content)
    xsa = data.getvalue()
    bit = members['e200_fpv.bit']
    manifest = {
        'format': 'e200-fpv-xsa-v1', 'board': 'e200',
        'xsa': {'file': 'e200_fpv.xsa', 'sha256': hashlib.sha256(xsa).hexdigest(), 'bytes': len(xsa)},
        'bitstream': {'member': 'e200_fpv.bit', 'sha256': hashlib.sha256(bit).hexdigest(), 'bytes': len(bit)},
        'capture': {'register_version': 6, 'packet_version': 2, 'channels': 4, 'dma_count': 1},
        'ethernet': {'phy_mode': 'rgmii-id', 'phy_address': 1, 'converter_address': 8, 'forced_phy_id': False},
    }
    return xsa, manifest


class HardwareInputTest(unittest.TestCase):
    def run_input(self, xsa, manifest, output=True):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / 'input.xsa').write_bytes(xsa)
            (root / 'manifest.json').write_text(json.dumps(manifest))
            args = ['python3', str(SCRIPT), '--xsa', str(root / 'input.xsa'),
                    '--manifest', str(root / 'manifest.json')]
            if output:
                args += ['--output-bit', str(root / 'out.bit')]
            result = subprocess.run(args, capture_output=True, text=True)
            bit = (root / 'out.bit').read_bytes() if (root / 'out.bit').exists() else None
            return result, bit

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
