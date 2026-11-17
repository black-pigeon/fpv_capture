#!/usr/bin/env python3
"""Apply a board patch series; accept already-applied patches, reject conflicts."""
from pathlib import Path
import subprocess
import sys


def apply(source, patch_directory):
    if not source.is_dir():
        raise ValueError(f'missing source directory: {source}')
    if not patch_directory.is_dir():
        raise ValueError(f'missing patch directory: {patch_directory}')
    for path in sorted(patch_directory.glob('*.patch')):
        content = path.read_bytes()
        base = ['patch', '--batch', '--no-backup-if-mismatch', '-p1', '-d', str(source)]
        forward = subprocess.run(base + ['--forward', '--dry-run'], input=content,
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if forward.returncode == 0:
            subprocess.run(base + ['--forward'], input=content, check=True)
            print(f'applied: {path.name}')
        else:
            reverse = subprocess.run(base + ['--reverse', '--dry-run'], input=content,
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            if reverse.returncode != 0:
                raise ValueError(f'patch conflict: {path}\n' +
                                 forward.stdout.decode(errors='replace'))
            print(f'already applied: {path.name}')


if __name__ == '__main__':
    try:
        if len(sys.argv) != 3:
            raise ValueError('usage: apply_patches.py <source> <patch-directory>')
        apply(Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve())
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        print(error, file=sys.stderr)
        sys.exit(1)
