"""Board selection in firmware packaging keeps the E200 defaults."""
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

from firmware.scripts import package_rootfs_update as rootfs
from firmware.scripts import package_test_sd as sd


class PackagingBoardTest(unittest.TestCase):
    def test_sd_cli_board_defaults_and_explicit_selection(self):
        for board, options in [('e200', []), ('e310', ['--board', 'e310'])]:
            with self.subTest(board=board), patch('sys.argv', ['package_test_sd.py', '--verify-only', *options]), \
                    patch.object(sd, 'verify', return_value={}) as verify:
                sd.main()
                verify.assert_called_once_with(sd.ROOT / f'firmware/build/{board}/sd', None, None, board=board)

    def test_rootfs_cli_board_defaults_and_explicit_selection(self):
        for board, options in [('e200', []), ('e310', ['--board', 'e310'])]:
            with self.subTest(board=board), patch('sys.argv', ['package_rootfs_update.py', '--verify-only', *options]), \
                    patch.object(rootfs, 'verify', return_value={}) as verify:
                rootfs.main()
                verify.assert_called_once_with(rootfs.ROOT / f'firmware/build/buildroot/{board}/images/rootfs.cpio.gz',
                                               None, None, board=board)

    def test_explicit_input_paths_are_forwarded_for_selected_board(self):
        for module, options, first in [
            (sd, [], sd.ROOT / 'firmware/build/e310/sd'),
            (rootfs, ['--rootfs', '/tmp/selected-rootfs.cpio.gz'], Path('/tmp/selected-rootfs.cpio.gz')),
        ]:
            with self.subTest(module=module.__name__), patch('sys.argv', [module.__name__, '--verify-only',
                    '--board', 'e310', '--xsa', '/tmp/reviewed.xsa',
                    '--hardware-manifest', '/tmp/reviewed.json', *options]), \
                    patch.object(module, 'verify', return_value={}) as verify:
                module.main()
                verify.assert_called_once_with(first, Path('/tmp/reviewed.xsa'), Path('/tmp/reviewed.json'), board='e310')

    def test_rootfs_verifies_selected_board_and_preserves_stream_checks(self):
        for board in ['e200', 'e310']:
            with self.subTest(board=board), tempfile.TemporaryDirectory() as temporary:
                directory = Path(temporary)
                target = directory / f'firmware/build/buildroot/{board}/target'
                files = {}
                elf = bytearray(20)
                elf[:4] = b'\x7fELF'
                struct.pack_into('<H', elf, 18, 40)
                for name in ['fpv-capture', 'fpv-radio-probe', 'fpv-webd']:
                    data = bytes(elf)
                    if name == 'fpv-webd':
                        data += b'window_phase rf_tunes offset_hz frequency_locked captured_frames waiting_signal '
                        data += b'/api/v1/stream multipart/mixed; boundary=fpv-stream-v1 X-FPV-Metadata dropped_before'
                    path = 'usr/sbin/' + name
                    files[path] = data
                    (target / path).parent.mkdir(parents=True, exist_ok=True)
                    (target / path).write_bytes(data)
                for name in ['index.html', 'app.js', 'style.css']:
                    data = (rootfs.ROOT / 'app/web' / name).read_bytes()
                    files['usr/share/e200-fpv/web/' + name] = data
                    path = directory / 'app/web' / name
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_bytes(data)
                for name in ['etc/init.d/S95fpv-web', 'usr/sbin/fpv-net-diag']:
                    data = (rootfs.ROOT / f'firmware/board/{board}/buildroot/rootfs-overlay' / name).read_bytes()
                    files[name] = data
                    path = directory / f'firmware/board/{board}/buildroot/rootfs-overlay' / name
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_bytes(data)
                files['etc/network/interfaces'] = b'address 192.168.1.10\n'
                modes = {name: 0o100755 for name in files}
                with patch.object(rootfs, 'ROOT', directory), \
                        patch.object(rootfs, 'cpio_files', return_value=(files, modes)), \
                        patch.object(rootfs, 'load_hardware', return_value=({'board': board}, b'')) as load:
                    self.assertEqual(rootfs.verify(Path('fixture.cpio.gz'), board=board), {'board': board})
                    load.assert_called_once_with(None, None, board=board)
                    files['usr/sbin/fpv-webd'] = files['usr/sbin/fpv-webd'].replace(b'X-FPV-Metadata', b'other-header')
                    (target / 'usr/sbin/fpv-webd').write_bytes(files['usr/sbin/fpv-webd'])
                    with self.assertRaises(AssertionError):
                        rootfs.verify(Path('fixture.cpio.gz'), board=board)

    def test_sd_uses_per_board_bitstream_and_kernel(self):
        self.assertEqual(sd.CORE, sd.core_files('e200'))
        self.assertIn('e310_fpv.bit', sd.core_files('e310'))
        self.assertNotIn('e200_fpv.bit', sd.core_files('e310'))
        self.assertEqual(sd.kernel_source('e200'), sd.ROOT / 'firmware/linux')
        self.assertEqual(sd.kernel_source('e310'), sd.ROOT / 'firmware/build/e310/sources/linux')
        with self.assertRaises(ValueError):
            sd.core_files('e310v2')


if __name__ == '__main__':
    unittest.main()
