#!/bin/bash
set -euo pipefail
board_dir=$(cd -- "$(dirname -- "$0")" && pwd)
if [ "$#" -ne 1 ]; then
    echo 'usage: apply-buildroot-patches.sh <buildroot-source>' >&2
    exit 2
fi
python3 "$board_dir/../../scripts/apply_patches.py" "$1" "$board_dir/patches/buildroot"
