#!/usr/bin/env python3
"""Create an isolated build source from local locked Git objects, preserving edits."""
import argparse
import fcntl
import json
from pathlib import Path
import re
import subprocess
import tempfile


def prepare(root: Path, component: str, output: Path) -> None:
    root = root.resolve()
    entry = json.loads((root / 'sources.lock.json').read_text())['components'][component]
    commit = entry['commit']
    if not re.fullmatch(r'[0-9a-f]{40}', commit):
        raise ValueError('invalid locked commit')
    source = root / component
    output = output.absolute()
    if output.is_symlink() or output.resolve().is_relative_to(source.resolve()):
        raise ValueError(f'refusing source destination: {output}')
    if source.resolve().is_relative_to(output.resolve()):
        raise ValueError(f'refusing source destination containing original checkout: {output}')
    identity = {'schema': 1, 'component': component, 'commit': commit}
    output.parent.mkdir(parents=True, exist_ok=True)
    with (output.parent / f'.{component}.prepare.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        marker = output / '.fpv-source.json'
        if output.exists():
            if not marker.is_file():
                raise ValueError(f'refusing unowned source directory: {output}')
            if json.loads(marker.read_text()) != identity:
                raise ValueError(f'source identity changed: {output}; preserve it and use a new BUILD_DIR')
            # Board patches and in-tree build results belong to this private copy.
            return
        with tempfile.TemporaryDirectory(prefix=f'.{component}-', dir=output.parent) as temporary:
            with tempfile.TemporaryFile() as errors:
                archive = subprocess.Popen(['git', '-C', str(source), 'archive', '--format=tar', commit],
                                           stdout=subprocess.PIPE, stderr=errors)
                try:
                    unpack = subprocess.run(['tar', '-xf', '-', '--no-same-owner', '-C', temporary],
                                            stdin=archive.stdout, check=False)
                finally:
                    archive.stdout.close()
                if unpack.returncode:
                    archive.terminate()
                status = archive.wait()
                if status or unpack.returncode:
                    errors.seek(0)
                    raise ValueError(f'cannot extract {component} at {commit}: ' + errors.read().decode())
            (Path(temporary) / '.fpv-source.json').write_text(json.dumps(identity, indent=2) + '\n')
            # Rename only after the complete archive and ownership marker exist.
            Path(temporary).rename(output)
    print(f'prepared {component} at {commit}: {output}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--component', choices=('linux', 'u-boot', 'buildroot'), required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    try:
        prepare(args.root, args.component, args.output)
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        parser.exit(1, f'board source: {error}\n')


if __name__ == '__main__':
    main()
