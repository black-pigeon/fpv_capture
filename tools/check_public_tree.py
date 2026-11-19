#!/usr/bin/env python3
"""Reject private implementation/artifacts in the Git index before a commit."""
import argparse
import hashlib
import io
import json
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'firmware/scripts'))
from hardware_input import SOURCE_PAYLOAD, validate_xsa

PUBLIC_XSA = 'firmware/board/e200/hardware/e200_fpv.xsa'
PUBLIC_MANIFEST = 'firmware/board/e200/hardware/manifest.json'
PUBLIC_HARDWARE = {
    'e200': (PUBLIC_XSA, PUBLIC_MANIFEST),
    'e310': ('firmware/board/e310/hardware/e310_fpv.xsa',
             'firmware/board/e310/hardware/manifest.json'),
}
PUBLIC_HARDWARE_FILES = {name for pair in PUBLIC_HARDWARE.values() for name in pair}
# Only the reviewed, unmodified branding asset is allowed as a standalone image.
PUBLIC_ASSET_HASHES = {
    'app/web/microphase-logo.png':
        '387b80817abed201a8acc7a874f1c0ef356c955b3540bd3e38d53bee06b726da',
}
PUBLIC_DRIVER_FILES = {
    'firmware/board/e200/linux/fpv_capture/' + name
    for name in ('Kconfig', 'Makefile', 'fpv_capture.c', 'COPYING')
}

PRIVATE_PREFIXES = (
    'fpga/', 'hardware/', 'images/', 'firmware/board/e200/patches/autosave/',
    'firmware/u-boot/', 'firmware/linux/', 'firmware/buildroot/',
    'firmware/embeddedsw/', 'firmware/device-tree-xlnx/', 'firmware/build/',
    'firmware/.source-stamps/', 'build/', '.vscode/', '.Xil/',
)
PRIVATE_FILES = {
    'tests/test_fpga_export.py', 'tests/test_e200_ethernet.py',
    'tests/check_capture_packets.py',
}
ARTIFACT_SUFFIXES = (
    '.bit', '.bin', '.xsa', '.ltx', '.dcp', '.xci', '.mmi', '.rpt', '.dtb',
    '.v', '.sv', '.vhd', '.vhdl', '.xdc', '.packet', '.pgm', '.bmp',
    '.tar', '.tar.gz', '.tgz', '.zip', '.gz', '.o', '.a', '.so', '.elf', '.ko',
)
ALLOWED_ROOTS = {'app', 'firmware', 'tests', 'tools', '.githooks'}
KERNEL_SOURCE = re.compile(rb'\b(?:module_platform_driver|MODULE_LICENSE|module_init|module_exit)\s*\(')
PRIVATE_RTL = re.compile(rb'\bmodule\s+(?:fpv_|e(?:200|310)_fpv\b)')
PRIVATE_KEY = re.compile(rb'^-----BEGIN (?:[A-Z0-9]+ )?PRIVATE KEY-----', re.M)


def git(root, *args):
    return subprocess.check_output(['git', '-C', str(root), *args])


def path_reason(name):
    p = PurePosixPath(name)
    if name.startswith(PRIVATE_PREFIXES) or name in PRIVATE_FILES:
        return 'private/local path'
    if name.startswith('docs/'):
        return 'internal documentation'
    if name.endswith('.md') and name != 'README.md':
        return 'documentation belongs in the top-level README'
    if name in ('.gitignore', 'README.md'):
        return None
    if p.parts[0] not in ALLOWED_ROOTS:
        return 'path outside the application/integration allowlist'
    board_linux = 'firmware/board/e200/linux/'
    if name.startswith(board_linux) and name != board_linux + 'system-user.dtsi' and name not in PUBLIC_DRIVER_FILES:
        return 'private kernel implementation'
    if name in PUBLIC_HARDWARE_FILES:
        return None
    if any(name.startswith(str(PurePosixPath(pair[0]).parent) + '/')
           for pair in PUBLIC_HARDWARE.values()):
        return 'unapproved hardware export'
    if name.lower().endswith(ARTIFACT_SUFFIXES):
        return 'hardware/source/archive/build or capture artifact'
    if p.name in ('.env', 'id_rsa', 'id_ed25519') or p.name.endswith('.pem'):
        return 'credential file'
    return None


