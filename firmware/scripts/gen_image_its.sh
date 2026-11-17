#!/bin/sh
set -eu

if [ "$#" -lt 7 ] || [ "$#" -gt 8 ]; then
	echo "usage: $0 <out.its> <kernel> <dtb> <ramdisk> <description> <load> <entry> [arch]" >&2
	exit 1
fi

out_its="$1"
kernel="$2"
dtb="$3"
ramdisk="$4"
description="$5"
load_addr="$6"
entry_addr="$7"
fit_arch="${8:-arm64}"

cat > "${out_its}" <<EOF
/dts-v1/;

/ {
	description = "${description}";
	#address-cells = <1>;

	images {
		kernel-1 {
			description = "Linux kernel";
			data = /incbin/("${kernel}");
			type = "kernel";
			arch = "${fit_arch}";
			os = "linux";
			compression = "none";
			load = <${load_addr}>;
			entry = <${entry_addr}>;
			hash-1 {
				algo = "sha256";
			};
		};

		fdt-1 {
			description = "Flattened Device Tree";
			data = /incbin/("${dtb}");
			type = "flat_dt";
			arch = "${fit_arch}";
			compression = "none";
			hash-1 {
				algo = "sha256";
			};
		};

		ramdisk-1 {
			description = "Buildroot rootfs";
			data = /incbin/("${ramdisk}");
			type = "ramdisk";
			arch = "${fit_arch}";
			os = "linux";
			compression = "gzip";
			hash-1 {
				algo = "sha256";
			};
		};
	};

	configurations {
		default = "conf-1";
		conf-1 {
			description = "Default Linux boot";
			kernel = "kernel-1";
			fdt = "fdt-1";
			ramdisk = "ramdisk-1";
		};
	};
};
EOF
