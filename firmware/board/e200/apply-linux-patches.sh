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
              linux/fpv_capture/fpv_capture.c linux/fpv_capture/fpv_spectrum.c \
              ../../../app/include/linux/fpv_capture.h ../../../app/include/linux/fpv_spectrum.h; do
    if [ ! -f "$board_dir/$source" ]; then
        echo "Missing capture driver input: $board_dir/$source" >&2
        exit 1
    fi
done
python3 "$board_dir/../../scripts/apply_patches.py" "$1" "$board_dir/patches/linux"

# The public ioctl ABI has one canonical source in the repository app/include/.
exec python3 "$board_dir/../../scripts/install_capture_driver.py" \
    "$board_dir/linux/fpv_capture" \
    "$board_dir/../../../app/include/linux/fpv_capture.h" "$1"
