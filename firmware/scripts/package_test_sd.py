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
    from .hardware_input import load_hardware
else:
    from hardware_input import load_hardware

ROOT = Path(__file__).resolve().parents[2]


def core_files(board='e200'):
    if board not in ('e200', 'e310'):
        raise ValueError(f'unsupported board: {board}')
    return ('BOOT.BIN', f'{board}_fpv.bit', 'zImage', 'devicetree.dtb', 'rootfs.cpio.gz', 'uEnv.txt')


def kernel_source(board='e200'):
    core_files(board)  # Validate before constructing paths.
    return ROOT / ('firmware/linux' if board == 'e200' else f'firmware/build/{board}/sources/linux')


CORE = core_files()
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

def verify_ethernet_dtb(dtb, expected_mode='rgmii-id', board='e200'):
    """Check the selected board's resolved MAC, PHY and reset topology."""
    core_files(board)
    def get(node, prop, kind='s'):
        return subprocess.check_output(['fdtget', '-t', kind, str(dtb), node, prop],
                                       text=True).strip()
    mac = get('/aliases', 'ethernet0')
    assert get(mac, 'status') == 'okay', 'GEM0 must be enabled in Linux'
    assert get(mac, 'phy-mode') == expected_mode, f'{board} PHY delay mode drifted'
    bus = mac + '/mdio'
    names = subprocess.check_output(['fdtget', '-l', str(dtb), bus], text=True).split()
    nodes = [bus + '/' + name for name in names]
    if board == 'e310':
        assert len(nodes) == 1, 'E310 expects one external PHY and no converter'
        phy = nodes[0]
        assert get(phy, 'reg', 'u') == '0', 'E310 PHY address must be 0'
        assert get(phy, 'compatible') == 'ethernet-phy-ieee802.3-c22', 'must read the hardware PHY ID'
        assert get(mac, 'phy-handle', 'x') == get(phy, 'phandle', 'x')
        reset = get(phy, 'reset-gpios', 'u').split()
        assert len(reset) == 3 and int(reset[0]) > 0 and reset[1:] == ['46', '1'], 'E310 PHY reset must be GPIO46 active low'
        return {'mac': mac, 'interface': 'ps-gem-mio', 'phy_mode': expected_mode,
                'phy_address': 0, 'converter_address': None, 'forced_phy_id': False,
                'board_link_tested': False}
    assert len(names) == 2, 'expected one external PHY and one converter'
    phy = next(n for n in nodes if get(n, 'reg', 'u') == '1')
    bridge = next(n for n in nodes if get(n, 'reg', 'u') == '8')
    assert get(phy, 'compatible') == 'ethernet-phy-ieee802.3-c22', 'must read the hardware PHY ID'
    assert get(bridge, 'compatible') == 'xlnx,gmii-to-rgmii-1.0'
    assert get(mac, 'phy-handle', 'x') == get(phy, 'phandle', 'x')
    assert get(bridge, 'phy-handle', 'x') == get(phy, 'phandle', 'x')
    return {'mac': mac, 'phy_mode': expected_mode, 'phy_address': 1,
            'converter_address': 8, 'forced_phy_id': False,
            'board_link_tested': False}

def verify_spectrum_dtb(dtb):
    """Resolve the spectrum consumer to its independent S2MM channel and IRQ."""
    def get(node, prop, kind='s'):
        return subprocess.check_output(['fdtget', '-t', kind, str(dtb), node, prop],
                                       text=True).strip()
    consumer = '/fpv-spectrum@43c40000'
    dma = '/amba_pl/dma@40440000'
    channel = dma + '/dma-channel@40440030'
    assert get(consumer, 'compatible') == 'microphase,fpv-spectrum-1.0'
    assert get(consumer, 'status') == 'okay'
    assert get(consumer, 'reg', 'x').split() == ['43c40000', '10000']
    assert get(consumer, 'dma-names') == 'rx'
    assert get(consumer, 'dmas', 'x').split() == [get(dma, 'phandle', 'x'), '1'], 'spectrum DMA phandle/channel mismatch'
    assert get(dma, '#dma-cells', 'u') == '1'
    assert get(dma, 'reg', 'x').split() == ['40440000', '10000']
    assert get(channel, 'compatible') == 'xlnx,axi-dma-s2mm-channel'
    assert get(channel, 'interrupts', 'u').split() == ['0', '31', '4'], 'spectrum DMA IRQ mismatch'
    assert get(channel, 'xlnx,datawidth', 'u') == '32'

def verify_spectrum_assets(files, manifest):
    """A spectrum handoff must ship its matching viewer and HTTP endpoint."""
    if 'spectrum' not in manifest:
        return
    asset = 'usr/share/e200-fpv/web/spectrum.js'
    assert files.get(asset) == (ROOT / 'app/web/spectrum.js').read_bytes(), asset
    assert b'/api/spectrum' in files['usr/sbin/fpv-webd'], 'spectrum API missing'

