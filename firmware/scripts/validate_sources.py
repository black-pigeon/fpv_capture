#!/usr/bin/env python3
"""Check build source identity against the lock, allowing project patch edits."""
import argparse
from pathlib import Path
import re
import sys

from fetch_sources import COMPONENTS, git, read_lock, read_manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--manifest', type=Path, required=True)
    args = parser.parse_args()
    root = args.root.resolve()
    manifest = read_manifest(args.manifest)
    lock = read_lock(root / 'sources.lock.json')
    for name, prefix in COMPONENTS.items():
        entry = lock['components'].get(name, {})
        url = manifest.get(f'{prefix}_REPO_URL')
        ref = manifest.get(f'{prefix}_REPO_REF')
        commit = entry.get('commit', '')
        if (not url or not ref or entry.get('url') != url
                or entry.get('ref') != ref or not re.fullmatch(r'[0-9a-f]{40}', commit)):
            raise ValueError(f'{name}: manifest/lock mismatch; review versions.mk and sources.lock.json')
        target = root / name
        if target.is_symlink():
            raise ValueError(f'{name}: refusing symlink source directory')
        if not target.exists():
            # The corresponding fetch-stamp rule will obtain this component.
            continue
        if not (target / '.git').is_dir() or (target / '.git').is_symlink():
            raise ValueError(f'{name}: expected a standalone Git checkout')
        if Path(git(target, 'rev-parse', '--show-toplevel')).resolve() != target:
            raise ValueError(f'{name}: not an independent repository root')
        if git(target, 'remote', 'get-url', 'origin') != url:
            raise ValueError(f'{name}: origin differs from locked source')
        if git(target, 'rev-parse', 'HEAD') != commit:
            raise ValueError(f'{name}: HEAD differs from source lock; preserve edits and restore the locked commit in a clean checkout')
        # Local board patches are expected during builds. This read-only check
        # intentionally does not apply the stricter clean-tree fetch policy.


if __name__ == '__main__':
    try:
        main()
    except (RuntimeError, ValueError, OSError) as error:
        print(f'source check: {error}', file=sys.stderr)
        sys.exit(1)
