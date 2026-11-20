#!/usr/bin/env python3
"""Install the canonical capture driver and public UAPI into a board's kernel."""
from pathlib import Path
import shutil
import sys


def install(source, header, kernel):
    files = [(source / name, name) for name in ('Kconfig', 'Makefile', 'fpv_capture.c', 'fpv_spectrum.c', 'COPYING')]
    files.append((header, 'fpv_capture.h'))
    files.append((header.with_name('fpv_spectrum.h'), 'fpv_spectrum.h'))
    for path, _ in files:
        if not path.is_file():
            raise ValueError(f'missing capture driver input: {path}')
    entries = [
        (kernel / 'drivers/misc/Kconfig', 'source "drivers/misc/fpv_capture/Kconfig"', 'endmenu'),
        (kernel / 'drivers/misc/Makefile', 'obj-$(CONFIG_FPV_CAPTURE) += fpv_capture/', None),
    ]
    updates = []
    for path, entry, marker in entries:
        text = path.read_text()
        if entry not in text:
            if marker:
                index = text.rfind(marker)
                if index < 0:
                    raise ValueError(f'{path} has no {marker}')
                text = text[:index] + entry + '\n\n' + text[index:]
            else:
                text = text.rstrip() + '\n' + entry + '\n'
        updates.append((path, text))
    dest = kernel / 'drivers/misc/fpv_capture'
    dest.mkdir(parents=True, exist_ok=True)
    for path, name in files:
        shutil.copy2(path, dest / name)
    for path, text in updates:
        path.write_text(text)


if __name__ == '__main__':
    try:
        if len(sys.argv) != 4:
            raise ValueError('usage: install_capture_driver.py <driver-source> <uapi-header> <kernel>')
        install(*map(Path, sys.argv[1:]))
    except (ValueError, OSError) as error:
        print(error, file=sys.stderr)
        sys.exit(1)
