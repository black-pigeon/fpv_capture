#!/usr/bin/env python3
"""Fetch pinned component sources without discarding local changes."""
import argparse
import fcntl
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

COMPONENTS = {
    'u-boot': 'UBOOT', 'linux': 'LINUX', 'buildroot': 'BUILDROOT',
    'embeddedsw': 'EMBEDDEDSW', 'device-tree-xlnx': 'DEVICE_TREE',
}


def git(directory, *args):
    result = subprocess.run(['git', '-C', str(directory), *args],
                            text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE)
    if result.returncode:
        raise RuntimeError(result.stderr.strip() or f'git {args[0]} failed')
    return result.stdout.strip()


def read_manifest(path):
    values = {}
    for line in path.read_text().splitlines():
        match = re.fullmatch(r'\s*([A-Z_]+)\s*(?:\?=|:=|=)\s*([^#]*?)\s*(?:#.*)?', line)
        if match:
            key, value = match.groups()
            if '$' in value:
                raise ValueError(f'{path}: {key} must be a literal, not a Make expansion')
            values[key] = value.strip()
    return values


def read_lock(path):
    if not path.exists():
        return {'schema': 1, 'components': {}}
    lock = json.loads(path.read_text())
    if lock.get('schema') != 1 or not isinstance(lock.get('components'), dict):
        raise ValueError(f'unsupported source lock: {path}')
    return lock


def check_existing(target, url):
    if target.is_symlink():
        raise ValueError(f'refusing symlink source directory: {target}')
    if not target.exists():
        return
    metadata = target / '.git'
    if not metadata.is_dir() or metadata.is_symlink():
        raise ValueError(f'{target} must be a standalone Git checkout')
    if Path(git(target, 'rev-parse', '--show-toplevel')).resolve() != target:
        raise ValueError(f'not a repository root: {target}')
    if git(target, 'remote', 'get-url', 'origin') != url:
        raise ValueError(f'{target}: origin differs from manifest; refusing to repoint it')
    if git(target, 'status', '--porcelain', '--untracked-files=all'):
        raise ValueError(f'{target}: local changes present; preserve them before fetching')


def fetch_one(root, name, url, revision):
    target = root / name
    check_existing(target, url)
    if (target.exists() and re.fullmatch(r'[0-9a-f]{40}', revision)
            and git(target, 'rev-parse', 'HEAD') == revision):
        return revision
    # Fetch and validate in staging. A network/ref failure cannot change HEAD
    # or the working files of an existing checkout.
    with tempfile.TemporaryDirectory(prefix=f'.fetch-{name}-', dir=root) as tmp:
        staging = Path(tmp) / 'source'
        staging.mkdir()
        git(staging, 'init', '-q')
        git(staging, 'remote', 'add', 'origin', url)
        git(staging, 'fetch', '--depth', '1', '--no-tags', 'origin', revision)
        commit = git(staging, 'rev-parse', 'FETCH_HEAD^{commit}')
        git(staging, 'checkout', '--detach', commit)
        if target.exists():
            # Recheck before mutation; a second fetch process is serialized by
            # the root lock, and ordinary user changes are still preserved.
            check_existing(target, url)
            if git(target, 'rev-parse', 'HEAD') != commit:
                git(target, 'fetch', '--no-tags', str(staging), commit)
                git(target, 'checkout', '--detach', commit)
        else:
            staging.rename(target)
    return commit


def write_lock(path, lock):
    # Avoid invalidating component build outputs when the identity is unchanged.
    text = json.dumps(lock, indent=2, sort_keys=True) + '\n'
    if path.exists() and path.read_text() == text:
        return
    with tempfile.NamedTemporaryFile(mode='w', prefix='.sources-lock-',
                                     dir=path.parent, delete=False) as stream:
        temp = Path(stream.name)
        stream.write(text)
    try:
        os.replace(temp, path)
    finally:
        temp.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--manifest', type=Path, required=True)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--component', choices=list(COMPONENTS), action='append')
    parser.add_argument('--locked', action='store_true')
    parser.add_argument('--expected-url')
    parser.add_argument('--expected-ref')
    args = parser.parse_args()
    root = args.root.resolve()
    root.mkdir(parents=True, exist_ok=True)
    manifest = read_manifest(args.manifest)
    names = args.component or list(COMPONENTS)
    with (root / '.fetch.lock').open('a') as mutex:
        fcntl.flock(mutex, fcntl.LOCK_EX)
        lock_path = root / 'sources.lock.json'
        lock = read_lock(lock_path)
        requests = []
        for name in names:
            prefix = COMPONENTS[name]
            url = manifest.get(f'{prefix}_REPO_URL')
            ref = manifest.get(f'{prefix}_REPO_REF')
            if not url or not ref:
                raise ValueError(f'missing {prefix}_REPO_URL/REF in manifest')
            if args.expected_url and args.expected_url != url:
                raise ValueError('Make URL override differs from versions.mk; edit the manifest')
            if args.expected_ref and args.expected_ref != ref:
                raise ValueError('Make ref override differs from versions.mk; edit the manifest')
            revision = ref
            if args.locked:
                entry = lock['components'].get(name, {})
                if entry.get('url') != url or entry.get('ref') != ref:
                    raise ValueError(f'{name}: missing lock or manifest differs from lock')
                revision = entry.get('commit', '')
                if not re.fullmatch(r'[0-9a-f]{40}', revision):
                    raise ValueError(f'{name}: invalid locked commit')
            check_existing(root / name, url)
            requests.append((name, url, ref, revision))
        # Only publish the new lock after every requested fetch succeeds.
        for name, url, ref, revision in requests:
            commit = fetch_one(root, name, url, revision)
            lock['components'][name] = {
                'url': url, 'ref': ref, 'commit': commit, 'directory': name,
            }
            print(f'{name}: {commit}', flush=True)
        write_lock(lock_path, lock)


if __name__ == '__main__':
    try:
        main()
    except (RuntimeError, ValueError, OSError) as error:
        print(f'fetch: {error}', file=sys.stderr)
        sys.exit(1)
