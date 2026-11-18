"""Independent timing/FM checks and offline replay CLI checks; no RF access."""
import hashlib
import shutil
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

try:
    import numpy as np
    from PIL import Image
except ImportError as exc:
    raise unittest.SkipTest('waveform tests need NumPy and Pillow') from exc

ROOT = Path(__file__).resolve().parents[1]
GENERATOR = ROOT / 'tools/u220_fpv_tx/generate_fpv.py'
spec = importlib.util.spec_from_file_location('fpv_source', GENERATOR)
source = importlib.util.module_from_spec(spec)
spec.loader.exec_module(source)


class WaveformTest(unittest.TestCase):
    def test_frame_and_field_timing_and_interlace(self):
        for name, count, lines, height, pulses, first, second in (
            ('pal', 1228800, 625, 576, 5, 22, 335),
            ('ntsc', 1025024, 525, 480, 6, 20, 283),
        ):
            with self.subTest(standard=name):
                standard = source.STANDARDS[name]
                pixels = np.zeros((height, 720), dtype=np.uint8)
                pixels[1::2] = 255
                video = source.video_frame(pixels, standard, filtered=False)
                self.assertEqual(video.size, count)
                # Sample well inside the active interval of both interlaced fields.
                for line, expected in ((first, 0.0), (second, .7)):
                    index = round(line * count / lines + 30e-6 * source.FS)
                    self.assertAlmostEqual(video[index], expected, places=10)
                # Independently check every equalizing/broad pulse in both fields.
                half = count / (2 * lines)
                for field in (0, count // 2):
                    for pulse in range(3 * pulses):
                        index = field + round(pulse * half)
                        self.assertAlmostEqual(video[index + round(1e-6 * source.FS)], -.3)
                        expected = -.3 if pulses <= pulse < 2 * pulses else 0.0
                        self.assertAlmostEqual(video[index + round(10e-6 * source.FS)], expected)
                # The second field starts at a half-line relative to horizontal sync.
                self.assertEqual(lines % 2, 1)
                self.assertAlmostEqual((count / 2) / (count / lines) % 1, .5)

    def test_quantized_fm_and_continuous_loop(self):
        for name, polarity in (('pal', 1), ('ntsc', -1)):
            with self.subTest(standard=name), tempfile.TemporaryDirectory() as temporary:
                result = subprocess.run([sys.executable, str(GENERATOR), '--standard', name,
                    '--frames', '2', '--polarity', str(polarity), '--output-dir', temporary],
                    capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                meta = json.loads((Path(temporary) / 'signal.json').read_text())
                iq = np.fromfile(Path(temporary) / meta['iq_file'], dtype='<i2').reshape(-1, 2)
                self.assertEqual(meta['bytes'], iq.size * 2)
                self.assertEqual(meta['samples'], len(iq))
                z = iq[:, 0].astype(float) + 1j * iq[:, 1]
                fm = np.angle(z * np.conj(np.roll(z, 1))) * source.FS / (2 * np.pi)
                expected = []
                for index in range(2):
                    image = source.test_card(index, 2, source.STANDARDS[name])
                    expected.append(polarity * 8e6 * (source.video_frame(image, source.STANDARDS[name]) - .2)
                        + meta['loop_frequency_correction_hz'])
                expected = np.concatenate(expected)
                self.assertLess(float(np.max(np.abs(fm - expected))), 1500)
                # Recover the first full grayscale frame from QUANTIZED IQ at
                # known timing; compare against the independent image fixture.
                # This is not an automatic receiver or an FPGA simulation.
                count, lines, height, first, second, start = (
                    (1228800, 625, 576, 22, 335, 10.4e-6) if name == 'pal' else
                    (1025024, 525, 480, 20, 283, 10.5e-6))
                rows = np.arange(height)
                line = np.where(rows % 2 == 0, first + rows // 2, second + rows // 2)
                indices = np.rint(line[:, None] * count / lines +
                    (start + (np.arange(720) + .5) * 52e-6 / 720) * source.FS).astype(int)
                volts = .2 + (fm[indices] - meta['loop_frequency_correction_hz']) / (polarity * 8e6)
                recovered = np.clip(volts * 255 / .7, 0, 255)
                reference = np.asarray(Image.open(Path(temporary) / 'test_card.png'), dtype=float)
                self.assertLess(float(np.mean(np.abs(recovered - reference))), 7)

                self.assertLess(abs(meta['final_phase_error_rad']), 1e-5)
                self.assertLess(meta['peak_instantaneous_offset_hz'], source.FS / 2)
                self.assertTrue((Path(temporary) / 'test_card.png').is_file())

    def test_invalid_generation_is_rejected_before_output(self):
        with tempfile.TemporaryDirectory() as temporary:
            out = Path(temporary) / 'not-created'
            for extra in (['--frames', '0'], ['--sensitivity', 'nan'],
                          ['--amplitude', 'nan'], ['--amplitude', '1']):
                result = subprocess.run([sys.executable, str(GENERATOR), '--output-dir', str(out), *extra],
                                        capture_output=True)
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(out.exists())



class ReplayCLITest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which('g++') or not shutil.which('pkg-config'):
            raise unittest.SkipTest('C++ replay tests need g++ and pkg-config')
        found = subprocess.run(['pkg-config', '--exists', 'uhd', 'libcrypto'])
        if found.returncode:
            raise unittest.SkipTest('C++ replay tests need UHD and OpenSSL development packages')
        cls.temporary = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.work = Path(cls.temporary.name)
        result = subprocess.run(['bash', str(ROOT / 'tools/u220_fpv_tx/build.sh'), str(cls.work)],
                                capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)
        cls.exe = cls.work / 'u220_fpv_tx'

    def fixture(self):
        iq = self.work / 'tiny.cs16'
        data = b'\x01\x00\x02\x00' * 100
        iq.write_bytes(data)
        metadata = self.work / 'signal.json'
        metadata.write_text(json.dumps(dict(format='cs16_le', iq_file=iq.name,
            bytes=len(data), samples=100, sha256=hashlib.sha256(data).hexdigest(), sample_rate_hz=30720000)))
        return iq, metadata

    def test_default_offline_does_not_open_uhd(self):
        iq, metadata = self.fixture()
        result = subprocess.run([str(self.exe), '--input', str(iq), '--metadata', str(metadata),
            '--args', 'type=device_that_does_not_exist'], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('mode=offline', result.stdout)
        self.assertIn('rate_hz=30720000', result.stdout)
        self.assertNotIn('[UHD]', result.stderr)

    def test_rejects_mismatched_or_malformed_inputs_before_device_access(self):
        iq, metadata = self.fixture()
        base = [str(self.exe), '--input', str(iq), '--metadata', str(metadata),
                '--args', 'type=device_that_does_not_exist']
        for extra in (['--sample-rate', '61440000'], ['--dwell', 'nan'], ['--dwell', '0'],
                      ['--freqs', '7000000000'], ['--freqs', '5800000000,'],
                      ['--channel', '0.5'], ['--transmit', '--dry-run']):
            with self.subTest(extra=extra):
                result = subprocess.run([*base, *extra], capture_output=True, text=True)
                self.assertNotEqual(result.returncode, 0)
                self.assertNotIn('No devices found', result.stderr)
        # Same-size corruption must be caught by the manifest digest.
        iq.write_bytes(b'\x00' * 400)
        result = subprocess.run([*base, '--transmit'], capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('SHA256', result.stderr)
        iq.write_bytes(b'bad')
        result = subprocess.run(base, capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('multiple of 4', result.stderr)

    def test_live_control_checks_both_waveforms_offline(self):
        iq, metadata = self.fixture()
        meta = json.loads(metadata.read_text())
        meta['standard'] = 'pal'
        metadata.write_text(json.dumps(meta))
        alternate = self.work / 'alternate.json'
        alternate.write_text(json.dumps({**meta, 'standard': 'ntsc'}))
        base = [str(self.exe), '--input', str(iq), '--metadata', str(metadata),
                '--args', 'type=device_that_does_not_exist', '--attenuation', '89.75',
                '--live-control', '--alternate-input', str(iq), '--alternate-metadata', str(alternate)]
        result = subprocess.run(base, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn('[UHD]', result.stderr)
        for changes in ({'standard': 'pal'}, {'sample_rate_hz': 61440000}, {'sha256': 'bad'}):
            alternate.write_text(json.dumps({**meta, 'standard': 'ntsc', **changes}))
            result = subprocess.run([*base, '--transmit'], capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertNotIn('No devices found', result.stderr)

    def test_usb_queue_default_and_explicit_override(self):
        iq, metadata = self.fixture()
        base = [str(self.exe), '--input', str(iq), '--metadata', str(metadata)]
        for extra, expected in (([], '256'),
                                (['--args', 'type=b200,num_send_frames=64'], '64')):
            with self.subTest(extra=extra):
                result = subprocess.run([*base, *extra], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn('usb_send_frames=' + expected, result.stdout)

    def test_attenuation_is_explicit_and_excludes_gain(self):
        iq, metadata = self.fixture()
        base = [str(self.exe), '--input', str(iq), '--metadata', str(metadata)]
        result = subprocess.run([*base, '--attenuation', '89.75'], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('attenuation_db=89.75', result.stdout)
        for args in (['--attenuation', '90'], ['--attenuation', '0.1'],
                     ['--attenuation', '70', '--gain', '0']):
            with self.subTest(args=args):
                self.assertNotEqual(subprocess.run([*base, *args], capture_output=True).returncode, 0)

    def test_external_ddm_iq_requires_explicit_rate(self):
        iq, _ = self.fixture()
        base = [str(self.exe), '--input', str(iq)]
        self.assertNotEqual(subprocess.run(base, capture_output=True).returncode, 0)
        result = subprocess.run([*base, '--sample-rate', '30720000', '--freqs', '5800000000'],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == '__main__':
    unittest.main()
