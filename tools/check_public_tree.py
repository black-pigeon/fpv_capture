#!/usr/bin/env python3
"""Reject private implementation/artifacts in the Git index before a commit."""
import argparse
import hashlib
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys

PRIVATE_PREFIXES = (
    'fpga/', 'hardware/', 'firmware/board/e200/patches/autosave/',
    'firmware/u-boot/', 'firmware/linux/', 'firmware/buildroot/',
    'firmware/embeddedsw/', 'firmware/device-tree-xlnx/', 'firmware/build/',
    'firmware/.source-stamps/', 'build/', '.vscode/', '.Xil/',
)
PRIVATE_FILES = {
    'app/tests/test_capture_driver.py', 'app/tests/test_capture_driver_host.c',
    'tests/test_fpga_export.py', 'tests/test_e200_ethernet.py',
    'tests/check_capture_packets.py',
}
ARTIFACT_SUFFIXES = (
    '.bit', '.bin', '.xsa', '.ltx', '.dcp', '.xci', '.mmi', '.rpt', '.dtb',
    '.v', '.sv', '.vhd', '.vhdl', '.xdc', '.packet', '.pgm', '.bmp',
    '.tar', '.tar.gz', '.tgz', '.zip', '.gz', '.o', '.a', '.so', '.elf',
)
ALLOWED_ROOTS = {'app', 'firmware', 'tests', 'tools', '.githooks'}
KERNEL_SOURCE = re.compile(rb'\b(?:module_platform_driver|MODULE_LICENSE|module_init|module_exit)\s*\(')
PRIVATE_RTL = re.compile(rb'\bmodule\s+(?:fpv_|e200_fpv\b)')
PRIVATE_KEY = re.compile(rb'^-----BEGIN (?:[A-Z0-9]+ )?PRIVATE KEY-----', re.M)


def git(root, *args):
    return subprocess.check_output(['git', '-C', str(root), *args])


def path_reason(name):
    p = PurePosixPath(name)
    if name.startswith(PRIVATE_PREFIXES) or name in PRIVATE_FILES:
        return 'private/local path'
    if name.startswith('docs/'):
        return None if name == 'docs/public-repository.md' else 'internal documentation'
    if name in ('.gitignore', 'README.md'):
        return None
    if p.parts[0] not in ALLOWED_ROOTS:
        return 'path outside the application/integration allowlist'
    board_linux = 'firmware/board/e200/linux/'
    if name.startswith(board_linux) and name != board_linux + 'system-user.dtsi':
        return 'private kernel implementation'
    if name.lower().endswith(ARTIFACT_SUFFIXES):
        return 'hardware/source/archive/build or capture artifact'
    if p.name in ('.env', 'id_rsa', 'id_ed25519') or p.name.endswith('.pem'):
        return 'credential file'
    return None


def private_source_hashes(root):
    """Also catch byte-identical copies renamed outside their private directory."""
    hashes = set()
    for directory in (root / 'firmware/board/e200/linux', root / 'fpga/e200_fpv/src/fpv'):
        if not directory.is_dir():
            continue
        for source in directory.rglob('*'):
            if source.is_file() and not source.is_symlink() and source.suffix in ('.v', '.sv', '.c', '.h'):
                if source.name == 'system-user.dtsi':
                    continue
                hashes.add(hashlib.sha256(source.read_bytes()).digest())
    return hashes


def check(root):
    errors = []
    entries = git(root, 'ls-files', '--stage', '-z').split(b'\0')
    hashes = private_source_hashes(root)
    count = 0
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
        if content.startswith((b'\x7fELF', b'PK\x03\x04', b'\x1f\x8b')) or b'\0' in content:
            errors.append(f'{name}: binary/archive content')
        elif PRIVATE_KEY.search(content):
            errors.append(f'{name}: private key material')
        elif KERNEL_SOURCE.search(content) or PRIVATE_RTL.search(content):
            errors.append(f'{name}: kernel/FPGA implementation content')
        elif hashlib.sha256(content).digest() in hashes:
            errors.append(f'{name}: copy of local private source')
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
