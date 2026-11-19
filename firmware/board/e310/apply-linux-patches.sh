#!/bin/bash
set -euo pipefail
board_dir=$(cd -- "$(dirname -- "$0")" && pwd)
if [ "$#" -ne 1 ]; then
    echo 'usage: apply-linux-patches.sh <linux-source>' >&2
    exit 2
fi
# Share only the capture driver, not E200 Ethernet or other board patches.
exec python3 "$board_dir/../../scripts/install_capture_driver.py" \
    "$board_dir/../e200/linux/fpv_capture" \
    "$board_dir/../../../app/include/linux/fpv_capture.h" "$1"
