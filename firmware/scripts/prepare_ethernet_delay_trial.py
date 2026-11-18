#!/usr/bin/env python3
"""Prepare one reversible rgmii-rxid -> rgmii-id DTB trial, without rebuilding PL."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

from package_test_sd import verify_ethernet_dtb


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def dump(path):
    return subprocess.check_output(['dtc', '-q', '-s', '-I', 'dtb', '-O', 'dts', str(path)], text=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', type=Path, required=True, help='matched Ethernet boot-file directory')
    parser.add_argument('--output', type=Path, required=True, help='new directory; existing directories are refused')
    args = parser.parse_args()
    baseline, out = args.baseline.resolve(), args.output.resolve()
    if out.exists():
        parser.error('output already exists; choose a new directory')
    source = baseline / 'devicetree.dtb'
    info = verify_ethernet_dtb(source, expected_mode='rgmii-rxid')
    before = dump(source)
    original = 'phy-mode = "rgmii-rxid";'
    if before.count(original) != 1:
        parser.error('expected exactly one RGMII_RXID interface')
    out.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.ethernet-trial-', dir=out.parent) as temporary:
        stage = Path(temporary) / 'result'
        (stage / 'rollback').mkdir(parents=True)
        candidate = stage / 'devicetree.dtb'
        shutil.copy2(source, candidate)
        shutil.copy2(source, stage / 'rollback/devicetree.dtb')
        subprocess.run(['fdtput', '-t', 's', str(candidate), info['mac'], 'phy-mode', 'rgmii-id'], check=True)
        if dump(candidate) != before.replace(original, 'phy-mode = "rgmii-id";'):
            raise ValueError('unexpected DT changes beyond phy-mode')
        record = {'format': 'e200-ethernet-delay-trial-v1', 'baseline': str(baseline),
                  'mac': info['mac'], 'from_phy_mode': 'rgmii-rxid', 'to_phy_mode': 'rgmii-id',
                  'only_changed_property': 'phy-mode', 'trial_board_tested': False,
                  'baseline_dtb_sha256': digest(source), 'trial_dtb_sha256': digest(candidate),
                  'unchanged_boot_files_sha256': {n: digest(baseline / n) for n in
                      ('BOOT.BIN', 'e200_fpv.bit', 'zImage', 'rootfs.cpio.gz', 'uEnv.txt') if (baseline / n).exists()}}
        (stage / 'trial.json').write_text(json.dumps(record, indent=2) + '\n')
        (stage / 'README.txt').write_text(
            'E200 PHY TX delay A/B test: rgmii-rxid -> rgmii-id\n'
            'Use only with the matched 2026-11-17 Ethernet boot files.\n'
            'Replace only devicetree.dtb at the SD FAT32 boot-partition root, then power-cycle.\n'
            'All other five boot files stay identical. This is an untested hardware hypothesis.\n'
            'To revert, copy rollback/devicetree.dtb to the SD boot root and power-cycle.\n'
            'Host: 192.168.1.100/24; board: 192.168.1.10/24.\n'
            'Check the live /proc/device-tree/axi/ethernet@e000b000/phy-mode value.\n')
        names = ('devicetree.dtb', 'rollback/devicetree.dtb', 'trial.json', 'README.txt')
        (stage / 'SHA256SUMS').write_text(''.join(f'{digest(stage / n)}  {n}\n' for n in names))
        stage.rename(out)
    print('PASS only phy-mode changed; rollback DTB is byte-identical to baseline')
    print(out)


if __name__ == '__main__':
    main()
