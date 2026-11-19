#!/bin/bash
set -euo pipefail
board_dir=$(cd -- "$(dirname -- "$0")" && pwd)
# The Dropbear/libxcrypt compatibility patch is independent of board hardware.
exec bash "$board_dir/../e200/apply-buildroot-patches.sh" "$@"
