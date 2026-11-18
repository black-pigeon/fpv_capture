#!/usr/bin/env python3
"""Verify and package the English scanner/fixed-frequency initramfs; no SD access."""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import shutil
import struct
import tarfile

from package_test_sd import ROOT, cpio_files, digest
from hardware_input import DEFAULT_XSA, DEFAULT_MANIFEST, load_hardware


def verify(rootfs, xsa=DEFAULT_XSA, hardware_manifest=DEFAULT_MANIFEST):
    files, modes = cpio_files(rootfs)
    target = ROOT / 'firmware/build/buildroot/e200/target'
    for name in ('fpv-webd', 'fpv-radio-probe', 'fpv-capture'):
        path = 'usr/sbin/' + name
        binary = files[path]
        assert binary[:4] == b'\x7fELF' and struct.unpack_from('<H', binary, 18)[0] == 40, path
        assert modes[path] & 0o111 and binary == (target / path).read_bytes(), path
        assert b'libiio' not in binary
    webd = files['usr/sbin/fpv-webd']
    assert b'window_phase' in webd and b'rf_tunes' in webd and b'offset_hz' in webd
    assert b'frequency_locked' in webd and b'captured_frames' in webd and b'waiting_signal' in webd
    for name in ('index.html', 'app.js', 'style.css'):
        content = files['usr/share/e200-fpv/web/' + name]
        assert content == (ROOT / 'app/web' / name).read_bytes(), name
        assert not any('\u4e00' <= ch <= '\u9fff' for ch in content.decode())
    assert b'Lock frequency' in files['usr/share/e200-fpv/web/index.html']
    assert b'lock-form' in files['usr/share/e200-fpv/web/app.js']
    assert b'<html lang="en">' in files['usr/share/e200-fpv/web/index.html']
    overlay = ROOT / 'firmware/board/e200/buildroot/rootfs-overlay'
    for path in ('etc/init.d/S95fpv-web', 'usr/sbin/fpv-net-diag'):
        assert files[path] == (overlay / path).read_bytes() and modes[path] & 0o111
    assert b'--simulate' not in files['etc/init.d/S95fpv-web']
    assert b'192.168.1.10' in files['etc/network/interfaces']
    assert not any('libiio' in name for name in files)
    manifest, _ = load_hardware(xsa, hardware_manifest)
    baseline = ROOT / 'firmware/build/e200/images/e200-fpv-ethernet-20261117-bootfiles/rootfs.cpio.gz'
    if baseline.exists():
        previous, _ = cpio_files(baseline)
        assert files['etc/network/interfaces'] == previous['etc/network/interfaces']
        assert files['etc/init.d/S95fpv-web'] == previous['etc/init.d/S95fpv-web']
    print(f'PASS: {len(files)} initramfs entries; ARM programs, English assets, hybrid status, v6 hardware contract, offline init and network baseline verified')
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rootfs', type=Path, default=ROOT / 'firmware/build/buildroot/e200/images/rootfs.cpio.gz')
    parser.add_argument('--name', default='e200-fpv-lock-20261118-rootfs')
    parser.add_argument('--verify-only', action='store_true')
    parser.add_argument('--xsa', type=Path, default=DEFAULT_XSA)
    parser.add_argument('--hardware-manifest', type=Path, default=DEFAULT_MANIFEST)
    args = parser.parse_args()
    if not args.name or Path(args.name).name != args.name or args.name in ('.', '..'):
        parser.error('--name must be a single directory name')
    manifest = verify(args.rootfs, args.xsa, args.hardware_manifest)
    if args.verify_only:
        return
    parent = ROOT / 'firmware/build/e200/images'
    out = parent / args.name
    archive = parent / (args.name + '.tar.gz')
    if out.exists() or archive.exists():
        parser.error('output already exists; select a new --name')
    out.mkdir(parents=True)
    shutil.copy2(args.rootfs, out / 'rootfs.cpio.gz')
    (out / 'README.txt').write_text(
        'E200 English UI + hybrid RF/DDC scan + fixed frequency, 2026-11-18\n'
        'Requires the currently working FPGA capture register v6, kernel and Ethernet DT.\n'
        '1. Back up rootfs.cpio.gz on the SD FAT32 boot partition.\n'
        '2. Replace ONLY rootfs.cpio.gz with the file in this directory.\n'
        '3. Keep the working BOOT.BIN, e200_fpv.bit, zImage, devicetree.dtb and uEnv.txt.\n'
        '4. Safely unmount the SD, boot the board, then hard-refresh the browser page.\n'
        'Web: http://192.168.1.10/ (when using the standard static IP).\n'
        'Fine preset: 2 MHz grid / 150 ms. Fast preset: 8 MHz grid / 100 ms.\n'
        'Four NCOs search distributed centers within up to 40 MHz per RF LO window.\n'
        'Digital phases skip RF SPI/settling but keep fresh FPGA tuning epochs.\n'
        'At 6 MHz, seven centers are searched in two phases before changing the LO.\n'
        'Scan progress uses a percentage instead of a changing point count.\n'
        'Coarse scans can miss signals. Narrow the band and use Fine for confirmation.\n'
        'Fixed frequency: enter MHz and press Lock frequency, or use Lock this frequency.\n'
        'Lock holds one LO/DDC epoch and continuously refreshes validated DMA frames.\n'
        'Signal loss stays on frequency; PAL/NTSC remains automatic. Unlock frequency resumes search. Default grid: 6 MHz.\n'
        'Browser checks for frames every 200 ms; actual frame rate requires hardware testing.\n'
        'This package does not contain a device tree or bitstream. Hardware speed and\n'
        'RF detection probability require board tests; host/browser tests use simulation.\n')
    paths = [ROOT / 'app' / p for p in ('scan/fpv_scan.h','scan/fpv_scan.c','scan/fpv_backend.h',
        'scan/fpv_backend.c','scan/fpv_service.h','scan/fpv_service.c','tools/fpv-webd.c',
        'web/index.html','web/app.js','web/style.css')]
    record = {'format':'e200-fpv-rootfs-update-v1', 'name':args.name,
        'created_utc':datetime.now(timezone.utc).isoformat(), 'required_fpga_register_version':6,
        'required_fpga_sha256':manifest['bitstream']['sha256'],
        'only_boot_file_to_replace':'rootfs.cpio.gz', 'rootfs_sha256':digest(out / 'rootfs.cpio.gz'),
        'source_sha256':{str(p.relative_to(ROOT)):digest(p) for p in paths},
        'features':['hybrid-scan', 'fixed-frequency-capture', 'automatic-pal-ntsc'],
        'on_board_tested':False}
    (out / 'release.json').write_text(json.dumps(record, indent=2) + '\n')
    names = ('rootfs.cpio.gz','README.txt','release.json')
    (out / 'SHA256SUMS').write_text(''.join(f'{digest(out / n)}  {n}\n' for n in names))
    with tarfile.open(archive, 'w:gz') as package:
        package.add(out, arcname=out.name)
    with tarfile.open(archive, 'r:gz') as package:
        for name in (*names, 'SHA256SUMS'):
            assert package.extractfile(out.name + '/' + name).read() == (out / name).read_bytes()
    assert digest(args.rootfs) == digest(out / 'rootfs.cpio.gz')
    print(out)
    print(f'{archive.stat().st_size} bytes  {archive}')
    print(f'{digest(archive)}  {archive.name}')


if __name__ == '__main__':
    main()
