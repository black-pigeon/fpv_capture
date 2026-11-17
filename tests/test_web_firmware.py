"""Source integration contract: web UI must be installed and started in rootfs."""
from pathlib import Path
import subprocess
import unittest
ROOT = Path(__file__).resolve().parents[1]
class WebFirmwareTest(unittest.TestCase):
    def test_rootfs_overlay_and_service(self):
        config = (ROOT / 'firmware/board/e200/buildroot/defconfig').read_text()
        self.assertIn('BR2_ROOTFS_OVERLAY="$(BR2_EXTERNAL_MPSOC_IMAGE_BUILDER_PATH)/../board/e200/buildroot/rootfs-overlay"', config)
        init = ROOT / 'firmware/board/e200/buildroot/rootfs-overlay/etc/init.d/S95fpv-web'
        self.assertTrue(init.stat().st_mode & 0o111)
        self.assertEqual(subprocess.run(['sh', '-n', str(init)]).returncode, 0)
        self.assertIn('/usr/sbin/fpv-webd', init.read_text())
        self.assertNotIn('--simulate', init.read_text())
    def test_offline_assets(self):
        for name in ['index.html','style.css','app.js']:
            source = (ROOT / 'app/web' / name).read_text()
            self.assertNotIn('https://', source)
        cmake = (ROOT / 'app/CMakeLists.txt').read_text()
        self.assertIn('install(TARGETS fpv-webd', cmake)
        self.assertIn('install(DIRECTORY web/', cmake)
if __name__ == '__main__': unittest.main()