def verify(sd, xsa=None, hardware_manifest=None, board='e200'):
    for name in core_files(board):
        assert (sd / name).stat().st_size > 0, name
    manifest, bitstream = load_hardware(xsa, hardware_manifest, board=board)
    assert (sd / f'{board}_fpv.bit').read_bytes() == bitstream, 'SD bitstream differs from released XSA'
    assert (sd / 'devicetree.dtb').read_bytes() == (ROOT / f'firmware/build/{board}/system-top.dtb').read_bytes()
    verify_ethernet_dtb(sd / 'devicetree.dtb', board=board)
    files, modes = cpio_files(sd / 'rootfs.cpio.gz')
    for name in ('fpv-webd', 'fpv-radio-probe', 'fpv-capture'):
        data = files['usr/sbin/' + name]
        assert data[:4] == b'\x7fELF' and struct.unpack_from('<H', data, 18)[0] == 40
        assert data == (ROOT / f'firmware/build/buildroot/{board}/target/usr/sbin' / name).read_bytes()
        assert b'libiio' not in data
    init = 'etc/init.d/S95fpv-web'
    assert modes[init] & 0o111
    assert files[init] == (ROOT / f'firmware/board/{board}/buildroot/rootfs-overlay' / init).read_bytes()
    diag = 'usr/sbin/fpv-net-diag'
    assert modes[diag] & 0o111
    assert files[diag] == (ROOT / f'firmware/board/{board}/buildroot/rootfs-overlay' / diag).read_bytes()
    for name in ('index.html', 'style.css', 'app.js'):
        assert files['usr/share/e200-fpv/web/' + name] == (ROOT / 'app/web' / name).read_bytes(), name
    verify_spectrum_assets(files, manifest)
    if 'spectrum' in manifest:
        verify_spectrum_dtb(sd / 'devicetree.dtb')
    assert b'192.168.1.10' in files['etc/network/interfaces']
    assert not any('libiio' in name for name in files)
    assert (sd / 'zImage').read_bytes() == (kernel_source(board) / 'arch/arm/boot/zImage').read_bytes()
    assert struct.unpack_from('<I', (sd / 'BOOT.BIN').read_bytes(), 0x20)[0] == 0xaa995566
    print(f'PASS: matched public XSA/bitstream and v6 ABI, {len(files)} initramfs entries, ARM apps, web/init assets, Ethernet DT and diagnostics, kernel and boot header')
    return manifest

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--board', choices=('e200', 'e310'), default='e200')
    parser.add_argument('--name')
    parser.add_argument('--verify-only', action='store_true')
    parser.add_argument('--xsa', type=Path)
    parser.add_argument('--hardware-manifest', type=Path)
    args = parser.parse_args()
    board = args.board
    if args.name is None:
        args.name = 'e200-fpv-webscan-20261117' if board == 'e200' else 'e310-fpv-webscan-20261119'
    if '/' in args.name or args.name in ('.', '..'):
        parser.error('name must be a filename prefix')
    sd = ROOT / f'firmware/build/{board}/sd'
    manifest = verify(sd, args.xsa, args.hardware_manifest, board=board)
    if args.verify_only:
        return
    out = ROOT / f'firmware/build/{board}/images'
    out.mkdir(parents=True, exist_ok=True)
    (sd / 'fpga-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    inputs = [Path(__file__).resolve(), ROOT / 'firmware/sources.lock.json', kernel_source(board) / '.config',
              ROOT / 'firmware/board/e200/linux/fpv_capture/fpv_capture.c',
              ROOT / 'firmware/board/e200/linux/fpv_capture/fpv_spectrum.c',
              ROOT / f'firmware/board/{board}/buildroot/defconfig',
              ROOT / f'firmware/board/{board}/buildroot/rootfs-overlay/etc/init.d/S95fpv-web',
              ROOT / f'firmware/board/{board}/buildroot/rootfs-overlay/usr/sbin/fpv-net-diag',
              ROOT / f'firmware/board/{board}/linux/system-user.dtsi',
              ROOT / f'firmware/board/{board}/boot/uEnv.txt']
    inputs += sorted(p for p in (ROOT / 'app').rglob('*') if p.is_file())
    record = {'format': f'{board}-fpv-webscan-test-v1', 'board': board,
              'created_utc': datetime.now(timezone.utc).isoformat(),
              'hardware_register_version': 6, 'video_packet_version': 2,
              'board_boot_tested': False, 'rf_detection_tested': False,
              'ethernet': verify_ethernet_dtb(sd / 'devicetree.dtb', board=board),
              'web_url': 'http://192.168.1.10/',
              'boot_files': {n: {'bytes': (sd / n).stat().st_size, 'sha256': digest(sd / n)} for n in core_files(board)},
              'source_sha256': {str(p.relative_to(ROOT)): digest(p) for p in inputs}}
    (sd / 'test-build.json').write_text(json.dumps(record, indent=2) + '\n')
    names = list(core_files(board)) + ['fpga-manifest.json', 'test-build.json']
    # Refresh Make's informational manifest after replacing the metadata files;
    # omit the checksum files themselves to avoid circular hash dependencies.
    lines = [f'format={board}-sd-boot-manifest-v1', f'board={board}',
             'version=' + args.name, 'created_utc=' + record['created_utc']]
    for name in names:
        lines += [f'file.{name}.size={(sd / name).stat().st_size}',
                  f'file.{name}.sha256={digest(sd / name)}']
    (sd / f'{board}-firmware.manifest').write_text('\n'.join(lines) + '\n')
    names.append(f'{board}-firmware.manifest')
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
