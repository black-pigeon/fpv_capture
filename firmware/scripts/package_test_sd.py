#!/usr/bin/env python3
"""Verify and archive the six SD boot files; no block device access."""
import argparse
from datetime import datetime, timezone
import gzip
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tarfile

if __package__:
    from .hardware_input import DEFAULT_XSA, DEFAULT_MANIFEST, load_hardware
else:
    from hardware_input import DEFAULT_XSA, DEFAULT_MANIFEST, load_hardware

ROOT = Path(__file__).resolve().parents[2]
CORE = ('BOOT.BIN', 'e200_fpv.bit', 'zImage', 'devicetree.dtb', 'rootfs.cpio.gz', 'uEnv.txt')
def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def cpio_files(path):
    raw = gzip.decompress(path.read_bytes())
    files, modes, pos = {}, {}, 0
    while True:
        header = raw[pos:pos+110]
        if header[:6] not in (b'070701', b'070702'):
            raise ValueError('invalid initramfs header')
        values = [int(header[6+i*8:14+i*8], 16) for i in range(13)]
        mode, size, namesize = values[1], values[6], values[11]
        name = raw[pos+110:pos+110+namesize-1].decode()
        pos = (pos+110+namesize+3) & ~3
        data = raw[pos:pos+size]
        if len(data) != size:
            raise ValueError('truncated initramfs')
        pos = (pos+size+3) & ~3
        if name == 'TRAILER!!!':
            return files, modes
        name = name.removeprefix('./')
        files[name], modes[name] = data, mode

def verify_ethernet_dtb(dtb, expected_mode='rgmii-id'):
    """Check resolved DT properties, including both PHY phandles, before delivery."""
    def get(node, prop, kind='s'):
        return subprocess.check_output(['fdtget', '-t', kind, str(dtb), node, prop],
                                       text=True).strip()
    mac = get('/aliases', 'ethernet0')
    assert get(mac, 'status') == 'okay', 'GEM0 must be enabled in Linux'
    assert get(mac, 'phy-mode') == expected_mode, 'E200 PHY delay mode drifted'
    bus = mac + '/mdio'
    names = subprocess.check_output(['fdtget', '-l', str(dtb), bus], text=True).split()
    assert len(names) == 2, 'expected one external PHY and one converter'
    nodes = [bus + '/' + name for name in names]
    phy = next(n for n in nodes if get(n, 'reg', 'u') == '1')
    bridge = next(n for n in nodes if get(n, 'reg', 'u') == '8')
    assert get(phy, 'compatible') == 'ethernet-phy-ieee802.3-c22', 'must read the hardware PHY ID'
    assert get(bridge, 'compatible') == 'xlnx,gmii-to-rgmii-1.0'
    assert get(mac, 'phy-handle', 'x') == get(phy, 'phandle', 'x')
    assert get(bridge, 'phy-handle', 'x') == get(phy, 'phandle', 'x')
    return {'mac': mac, 'phy_mode': expected_mode, 'phy_address': 1,
            'converter_address': 8, 'forced_phy_id': False,
            'board_link_tested': False}

