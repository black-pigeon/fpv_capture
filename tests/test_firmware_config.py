"""The local board configuration must parse without hardware or source trees."""
from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]


class FirmwareConfigurationTest(unittest.TestCase):
    def test_show_config_is_available_without_hardware(self):
        result = subprocess.run(['make', '-s', '-C', str(ROOT / 'firmware'),
                                 'show-config'], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        values = dict(line.split('=', 1) for line in result.stdout.splitlines()
                      if '=' in line)
        self.assertEqual(values['BOARD'], 'e200')
        self.assertEqual(values['SOC_FAMILY'], 'zynq')
        self.assertEqual(Path(values['HW_FILE']), ROOT / 'hardware/e200_fpv/e200_fpv.xsa')
        self.assertEqual(Path(values['HW_BIT']), ROOT / 'hardware/e200_fpv/e200_fpv.bit')
        self.assertTrue(Path(values['BUILD_DIR']).is_relative_to(ROOT / 'firmware'))

    def test_help_is_available_without_fetching(self):
        result = subprocess.run(['make', '-s', '-C', str(ROOT / 'firmware'), 'help'],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('fetch-locked', result.stdout)


if __name__ == '__main__':
    unittest.main()
