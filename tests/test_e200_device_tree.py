"""Verify public device-tree PHY topology and the board-tested delay mode."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from firmware.scripts.package_test_sd import verify_ethernet_dtb

ROOT = Path(__file__).resolve().parents[1]
BASE = r'''/dts-v1/;
/ {
    #address-cells = <1>; #size-cells = <1>;
    gem0: ethernet@e000b000 {
        reg = <0xe000b000 0x1000>;
        phy-handle = <&phy1>;
        mdio {
            #address-cells = <1>; #size-cells = <0>;
            gmii_to_rgmii_0: converter@8 {
                compatible = "xlnx,gmii-to-rgmii-1.0";
                reg = <8>; phy-handle = <&phy1>;
            };
        };
    };
    sdhci0: mmc {}; uart0: serial {}; uart1: serial1 {}; spi0: spi {};
    qspi: qspi {}; i2c0: i2c {};
    gpio0: gpio { gpio-controller; #gpio-cells = <2>; };
    clkc: clock {}; fpv_dma: dma {};
};
'''


class EthernetDeviceTreeTest(unittest.TestCase):
    def test_compiled_phy_topology_and_delay(self):
        for tool in ('dtc', 'fdtget'):
            if not shutil.which(tool):
                self.skipTest(f'{tool} unavailable')
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            dts, dtb = directory / 'board.dts', directory / 'board.dtb'
            dts.write_text(BASE + (ROOT / 'firmware/board/e200/linux/system-user.dtsi').read_text())
            result = subprocess.run(['dtc', '-I', 'dts', '-O', 'dtb', '-o', str(dtb), str(dts)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            def get(node, prop, kind='s'):
                return subprocess.check_output(['fdtget', '-t', kind, str(dtb), node, prop], text=True).strip()
            # Check the resolved GPIO phandle as well as polarity and trigger:
            # this must use PS MIO0, not the AD9361 EMIO control pins.
            led = '/leds/led0'
            self.assertEqual(get('/leds', 'compatible'), 'gpio-leds')
            self.assertEqual(get(led, 'label'), 'led0:green')
            self.assertEqual(get(led, 'linux,default-trigger'), 'heartbeat')
            self.assertEqual(get(led, 'default-state'), 'off')
            self.assertEqual(get(led, 'gpios', 'u').split(),
                             [get('/gpio', 'phandle', 'u'), '0', '0'])
            mac = '/ethernet@e000b000'
            self.assertEqual(get(mac, 'phy-mode'), 'rgmii-id')
            # Find the PHY and converter whether or not DTG supplied a container.
            def children(node):
                names = subprocess.check_output(['fdtget', '-l', str(dtb), node], text=True).split()
                return [node + '/' + name for name in names]
            nodes = children(mac)
            if mac + '/mdio' in nodes:
                nodes += children(mac + '/mdio')
            phy = next(n for n in nodes if 'ethernet-phy-ieee802.3-c22' in
                       subprocess.run(['fdtget', str(dtb), n, 'compatible'], capture_output=True, text=True).stdout)
            self.assertEqual(get(phy, 'compatible'), 'ethernet-phy-ieee802.3-c22',
                             'read the real ID over MDIO; a forced ID can hide a disconnected PHY')
            self.assertEqual(get(phy, 'reg', 'u'), '1')
            converter = next(n for n in nodes if 'xlnx,gmii-to-rgmii-1.0' in
                             subprocess.run(['fdtget', str(dtb), n, 'compatible'], capture_output=True, text=True).stdout)
            self.assertEqual(get(converter, 'reg', 'u'), '8')
            self.assertEqual(get(mac, 'phy-handle', 'x'), get(phy, 'phandle', 'x'))
            self.assertEqual(get(converter, 'phy-handle', 'x'), get(phy, 'phandle', 'x'))
            self.assertEqual(sum(n.endswith('/mdio') for n in nodes), 1)
            report = verify_ethernet_dtb(dtb)
            self.assertFalse(report['board_link_tested'])
            self.assertEqual(report['phy_address'], 1)


if __name__ == '__main__':
    unittest.main()
