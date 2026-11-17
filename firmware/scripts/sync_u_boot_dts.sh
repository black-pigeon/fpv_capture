#!/bin/bash
set -euo pipefail

src_dts="$1"
dst_dts="$2"
dts_makefile="$3"
dts_name="$4"

if [ ! -f "$src_dts" ]; then
	echo "missing U-Boot DTS source: $src_dts" >&2
	exit 1
fi

mkdir -p "$(dirname "$dst_dts")"
cp "$src_dts" "$dst_dts"

if ! grep -q -F "${dts_name}.dtb" "$dts_makefile"; then
	python3 - "$dts_makefile" "$dts_name" "$src_dts" <<'PY'
import pathlib
import sys

makefile = pathlib.Path(sys.argv[1])
dts_name = sys.argv[2]
src_dts = pathlib.Path(sys.argv[3])
needle = "targets += $(dtb-y)\n"
src_text = src_dts.read_text()
arch_config = "CONFIG_ARCH_ZYNQMP"
if "zynq-7000.dtsi" in src_text:
    arch_config = "CONFIG_ARCH_ZYNQ"
entry = f"dtb-$({arch_config}) += {dts_name}.dtb\n"

data = makefile.read_text()
if needle in data:
    data = data.replace(needle, entry + "\n" + needle, 1)
else:
    data += "\n" + entry
makefile.write_text(data)
PY
fi
