#!/bin/bash
set -euo pipefail
board_dir=$(cd -- "$(dirname -- "$0")" && pwd)
if [ "$#" -ne 1 ]; then
    echo 'usage: apply-linux-patches.sh <linux-source>' >&2
    exit 2
fi
# Public exports omit custom kernel source. Check private inputs before any
# kernel mutation, including applying otherwise public board patches.
for source in linux/axi_dma/Kconfig linux/axi_dma/Makefile \
              linux/axi_dma/my_axi_dma.c linux/axi_dma/my_axi_dma_driver.h \
              linux/fpv_capture/Kconfig linux/fpv_capture/Makefile \
              linux/fpv_capture/fpv_capture.c; do
    if [ ! -f "$board_dir/$source" ]; then
        echo "Missing private kernel input: $board_dir/$source" >&2
        echo 'Restore private driver sources; see docs/public-repository.md.' >&2
        exit 1
    fi
done
python3 "$board_dir/../../scripts/apply_patches.py" "$1" "$board_dir/patches/linux"
# Keep the custom driver source in this board, rather than referencing DFONE.
python3 - "$board_dir/linux/axi_dma" "$1" <<'PY'
from pathlib import Path
import shutil
import sys
source, kernel = map(Path, sys.argv[1:])
destination = kernel / 'drivers/misc/my_axi_dma'
destination.mkdir(parents=True, exist_ok=True)
for name in ['Kconfig', 'Makefile', 'my_axi_dma.c', 'my_axi_dma_driver.h']:
    shutil.copy2(source / name, destination / name)
kconfig = kernel / 'drivers/misc/Kconfig'
text = kconfig.read_text()
entry = 'source "drivers/misc/my_axi_dma/Kconfig"'
if entry not in text:
    index = text.rfind('endmenu')
    if index < 0:
        raise SystemExit('drivers/misc/Kconfig has no endmenu')
    kconfig.write_text(text[:index] + entry + '\n\n' + text[index:])
makefile = kernel / 'drivers/misc/Makefile'
text = makefile.read_text()
entry = 'obj-$(CONFIG_MY_AXI_DMA)\t+= my_axi_dma/'
if 'CONFIG_MY_AXI_DMA' not in text:
    makefile.write_text(text.rstrip() + '\n' + entry + '\n')
PY

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