def verify(sd, xsa=DEFAULT_XSA, hardware_manifest=DEFAULT_MANIFEST):
    for name in CORE:
        assert (sd / name).stat().st_size > 0, name
    manifest, bitstream = load_hardware(xsa, hardware_manifest)
    assert (sd / 'e200_fpv.bit').read_bytes() == bitstream, 'SD bitstream differs from released XSA'
    assert (sd / 'devicetree.dtb').read_bytes() == (ROOT / 'firmware/build/e200/system-top.dtb').read_bytes()
    verify_ethernet_dtb(sd / 'devicetree.dtb')
    files, modes = cpio_files(sd / 'rootfs.cpio.gz')
    for name in ('fpv-webd', 'fpv-radio-probe', 'fpv-capture'):
        data = files['usr/sbin/' + name]
        assert data[:4] == b'\x7fELF' and struct.unpack_from('<H', data, 18)[0] == 40
        assert data == (ROOT / 'firmware/build/buildroot/e200/target/usr/sbin' / name).read_bytes()
        assert b'libiio' not in data
    init = 'etc/init.d/S95fpv-web'
    assert modes[init] & 0o111
    assert files[init] == (ROOT / 'firmware/board/e200/buildroot/rootfs-overlay' / init).read_bytes()
    diag = 'usr/sbin/fpv-net-diag'
    assert modes[diag] & 0o111
    assert files[diag] == (ROOT / 'firmware/board/e200/buildroot/rootfs-overlay' / diag).read_bytes()
    for name in ('index.html', 'style.css', 'app.js'):
        assert files['usr/share/e200-fpv/web/' + name] == (ROOT / 'app/web' / name).read_bytes(), name
    assert b'192.168.1.10' in files['etc/network/interfaces']
    assert not any('libiio' in name for name in files)
    assert (sd / 'zImage').read_bytes() == (ROOT / 'firmware/linux/arch/arm/boot/zImage').read_bytes()
    assert struct.unpack_from('<I', (sd / 'BOOT.BIN').read_bytes(), 0x20)[0] == 0xaa995566
    print(f'PASS: matched public XSA/bitstream and v6 ABI, {len(files)} initramfs entries, ARM apps, web/init assets, Ethernet DT and diagnostics, kernel and boot header')
    return manifest

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--name', default='e200-fpv-webscan-20261117')
    parser.add_argument('--verify-only', action='store_true')
    parser.add_argument('--xsa', type=Path, default=DEFAULT_XSA)
    parser.add_argument('--hardware-manifest', type=Path, default=DEFAULT_MANIFEST)
    args = parser.parse_args()
    if '/' in args.name or args.name in ('.', '..'):
        parser.error('name must be a filename prefix')
    sd = ROOT / 'firmware/build/e200/sd'
    manifest = verify(sd, args.xsa, args.hardware_manifest)
    if args.verify_only:
        return
    out = ROOT / 'firmware/build/e200/images'
    out.mkdir(parents=True, exist_ok=True)
    (sd / 'fpga-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    inputs = [Path(__file__).resolve(), ROOT / 'firmware/sources.lock.json', ROOT / 'firmware/linux/.config',
              ROOT / 'firmware/board/e200/linux/fpv_capture/fpv_capture.c',
              ROOT / 'firmware/board/e200/buildroot/defconfig',
              ROOT / 'firmware/board/e200/buildroot/rootfs-overlay/etc/init.d/S95fpv-web',
              ROOT / 'firmware/board/e200/buildroot/rootfs-overlay/usr/sbin/fpv-net-diag',
              ROOT / 'firmware/board/e200/linux/system-user.dtsi',
              ROOT / 'firmware/board/e200/boot/uEnv.txt']
    inputs += sorted(p for p in (ROOT / 'app').rglob('*') if p.is_file())
    record = {'format': 'e200-fpv-webscan-test-v1',
              'created_utc': datetime.now(timezone.utc).isoformat(),
              'hardware_register_version': 6, 'video_packet_version': 2,
              'board_boot_tested': False, 'rf_detection_tested': False,
              'ethernet': verify_ethernet_dtb(sd / 'devicetree.dtb'),
              'web_url': 'http://192.168.1.10/',
              'boot_files': {n: {'bytes': (sd / n).stat().st_size, 'sha256': digest(sd / n)} for n in CORE},
              'source_sha256': {str(p.relative_to(ROOT)): digest(p) for p in inputs}}
    (sd / 'test-build.json').write_text(json.dumps(record, indent=2) + '\n')
    names = list(CORE) + ['fpga-manifest.json', 'test-build.json']
    # Refresh Make's informational manifest after replacing the metadata files;
    # omit the checksum files themselves to avoid circular hash dependencies.
    lines = ['format=e200-sd-boot-manifest-v1', 'board=e200',
             'version=' + args.name, 'created_utc=' + record['created_utc']]
    for name in names:
        lines += [f'file.{name}.size={(sd / name).stat().st_size}',
                  f'file.{name}.sha256={digest(sd / name)}']
    (sd / 'e200-firmware.manifest').write_text('\n'.join(lines) + '\n')
    names.append('e200-firmware.manifest')
    (sd / 'SHA256SUMS').write_text(''.join(f'{digest(sd / n)}  {n}\n' for n in names))
    archive = out / (args.name + '-bootfiles.tar.gz')
    with tarfile.open(archive, 'w:gz') as tar:
        for name in names + ['SHA256SUMS']:
            tar.add(sd / name, arcname=name)
    with tarfile.open(archive) as tar:
        assert sorted(tar.getnames()) == sorted(names + ['SHA256SUMS'])
        for name in names:
            assert tar.extractfile(name).read() == (sd / name).read_bytes(), name
    (out / (args.name + '-SHA256SUMS')).write_text(f'{digest(archive)}  {archive.name}\n')
    print(f'PASS: boot-file archive readback; {archive} ({archive.stat().st_size} bytes)')
if __name__ == '__main__':
    main()