def private_source_hashes(root):
    """Also catch byte-identical copies renamed outside their private directory."""
    hashes = set()
    for directory in (root / 'fpga/e200_fpv', root / 'fpga/e310_fpv',
                      root / 'firmware/board/e200/linux/axi_dma'):
        if not directory.is_dir():
            continue
        for source in directory.rglob('*'):
            if source.is_file() and not source.is_symlink() and source.suffix.lower() in (
                    '.v', '.sv', '.vhd', '.vhdl', '.tcl', '.xdc', '.c', '.h'):
                # Generated PS initialization is explicitly public XSA content.
                if source.name in ('system-user.dtsi', 'ps7_init.c', 'ps7_init.h',
                                   'ps7_init_gpl.c', 'ps7_init_gpl.h', 'ps7_init.tcl'):
                    continue
                hashes.add(hashlib.sha256(source.read_bytes()).digest())
    return hashes


def check(root):
    errors = []
    entries = git(root, 'ls-files', '--stage', '-z').split(b'\0')
    hashes = private_source_hashes(root)
    count = 0
    hardware = {}
    for entry in entries:
        if not entry:
            continue
        meta, raw_path = entry.split(b'\t', 1)
        mode, oid, stage = meta.split()
        name = raw_path.decode('utf-8', 'surrogateescape')
        count += 1
        reason = path_reason(name)
        if reason:
            errors.append(f'{name}: {reason}')
            continue
        if stage != b'0' or mode not in (b'100644', b'100755'):
            errors.append(f'{name}: only resolved regular files may be committed')
            continue
        content = git(root, 'cat-file', 'blob', oid.decode())
        if name in PUBLIC_HARDWARE_FILES:
            hardware[name] = content
            continue
        if name in PUBLIC_ASSET_HASHES:
            if hashlib.sha256(content).hexdigest() != PUBLIC_ASSET_HASHES[name]:
                errors.append(f'{name}: unreviewed branding asset')
            continue
        if content.startswith((b'\x7fELF', b'PK\x03\x04', b'\x1f\x8b')) or b'\0' in content:
            errors.append(f'{name}: binary/archive content')
        elif PRIVATE_KEY.search(content):
            errors.append(f'{name}: private key material')
        elif ((KERNEL_SOURCE.search(content) and name not in PUBLIC_DRIVER_FILES)
              or PRIVATE_RTL.search(content) or SOURCE_PAYLOAD.search(content)):
            errors.append(f'{name}: kernel/FPGA implementation content')
        elif hashlib.sha256(content).digest() in hashes:
            errors.append(f'{name}: copy of local private source')
    for board, (xsa_path, manifest_path) in PUBLIC_HARDWARE.items():
        pair = {xsa_path, manifest_path}
        if not pair.intersection(hardware):
            continue
        try:
            if not pair.issubset(hardware):
                raise ValueError('XSA and public manifest must both be indexed')
            validate_xsa(hardware[xsa_path], json.loads(hardware[manifest_path]), board=board)
            # Names alone cannot catch a private script renamed as PS init data.
            # Validation above has already bounded and checked the archive.
            with zipfile.ZipFile(io.BytesIO(hardware[xsa_path])) as archive:
                for entry in archive.infolist():
                    if hashlib.sha256(archive.read(entry)).digest() in hashes:
                        raise ValueError('copy of local private source in XSA member: ' + entry.filename)
        except (ValueError, OSError) as error:
            errors.append(f'{xsa_path}: {error}')
    if errors:
        print('Commit blocked: excluded private content is present in the index.', file=sys.stderr)
        for error in errors:
            print('  ' + error, file=sys.stderr)
        return 1
    print(f'PASS: {count} indexed files; no excluded private paths, source copies or artifacts')
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    return check(args.root.resolve())


if __name__ == '__main__':
    sys.exit(main())
