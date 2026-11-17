"""A trial DTB must preserve every property except the selected PHY mode."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / 'firmware/scripts/prepare_ethernet_delay_trial.py'
DTS = '''/dts-v1/;
/ {
    aliases { ethernet0 = &gem0; };
    gem0: ethernet {
        status = "okay"; phy-mode = "rgmii-rxid"; phy-handle = <&phy>;
        mdio {
            #address-cells = <1>; #size-cells = <0>;
            phy: ethernet-phy@1 { compatible = "ethernet-phy-ieee802.3-c22"; reg = <1>; };
            converter@8 { compatible = "xlnx,gmii-to-rgmii-1.0"; reg = <8>; phy-handle = <&phy>; };
        };
    };
};'''


@unittest.skipUnless(all(shutil.which(t) for t in ('dtc', 'fdtget', 'fdtput')), 'device tree tools unavailable')
class EthernetDelayTrialTest(unittest.TestCase):
    def make_baseline(self, directory, text=DTS):
        directory.mkdir()
        source = directory / 'board.dts'
        source.write_text(text)
        subprocess.run(['dtc', '-q', '-I', 'dts', '-O', 'dtb', '-o', str(directory / 'devicetree.dtb'), str(source)], check=True)

    def test_only_phy_mode_changes_and_rollback_is_exact(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            baseline, out = root / 'baseline', root / 'trial'
            self.make_baseline(baseline)
            before = (baseline / 'devicetree.dtb').read_bytes()
            result = subprocess.run(['python3', str(SCRIPT), '--baseline', str(baseline), '--output', str(out)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            def dump(path):
                return subprocess.check_output(['dtc', '-q', '-s', '-I', 'dtb', '-O', 'dts', str(path)], text=True)
            self.assertEqual(dump(out / 'devicetree.dtb'), dump(baseline / 'devicetree.dtb').replace('phy-mode = "rgmii-rxid";', 'phy-mode = "rgmii-id";'))
            self.assertEqual((out / 'rollback/devicetree.dtb').read_bytes(), before)
            self.assertEqual((baseline / 'devicetree.dtb').read_bytes(), before)
            self.assertEqual(sorted(p.name for p in out.glob('*.dtb')), ['devicetree.dtb'])
            result = subprocess.run(['sha256sum', '-c', 'SHA256SUMS'], cwd=out, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_forced_phy_id_baseline_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            baseline, out = root / 'baseline', root / 'trial'
            self.make_baseline(baseline, DTS.replace('"ethernet-phy-ieee802.3-c22"', '"ethernet-phy-id001c.c916", "ethernet-phy-ieee802.3-c22"'))
            result = subprocess.run(['python3', str(SCRIPT), '--baseline', str(baseline), '--output', str(out)], capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('hardware PHY ID', result.stderr)
            self.assertFalse(out.exists())


if __name__ == '__main__':
    unittest.main()
