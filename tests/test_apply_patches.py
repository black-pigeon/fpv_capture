"""Patches must be repeatable and conflicts must stop the build."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

APPLY = Path(__file__).resolve().parents[1] / 'firmware/scripts/apply_patches.py'


class ApplyPatchesTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.base = Path(self.tmp.name)
        self.source = self.base / 'source'
        self.patches = self.base / 'patches'
        self.source.mkdir()
        self.patches.mkdir()
        (self.patches / '0001-change.patch').write_text(
            '--- a/file.txt\n+++ b/file.txt\n@@ -1 +1 @@\n-old\n+new\n')

    def apply(self):
        return subprocess.run([sys.executable, str(APPLY), str(self.source),
                               str(self.patches)], capture_output=True, text=True)

    def test_patch_is_applied_once_and_repeat_succeeds(self):
        (self.source / 'file.txt').write_text('old\n')
        first = self.apply()
        self.assertEqual(first.returncode, 0, first.stdout + first.stderr)
        self.assertEqual((self.source / 'file.txt').read_text(), 'new\n')
        second = self.apply()
        self.assertEqual(second.returncode, 0, second.stdout + second.stderr)
        self.assertEqual((self.source / 'file.txt').read_text(), 'new\n')

    def test_e200_patch_enables_phy_on_locked_kernel_configuration(self):
        # Relevant unmodified config context from linux-xlnx xilinx-v2022.2.
        config = self.source / 'arch/arm/configs/xilinx_zynq_defconfig'
        config.parent.mkdir(parents=True)
        config.write_text('CONFIG_XILINX_EMACLITE=y\nCONFIG_XILINX_AXI_EMAC=y\n'
                          'CONFIG_MARVELL_PHY=y\nCONFIG_VITESSE_PHY=y\n'
                          'CONFIG_MDIO_BITBANG=y\nCONFIG_INPUT_SPARSEKMAP=y\n'
                          'CONFIG_XILINX_DMA=y\n')
        patch_dir = APPLY.parent.parent / 'board/e200/patches/linux'
        result = subprocess.run([sys.executable, str(APPLY), str(self.source),
                                 str(patch_dir)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('CONFIG_REALTEK_PHY=y\n', config.read_text())
        self.assertEqual(config.read_text().count('CONFIG_XILINX_DMA=y'), 1)

    def test_conflict_fails_and_preserves_source(self):
        (self.source / 'file.txt').write_text('local content\n')
        result = self.apply()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual((self.source / 'file.txt').read_text(), 'local content\n')
        self.assertFalse((self.source / 'file.txt.rej').exists())


if __name__ == '__main__':
    unittest.main()
