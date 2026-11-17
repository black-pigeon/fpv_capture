#!/bin/bash
set -euo pipefail

VIVADO_VERSION="${XILINX_VERSION:-2022.2}"

find_xilinx_settings() {
	local product="$1"
	local extra="${2:-}"
	local candidate
	for candidate in \
		"${extra}" \
		"/opt/Xilinx/${product}/${VIVADO_VERSION}/settings64.sh" \
		"${HOME}/Documents/Vivado/${product}/${VIVADO_VERSION}/settings64.sh" \
		"${HOME}/Xilinx/${product}/${VIVADO_VERSION}/settings64.sh" \
		"/tools/Xilinx/${product}/${VIVADO_VERSION}/settings64.sh"
	do
		if [ -n "${candidate}" ] && [ -f "${candidate}" ]; then
			printf '%s\n' "${candidate}"
			return 0
		fi
	done
	return 1
}

if [ -z "${VIVADO_SETTINGS_FILE:-}" ]; then
	VIVADO_SETTINGS_FILE="$(find_xilinx_settings Vivado || true)"
fi

if [ -z "${VITIS_SETTINGS_FILE:-}" ]; then
	VITIS_SETTINGS_FILE="$(find_xilinx_settings Vitis || true)"
	if [ -z "${VITIS_SETTINGS_FILE}" ]; then
		VITIS_SETTINGS_FILE="$(find_xilinx_settings Vitis_HLS || true)"
	fi
fi

if [ -z "${VIVADO_SETTINGS_FILE}" ] || [ ! -f "${VIVADO_SETTINGS_FILE}" ]; then
	echo "missing Vivado settings file for ${VIVADO_VERSION}" >&2
	echo "set VIVADO_SETTINGS_FILE to <install>/Vivado/${VIVADO_VERSION}/settings64.sh" >&2
	exit 1
fi

source "${VIVADO_SETTINGS_FILE}"

if [ -n "${VITIS_SETTINGS_FILE}" ] && [ -f "${VITIS_SETTINGS_FILE}" ] &&
	[ "${VITIS_SETTINGS_FILE}" != "${VIVADO_SETTINGS_FILE}" ]; then
	source "${VITIS_SETTINGS_FILE}"
fi
