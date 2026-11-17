#!/bin/bash
set -euo pipefail

host_bin_dir="$1"
toolchain_dir="$2"

if [ ! -d "$host_bin_dir" ]; then
	echo "missing host bin dir: $host_bin_dir" >&2
	exit 1
fi

if [ -d "${toolchain_dir}/x86_64-petalinux-linux/usr/bin/aarch64-xilinx-linux" ]; then
	actual_bin_dir="${toolchain_dir}/x86_64-petalinux-linux/usr/bin/aarch64-xilinx-linux"
	actual_prefix="aarch64-xilinx-linux"
	host_prefix="aarch64-linux-gnu"
elif [ -d "${toolchain_dir}/x86_64-petalinux-linux/usr/bin/arm-xilinx-linux-gnueabi" ]; then
	actual_bin_dir="${toolchain_dir}/x86_64-petalinux-linux/usr/bin/arm-xilinx-linux-gnueabi"
	actual_prefix="arm-xilinx-linux-gnueabi"
	host_prefix="arm-linux-gnueabihf"
else
	echo "missing Vitis binutils dir under: $toolchain_dir" >&2
	exit 1
fi

for actual in "$actual_bin_dir"/"${actual_prefix}"-*; do
	[ -e "$actual" ] || continue

	name="${actual##*/}"
	suffix="${name#${actual_prefix}-}"
	case "$suffix" in
		gcc|g++|cpp)
			continue
			;;
	esac

	ln -snf "$actual" "${host_bin_dir}/${host_prefix}-${suffix}"
done
