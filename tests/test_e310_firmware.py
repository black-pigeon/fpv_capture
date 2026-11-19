"""Original ANTSDR E310 firmware configuration and isolated-source regressions."""
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from firmware.scripts.package_test_sd import verify_ethernet_dtb

ROOT = Path(__file__).resolve().parents[1]
FW = ROOT / 'firmware'


def run(*args, **kwargs):
    return subprocess.run([str(arg) for arg in args], capture_output=True, text=True, **kwargs)


class E310FirmwareTest(unittest.TestCase):
    def test_board_selection_and_source_paths(self):
        result = run('make', '-s', '-C', FW, 'BOARD=e310', 'show-config')
        self.assertEqual(result.returncode, 0, result.stderr)
        values = dict(line.split('=', 1) for line in result.stdout.splitlines() if '=' in line)
        self.assertEqual(values['CONSOLE_TTY'], 'ttyPS1')
        self.assertEqual(values['CONSOLE_UART_BASE'], '0xe0001000')
        self.assertEqual(values['FSBL_STDIO'], 'ps7_uart_1')
        self.assertEqual(values['HW_FILE'], str(FW / 'board/e310/hardware/e310_fpv.xsa'))
        self.assertEqual(values['HW_MANIFEST'], str(FW / 'board/e310/hardware/manifest.json'))
        for key, name in [('LINUX_DIR', 'linux'), ('UBOOT_DIR', 'u-boot'), ('BUILDROOT_DIR', 'buildroot')]:
            self.assertEqual(values[key], str(FW / 'build/e310/sources' / name))
        e200 = run('make', '-s', '-C', FW, 'show-config')
        self.assertEqual(e200.returncode, 0, e200.stderr)
        defaults = dict(line.split('=', 1) for line in e200.stdout.splitlines() if '=' in line)
        self.assertEqual(defaults['BOARD'], 'e200')
        self.assertEqual(defaults['LINUX_DIR'], str(FW / 'linux'))
        self.assertEqual(defaults['UBOOT_DIR'], str(FW / 'u-boot'))
        self.assertEqual(defaults['BUILDROOT_DIR'], str(FW / 'buildroot'))

    def test_board_files_are_publishable_and_dropbear_options_exist(self):
        board = FW / 'board/e310'
        self.assertFalse([str(path) for path in board.rglob('*') if path.is_symlink()])
        header = board / 'buildroot/dropbear-localoptions.h'
        self.assertTrue(header.is_file())
        self.assertEqual(header.read_bytes(), (FW / 'board/e200/buildroot/dropbear-localoptions.h').read_bytes())

    def test_heartbeat_driver_is_built_into_kernel(self):
        config = (FW / 'board/e310/board.mk').read_text()
        enabled = next(line.split(':=', 1)[1].split() for line in config.splitlines()
                       if line.startswith('LINUX_CONFIG_ENABLES :='))
        for option in ('NEW_LEDS', 'LEDS_CLASS', 'LEDS_GPIO', 'LEDS_TRIGGERS',
                       'LEDS_TRIGGER_HEARTBEAT'):
            self.assertIn(option, enabled)

    def test_uart1_alias_matches_linux_tty(self):
        with tempfile.TemporaryDirectory() as directory:
            for mode, source in [('linux-dtsi', 'linux/system-user.dtsi'),
                                 ('uboot-dts', 'u-boot/zynq-e310.dts')]:
                output = Path(directory) / mode
                result = run('python3', FW / 'scripts/render_ps_uart_config.py', mode,
                             '--source', FW / 'board/e310' / source, '--output', output,
                             '--console-uart', '1', '--console-tty', 'ttyPS1',
                             '--console-uart-base', '0xe0001000', '--preserve-uart-numbering')
                self.assertEqual(result.returncode, 0, result.stderr)
                text = output.read_text()
                self.assertNotIn('serial0 = &uart1', text)
                self.assertIn('serial1 = &uart1', text)
                self.assertIn('stdout-path = "serial1:115200n8"', text)

    def test_compiled_linux_mio_phy_and_noos_spi(self):
        if not shutil.which('dtc') or not shutil.which('fdtget'):
            self.skipTest('device tree compiler tools unavailable')
        # Deliberately omit the E200 converter: references to it must fail.
        base = '''/dts-v1/;
/ {
 #address-cells = <1>; #size-cells = <1>;
 memory { device_type = "memory"; reg = <0 0x20000000>; };
 gem0: ethernet@e000b000 { reg = <0xe000b000 0x1000>; };
 sdhci0: mmc {}; uart0: serial@e0000000 { reg = <0xe0000000 0x1000>; };
 uart1: serial@e0001000 { reg = <0xe0001000 0x1000>; }; spi0: spi {}; qspi: qspi {};
 gpio0: gpio { gpio-controller; #gpio-cells = <2>; }; clkc: clock {};
 fpv_dma: dma@40400000 { reg = <0x40400000 0x10000>; #dma-cells = <1>; };
};
'''
        with tempfile.TemporaryDirectory() as directory:
            dts, dtb = Path(directory) / 'board.dts', Path(directory) / 'board.dtb'
            dts.write_text(base + (FW / 'board/e310/linux/system-user.dtsi').read_text())
            result = run('dtc', '-I', 'dts', '-O', 'dtb', '-o', dtb, dts)
            self.assertEqual(result.returncode, 0, result.stderr)
            def get(node, prop, kind='s'):
                result = run('fdtget', '-t', kind, dtb, node, prop)
                self.assertEqual(result.returncode, 0, result.stderr)
                return result.stdout.strip()
            self.assertEqual(get('/leds', 'compatible'), 'gpio-leds')
            self.assertEqual(get('/leds/led0', 'gpios', 'u').split(),
                             [get('/gpio', 'phandle', 'u'), '15', '0'])
            self.assertEqual(get('/leds/led0', 'linux,default-trigger'), 'heartbeat')
            self.assertEqual(get('/leds/led0', 'label'), 'led0:green')
            mac = '/ethernet@e000b000'
            phy = mac + '/mdio/ethernet-phy@0'
            self.assertEqual(get(mac, 'status'), 'okay')
            self.assertEqual(get(mac, 'phy-mode'), 'rgmii-id')
            self.assertEqual(get(phy, 'reg', 'u'), '0')
            self.assertEqual(get(phy, 'compatible'), 'ethernet-phy-ieee802.3-c22')
            self.assertEqual(get(mac, 'phy-handle', 'u'), get(phy, 'phandle', 'u'))
            self.assertEqual(get(phy, 'reset-gpios', 'u').split(),
                             [get('/gpio', 'phandle', 'u'), '46', '1'])
            report = verify_ethernet_dtb(dtb, board='e310')
            self.assertEqual(report['interface'], 'ps-gem-mio')
            self.assertEqual(report['phy_address'], 0)
            self.assertIsNone(report['converter_address'])
            self.assertFalse(report['forced_phy_id'])
            self.assertFalse(report['board_link_tested'])
            self.assertNotIn('memory', run('fdtget', '-l', dtb, '/').stdout.split(),
                             'generated unqualified memory node must not overlap memory@0')
            self.assertEqual(get('/memory@0', 'reg', 'x'), '0 40000000')
            self.assertIn('console=ttyPS1,115200', get('/chosen', 'bootargs'))
            self.assertNotIn('mem=256M', get('/chosen', 'bootargs'))
            self.assertEqual(get('/aliases', 'serial1'), '/serial@e0001000')
            self.assertEqual(get('/spi/ad9361@0', 'compatible'), 'rohm,dh2228fv')
            self.assertEqual(get('/spi/ad9361@0', 'reg', 'u'), '0')
            self.assertEqual(get('/fpv-capture@43c30000', 'compatible'), 'fpv,e200-capture-1.0')
            self.assertEqual(get('/fpv-capture@43c30000', 'dmas', 'u').split(),
                             [get('/dma@40400000', 'phandle', 'u'), '1'])
            # Delivery checks must reject an E200-style PHY or the wrong reset.
            original = dts.read_text()
            for old, new in [('reg = <0>;', 'reg = <1>;'),
                             ('reset-gpios = <&gpio0 46 1>', 'reset-gpios = <&gpio0 46 0>')]:
                with self.subTest(invalid=new):
                    dts.write_text(original.replace(old, new))
                    result = run('dtc', '-I', 'dts', '-O', 'dtb', '-o', dtb, dts)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    with self.assertRaises(AssertionError):
                        verify_ethernet_dtb(dtb, board='e310')


class IsolatedSourceTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.repo = self.root / 'linux'
        self.repo.mkdir()
        for args in [('init', '-q'), ('config', 'user.name', 'Board Test'),
                     ('config', 'user.email', 'test@example.invalid')]:
            self.assertEqual(run('git', '-C', self.repo, *args).returncode, 0)
        (self.repo / 'driver.c').write_text('locked source\n')
        run('git', '-C', self.repo, 'add', 'driver.c')
        result = run('git', '-C', self.repo, 'commit', '-qm', 'fixture')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.commit = run('git', '-C', self.repo, 'rev-parse', 'HEAD').stdout.strip()
        self.lock = {'schema': 1, 'components': {'linux': {'commit': self.commit, 'directory': 'linux'}}}
        (self.root / 'sources.lock.json').write_text(json.dumps(self.lock))
        self.output = self.root / 'build/e310/sources/linux'
        self.command = ['python3', FW / 'scripts/prepare_board_source.py', '--root', self.root,
                        '--component', 'linux', '--output', self.output]

    def test_locked_copy_excludes_dirty_changes_and_reuses_private_patches(self):
        (self.repo / 'driver.c').write_text('E200 working patch\n')
        (self.repo / 'untracked.c').write_text('E200 extra file\n')
        before = run('git', '-C', self.repo, 'status', '--porcelain').stdout
        result = run(*self.command)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((self.output / 'driver.c').read_text(), 'locked source\n')
        self.assertFalse((self.output / 'untracked.c').exists())
        (self.output / 'driver.c').write_text('E310 working patch\n')
        self.assertEqual(run(*self.command).returncode, 0)
        self.assertEqual((self.output / 'driver.c').read_text(), 'E310 working patch\n')
        self.assertEqual(run('git', '-C', self.repo, 'status', '--porcelain').stdout, before)
        self.assertEqual((self.repo / 'driver.c').read_text(), 'E200 working patch\n')

    def test_make_alternates_boards_without_modifying_shared_sources(self):
        firmware = self.root / 'fixture-firmware'
        firmware.mkdir()
        (self.root / 'app').mkdir()
        (firmware / 'buildroot-external/package').mkdir(parents=True)
        for name in ['Makefile', 'board/common.mk', 'board/e200/board.mk', 'board/e310/board.mk',
                     'scripts/prepare_board_source.py', 'scripts/check_shared_source_board.sh',
                     'scripts/validate_sources.py', 'scripts/fetch_sources.py']:
            target = firmware / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(FW / name, target)
        components = [('linux', 'LINUX'), ('u-boot', 'UBOOT'), ('buildroot', 'BUILDROOT'),
                      ('embeddedsw', 'EMBEDDEDSW'), ('device-tree-xlnx', 'DEVICE_TREE')]
        lock = {'schema': 1, 'components': {}}
        manifest = []
        stamps = firmware / '.source-stamps'
        stamps.mkdir()
        (stamps / 'active-board').write_text('e200\n')
        for name, prefix in components:
            target = firmware / name
            result = run('git', 'clone', '-q', '--no-hardlinks', self.repo, target)
            self.assertEqual(result.returncode, 0, result.stderr)
            url, ref = f'fixture://{name}', 'fixture-ref'
            run('git', '-C', target, 'remote', 'set-url', 'origin', url)
            (target / 'driver.c').write_text('E200 working patch\n')
            lock['components'][name] = {'commit': self.commit, 'url': url, 'ref': ref, 'directory': name}
            manifest += [f'{prefix}_REPO_URL := {url}', f'{prefix}_REPO_REF := {ref}']
            (stamps / f'{name}-{ref}.stamp').touch()
        (firmware / 'sources.lock.json').write_text(json.dumps(lock))
        (firmware / 'versions.mk').write_text('\n'.join(manifest) + '\n')
        for board in ['e310', 'e200', 'e310']:
            result = run('make', '-s', '-C', firmware, f'BOARD={board}', 'check-source-board')
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            for name, _ in components:
                self.assertEqual((firmware / name / 'driver.c').read_text(), 'E200 working patch\n')
        self.assertEqual((stamps / 'active-board').read_text(), 'e200\n')
        private = firmware / 'build/e310'
        self.assertEqual((private / '.source-stamps/active-board').read_text(), 'e310\n')
        for name in ['linux', 'u-boot', 'buildroot']:
            self.assertEqual((private / 'sources' / name / 'driver.c').read_text(), 'locked source\n')

    def test_unowned_destination_is_preserved(self):
        self.output.mkdir(parents=True)
        sentinel = self.output / 'keep'
        sentinel.write_text('preserve\n')
        result = run(*self.command)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('refusing', result.stderr.lower())
        self.assertEqual(sentinel.read_text(), 'preserve\n')

    def test_source_identity_change_requires_new_output(self):
        self.assertEqual(run(*self.command).returncode, 0)
        (self.repo / 'driver.c').write_text('next source\n')
        run('git', '-C', self.repo, 'commit', '-qam', 'next')
        self.lock['components']['linux']['commit'] = run('git', '-C', self.repo, 'rev-parse', 'HEAD').stdout.strip()
        (self.root / 'sources.lock.json').write_text(json.dumps(self.lock))
        result = run(*self.command)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('identity', result.stderr.lower())
        self.assertEqual((self.output / 'driver.c').read_text(), 'locked source\n')


if __name__ == '__main__':
    unittest.main()
