#!/bin/bash
set -euo pipefail
board_dir=$(cd -- "$(dirname -- "$0")" && pwd)
if [ "$#" -ne 1 ]; then
    echo 'usage: apply-linux-patches.sh <linux-source>' >&2
    exit 2
fi
# All sources required by this kernel build are public. Check them before
# modifying a kernel checkout so missing inputs fail without partial patches.
for source in linux/fpv_capture/Kconfig linux/fpv_capture/Makefile \
              linux/fpv_capture/fpv_capture.c ../../../app/include/linux/fpv_capture.h; do
    if [ ! -f "$board_dir/$source" ]; then
        echo "Missing capture driver input: $board_dir/$source" >&2
        exit 1
    fi
done
python3 "$board_dir/../../scripts/apply_patches.py" "$1" "$board_dir/patches/linux"

# The public ioctl ABI has one canonical source in the repository app/include/.
python3 - "$board_dir/linux/fpv_capture" "$board_dir/../../../app/include/linux/fpv_capture.h" "$1" <<'PYEND'
from pathlib import Path
import shutil
import sys
source, header, kernel = map(Path, sys.argv[1:])
dest = kernel / 'drivers/misc/fpv_capture'
dest.mkdir(parents=True, exist_ok=True)
for name in ('Kconfig', 'Makefile', 'fpv_capture.c'):
    shutil.copy2(source / name, dest / name)
shutil.copy2(header, dest / 'fpv_capture.h')
for path, entry, marker in [
    (kernel / 'drivers/misc/Kconfig', 'source "drivers/misc/fpv_capture/Kconfig"', 'endmenu'),
    (kernel / 'drivers/misc/Makefile', 'obj-$(CONFIG_FPV_CAPTURE) += fpv_capture/', None),
]:
    text = path.read_text()
    if entry not in text:
        if marker:
            index = text.rfind(marker)
            if index < 0:
                raise SystemExit('drivers/misc/Kconfig has no endmenu')
            text = text[:index] + entry + '\n\n' + text[index:]
        else:
            text = text.rstrip() + '\n' + entry + '\n'
        path.write_text(text)
PYEND
