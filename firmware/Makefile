.DEFAULT_GOAL := sd
SHELL := /bin/bash

BOARD ?= e200
BOARD_MK_FILES := $(wildcard $(CURDIR)/board/*/board.mk)
AVAILABLE_BOARDS := $(sort $(patsubst $(CURDIR)/board/%/board.mk,%,$(BOARD_MK_FILES)))
BOARD_DIR := $(CURDIR)/board/$(BOARD)

ifeq ($(filter $(BOARD),$(AVAILABLE_BOARDS)),)
$(error Unknown BOARD '$(BOARD)'. Available boards: $(AVAILABLE_BOARDS))
endif

include versions.mk
include board/common.mk
include board/$(BOARD)/board.mk

SOURCE_IDENTITY_INPUTS := versions.mk sources.lock.json

ifeq ($(filter 0 1,$(CONSOLE_UART)),)
$(error Unsupported CONSOLE_UART '$(CONSOLE_UART)'. Expected 0 or 1)
endif

NPROC ?= $(shell nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 8)
UBOOT_JOBS ?= 1
BUILDROOT_JOBS ?= $(NPROC)

BUILD_ROOT ?= $(CURDIR)/build
BUILD_DIR ?= $(BUILD_ROOT)/$(BOARD)
SOURCE_STAMP_DIR ?= $(CURDIR)/.source-stamps
LINUX_DIR ?= $(CURDIR)/linux
UBOOT_DIR ?= $(CURDIR)/u-boot
BUILDROOT_DIR ?= $(CURDIR)/buildroot
ifeq ($(ISOLATE_BOARD_SOURCES),y)
LINUX_DIR := $(BUILD_DIR)/sources/linux
UBOOT_DIR := $(BUILD_DIR)/sources/u-boot
BUILDROOT_DIR := $(BUILD_DIR)/sources/buildroot
PRIVATE_SOURCE_STAMPS := $(addsuffix /.fpv-source.json,$(LINUX_DIR) $(UBOOT_DIR) $(BUILDROOT_DIR))
endif
# Buildroot clears overrides itself after its umask restart; keep explicitly
# passed O/BR2_* values until then (SUBMAKE_ENV strips the parent make flags).
BUILDROOT_EXTERNAL := $(CURDIR)/buildroot-external:$(CURDIR)/package
BUILDROOT_OUTPUT_ROOT ?= $(BUILD_ROOT)/buildroot
BUILDROOT_O ?= $(BUILDROOT_OUTPUT_ROOT)/$(BOARD)
BUILDROOT_HOST_DIR := $(BUILDROOT_O)/host
CROSS_COMPILE ?= $(BUILDROOT_CROSS)
ATF_CROSS_COMPILE ?= $(CROSS_COMPILE)
VIVADO_SETTINGS ?= $(CURDIR)/scripts/xilinx-settings.sh
# XSCT/Vitis is not safe to run concurrently (e.g. BSP and device-tree
# generation under a parallel top-level make), so serialize Xilinx tool calls
# per board while leaving ordinary compiler jobs parallel.
XILINX_TOOL_LOCK := $(BUILD_DIR)/.xilinx-tool.lock
BUILDROOT_LOCK := $(BUILDROOT_O)/.buildroot.lock
UBOOT_LOCK := $(BUILD_DIR)/.u-boot.lock
BUILDROOT_TOOLCHAIN_GCC := $(BUILDROOT_HOST_DIR)/bin/$(CROSS_COMPILE)gcc
BUILDROOT_TOOLCHAIN_STAMP := $(BUILDROOT_O)/.toolchain.stamp
BUILDROOT_TOOLCHAIN_FIX_STAMP := $(BUILDROOT_O)/.toolchain-fix.stamp
BUILDROOT_DROPBEAR_REFRESH_STAMP := $(BUILDROOT_O)/.dropbear-refresh-uart$(CONSOLE_UART).stamp
BUILDROOT_OUTPUT_PATH_STAMP := $(BUILDROOT_O)/.output-path.stamp
TOOLCHAIN_GCC ?= $(BUILDROOT_TOOLCHAIN_GCC)
HOST_PATH = PATH="$(BUILDROOT_HOST_DIR)/bin:$(PATH)"
SAFE_ENV = env -u LD_LIBRARY_PATH
SUBMAKE_ENV = env -u BOARD -u MAKEFLAGS -u MFLAGS
STAMP_DIR := $(BUILD_DIR)/.stamps
UBOOT_BUILD_FLAGS := $(if $(UBOOT_DTS_NAME),DEVICE_TREE=$(UBOOT_DTS_NAME))
XILINX_VERSION ?= 2022.2
VITIS_AARCH64_LINUX_DIR ?= $(if $(filter /%,$(CROSS_COMPILE)),$(patsubst %/bin/aarch64-linux-gnu-,%,$(CROSS_COMPILE)),/opt/Xilinx/Vitis/$(XILINX_VERSION)/gnu/aarch64/lin/aarch64-linux)

FETCH_SCRIPT := $(CURDIR)/scripts/fetch_repo.sh
DTC_PATCH_SCRIPT := $(CURDIR)/scripts/patch_legacy_dtc.sh
UBOOT_DTS_SYNC_SCRIPT := $(CURDIR)/scripts/sync_u_boot_dts.sh
CONSOLE_RENDER_SCRIPT := $(CURDIR)/scripts/render_ps_uart_config.py
BUILDROOT_RELOCATION_CHECK_SCRIPT := $(CURDIR)/scripts/check_buildroot_output_relocation.sh
SOURCE_BOARD_GUARD_SCRIPT := $(CURDIR)/scripts/check_shared_source_board.sh
ACTIVE_BOARD_FILE := $(SOURCE_STAMP_DIR)/active-board
ifeq ($(ISOLATE_BOARD_SOURCES),y)
ACTIVE_BOARD_FILE := $(BUILD_DIR)/.source-stamps/active-board
# E310 must never bypass its private-source guard via shared-source reuse.
override REUSE_SHARED_SOURCES := 0
endif
SOURCE_PATCH_BOARD ?= $(shell if [ -f "$(ACTIVE_BOARD_FILE)" ]; then cat "$(ACTIVE_BOARD_FILE)"; else printf '%s' "$(BOARD)"; fi)
REUSE_SHARED_SOURCES ?= 0

HARDWARE_FILE := $(BUILD_DIR)/$(notdir $(HW_FILE))
BITSTREAM_FILE := $(BUILD_DIR)/$(notdir $(HW_BIT))

FSBL_ELF := $(BUILD_DIR)/sdk/fsbl/Release/fsbl.elf
PMUFW_ELF := $(BUILD_DIR)/sdk/pmufw/Release/pmufw.elf
BL31_ELF := $(BUILD_DIR)/bl31.elf
BL31_BIN := $(BUILD_DIR)/bl31.bin
UBOOT_ELF := $(BUILD_DIR)/u-boot.elf

ROOTFS_CPIO := $(BUILDROOT_O)/images/rootfs.cpio.gz
ROOTFS_EXT4 := $(BUILDROOT_O)/images/rootfs.ext4
KERNEL_IMAGE := $(LINUX_DIR)/$(LINUX_IMAGE_PATH)
DT_DIR := $(BUILD_DIR)/devicetree
DT_SOURCE := $(DT_DIR)/system-top.dts
DTB_FILE := $(BUILD_DIR)/system-top.dtb
URAMDISK_FILE := $(BUILD_DIR)/$(SD_RAMDISK_NAME)

FIT_ITS := $(BUILD_DIR)/image.its
IMAGE_UB := $(BUILD_DIR)/image.ub
BOOT_BIF := $(BUILD_DIR)/boot.bif
BOOT_BIN := $(BUILD_DIR)/BOOT.BIN
BOOT_PROFILE_STAMP := $(BUILD_DIR)/.boot-profile

SD_DIR := $(BUILD_DIR)/sd
SD_BUILD_INFO := $(SD_DIR)/$(BOARD)-firmware.manifest
EMMC_DIR := $(BUILD_DIR)/emmc
QSPI_DIR := $(BUILD_DIR)/qspi
QSPI_IMAGE := $(QSPI_DIR)/flash_$(BOARD).bin
QSPI_LAYOUT := $(QSPI_DIR)/layout.txt
EMMC_BOOT_BIN_FILE := $(EMMC_DIR)/$(SD_BOOT_IMAGE_NAME)
QSPI_BOOT_BIN_FILE := $(QSPI_DIR)/BOOT.BIN
QSPI_IMAGE_UB_FILE := $(QSPI_DIR)/image.ub
FIRMWARE_DIR := $(BUILD_DIR)/firmware
EMMC_FRM := $(FIRMWARE_DIR)/$(BOARD)-emmc.frm
QSPI_FRM := $(FIRMWARE_DIR)/$(BOARD)-qspi.frm
SD_FRM := $(FIRMWARE_DIR)/$(BOARD)-sd.frm
SD_BITSTREAM_FILE = $(if $(strip $(SD_BITSTREAM_NAME)),$(SD_DIR)/$(SD_BITSTREAM_NAME))
CONSOLE_DIR := $(BUILD_DIR)/console-uart$(CONSOLE_UART)
CONSOLE_STAMP := $(STAMP_DIR)/console-uart$(CONSOLE_UART)
RENDERED_SYSTEM_USER_DTSI := $(CONSOLE_DIR)/system-user.dtsi
RENDERED_UENV_FILE := $(CONSOLE_DIR)/uEnv.txt
EMMC_UENV_FILE ?= $(BOARD_BOOT_DIR)/uEnv-emmc-ext4.txt
RENDERED_EMMC_UENV_FILE := $(CONSOLE_DIR)/uEnv-emmc-ext4.txt
RENDERED_UBOOT_CONFIG_FRAGMENT := $(CONSOLE_DIR)/fragment.config
RENDERED_BUILDROOT_POST_BUILD_SCRIPT := $(CONSOLE_DIR)/post-build.sh
RENDERED_BUILDROOT_DEFCONFIG_FILE := $(CONSOLE_DIR)/defconfig
RENDERED_UBOOT_DTS_SOURCE := $(if $(and $(UBOOT_DTS_NAME),$(UBOOT_DTS_SOURCE)),$(CONSOLE_DIR)/$(UBOOT_DTS_NAME).dts)
ACTIVE_UBOOT_DTS_SOURCE := $(if $(RENDERED_UBOOT_DTS_SOURCE),$(RENDERED_UBOOT_DTS_SOURCE),$(UBOOT_DTS_SOURCE))
LINUX_PATCH_FILES := $(sort $(wildcard $(BOARD_DIR)/patches/linux/*.patch))
BUILDROOT_PATCH_FILES := $(sort $(wildcard $(BOARD_DIR)/patches/buildroot/*.patch $(SHARED_BUILDROOT_PATCH_DIR)/*.patch))
ROOTFS_OVERLAY_FILES := $(shell if [ -d "$(BOARD_ROOTFS_OVERLAY_DIR)" ]; then find "$(BOARD_ROOTFS_OVERLAY_DIR)" \( -type f -o -type l \) -print | sort; fi)
ROOTFS_PACKAGE_FILES := $(shell find "$(CURDIR)/buildroot-external/package" -type f -o -type l | sort; if [ -d "$(BOARD_DIR)" ]; then find "$(BOARD_DIR)" \( -path '*/.git' -o -path '*/.cache' -o -path '*/build' -o -path '*/build-*' -o -path '*/patches/autosave' \) -prune -o \( -type f -o -type l \) -print | sort; fi)
# Re-extract the local package when application sources change.
BUILDROOT_BOARD_FILES := $(wildcard $(CURDIR)/package/*)
ROOTFS_PACKAGE_FILES += $(BUILDROOT_BOARD_FILES)
ROOTFS_PACKAGE_FILES += $(shell find "$(CURDIR)/../app" -type f -print | sort)
BUILDROOT_LOCAL_PACKAGE_STAMPS := $(foreach pkg,$(BUILDROOT_LOCAL_PACKAGES),$(BUILDROOT_O)/.local-package-$(pkg).stamp)
BOARD_LINUX_FILES := $(shell if [ -d "$(BOARD_LINUX_DIR)" ]; then find "$(BOARD_LINUX_DIR)" \( -type f -o -type l \) ! -name '*.o' ! -name '*.cmd' ! -name '.*.cmd' ! -name 'built-in.a' ! -name 'modules.*' | sort; fi)
# E310 installs the same canonical driver and UAPI as E200.
BOARD_LINUX_FILES += $(wildcard $(CAPTURE_DRIVER_DIR)/*) $(CAPTURE_INSTALL_SCRIPT)
U_BOOT_STAMP := $(STAMP_DIR)/u-boot-uart$(CONSOLE_UART)
U_BOOT_CONFIG_STAMP := $(STAMP_DIR)/u-boot-config-uart$(CONSOLE_UART)
BSP_STAMP := $(STAMP_DIR)/sdk-uart$(CONSOLE_UART)
DTC_COMPAT_STAMP := $(STAMP_DIR)/dtc-compat
UBOOT_DTS_STAMP := $(STAMP_DIR)/u-boot-dts-uart$(CONSOLE_UART)
BUILDROOT_DEFCONFIG_STAMP := $(BUILDROOT_O)/.defconfig-uart$(CONSOLE_UART).stamp
LINUX_CONFIG_STAMP := $(STAMP_DIR)/linux-config
BUILDROOT_FETCH_STAMP := $(SOURCE_STAMP_DIR)/buildroot-$(BUILDROOT_REPO_REF).stamp
LINUX_FETCH_STAMP := $(SOURCE_STAMP_DIR)/linux-$(LINUX_REPO_REF).stamp
UBOOT_FETCH_STAMP := $(SOURCE_STAMP_DIR)/u-boot-$(UBOOT_REPO_REF).stamp
ATF_FETCH_STAMP := $(SOURCE_STAMP_DIR)/arm-trusted-firmware-$(ATF_REPO_REF).stamp
EMBEDDEDSW_FETCH_STAMP := $(SOURCE_STAMP_DIR)/embeddedsw-$(EMBEDDEDSW_REPO_REF).stamp
DEVICE_TREE_FETCH_STAMP := $(SOURCE_STAMP_DIR)/device-tree-xlnx-$(DEVICE_TREE_REPO_REF).stamp

.PHONY: validate-source-lock FORCE all help list-boards show-config hardware check-source-board fetch fetch-locked repos toolchain rootfs rootfs-ext4 ramdisk kernel u-boot fsbl dt imageub bootbin sd clean

FORCE:

all: sd

help:
	@echo 'FPV firmware foundation ($(BOARD))'
	@echo '  fetch / fetch-locked  - fetch fixed refs / exact locked commits'
	@echo '  show-config          - print board, hardware and output paths'
	@echo '  toolchain / rootfs   - build ARM toolchain / root filesystem'
	@echo '  kernel / u-boot      - build Linux / U-Boot'
	@echo '  fsbl / dt / bootbin  - build from matching $(BOARD) FPV XSA'
	@echo '  sd                   - assemble SD boot files (default)'
	@echo '  clean                - remove build output only'
	@echo '  hardware             - validate XSA and extract its bitstream'
	@echo 'Hardware input: $(HW_FILE)'
	@echo 'Hardware manifest: $(HW_MANIFEST)'

list-boards:
	@printf '%s\n' $(AVAILABLE_BOARDS)

show-config:
	@echo "BOARD=$(BOARD)"
	@echo "BOARD_NAME=$(BOARD_NAME)"
	@echo "BOARD_DESC=$(BOARD_DESC)"
	@echo "SOC_FAMILY=$(SOC_FAMILY)"
	@echo "HW_FILE=$(HW_FILE)"
	@echo "HW_BIT=$(HW_BIT)"
	@echo "HW_MANIFEST=$(HW_MANIFEST)"
	@echo "CONSOLE_UART=$(CONSOLE_UART)"
	@echo "CONSOLE_TTY=$(CONSOLE_TTY)"
	@echo "CONSOLE_UART_BASE=$(CONSOLE_UART_BASE)"
	@echo "FSBL_STDIO=$(FSBL_STDIO)"
	@echo "PMUFW_STDIO=$(PMUFW_STDIO)"
	@echo "BUILD_DIR=$(BUILD_DIR)"
	@echo "BUILDROOT_O=$(BUILDROOT_O)"
	@echo "LINUX_DIR=$(LINUX_DIR)"
	@echo "UBOOT_DIR=$(UBOOT_DIR)"
	@echo "BUILDROOT_DIR=$(BUILDROOT_DIR)"
	@echo "BUILDROOT_JOBS=$(BUILDROOT_JOBS)"
	@echo "BUILDROOT_DEFCONFIG_FILE=$(BUILDROOT_DEFCONFIG_FILE)"
	@echo "LINUX_DEFCONFIG=$(LINUX_DEFCONFIG)"
	@echo "UBOOT_DEFCONFIG=$(UBOOT_DEFCONFIG)"
	@echo "BOOTGEN_ARCH=$(BOOTGEN_ARCH)"

check-source-board: $(PRIVATE_SOURCE_STAMPS) | $(SOURCE_STAMP_DIR)
	bash "$(SOURCE_BOARD_GUARD_SCRIPT)" "$(ACTIVE_BOARD_FILE)" "$(BOARD)" "$(REUSE_SHARED_SOURCES)" \
		"$(LINUX_DIR)" \
		"$(UBOOT_DIR)" \
		"$(BUILDROOT_DIR)"

fetch:
	python3 scripts/fetch_sources.py --manifest versions.mk --root .
	@mkdir -p "$(SOURCE_STAMP_DIR)"
	@touch $(BUILDROOT_FETCH_STAMP) $(LINUX_FETCH_STAMP) $(UBOOT_FETCH_STAMP) $(EMBEDDEDSW_FETCH_STAMP) $(DEVICE_TREE_FETCH_STAMP)

fetch-locked:
	python3 scripts/fetch_sources.py --manifest versions.mk --root . --locked
	@mkdir -p "$(SOURCE_STAMP_DIR)"
	@touch $(BUILDROOT_FETCH_STAMP) $(LINUX_FETCH_STAMP) $(UBOOT_FETCH_STAMP) $(EMBEDDEDSW_FETCH_STAMP) $(DEVICE_TREE_FETCH_STAMP)

# Order-only validation runs even when all cached fetch stamps are current.
# Board patch edits are allowed here; fetch itself retains clean-tree checks.
validate-source-lock:
	python3 scripts/validate_sources.py --root . --manifest versions.mk

repos: \
	$(BUILDROOT_FETCH_STAMP) \
	$(LINUX_FETCH_STAMP) \
	$(UBOOT_FETCH_STAMP) \
	$(EMBEDDEDSW_FETCH_STAMP) \
	$(DEVICE_TREE_FETCH_STAMP)

$(BUILDROOT_FETCH_STAMP): | validate-source-lock $(SOURCE_STAMP_DIR)
	$(FETCH_SCRIPT) "$(BUILDROOT_REPO_URL)" "$(BUILDROOT_REPO_REF)" "$(CURDIR)/buildroot"
	touch $@

$(LINUX_FETCH_STAMP): | validate-source-lock $(SOURCE_STAMP_DIR)
	$(FETCH_SCRIPT) "$(LINUX_REPO_URL)" "$(LINUX_REPO_REF)" "$(CURDIR)/linux"
	touch $@

$(UBOOT_FETCH_STAMP): | validate-source-lock $(SOURCE_STAMP_DIR)
	$(FETCH_SCRIPT) "$(UBOOT_REPO_URL)" "$(UBOOT_REPO_REF)" "$(CURDIR)/u-boot"
	touch $@

$(EMBEDDEDSW_FETCH_STAMP): | validate-source-lock $(SOURCE_STAMP_DIR)
	$(FETCH_SCRIPT) "$(EMBEDDEDSW_REPO_URL)" "$(EMBEDDEDSW_REPO_REF)" "$(CURDIR)/embeddedsw"
	touch $@

$(DEVICE_TREE_FETCH_STAMP): | validate-source-lock $(SOURCE_STAMP_DIR)
	$(FETCH_SCRIPT) "$(DEVICE_TREE_REPO_URL)" "$(DEVICE_TREE_REPO_REF)" "$(CURDIR)/device-tree-xlnx"
	touch $@

ifeq ($(ISOLATE_BOARD_SOURCES),y)
$(LINUX_DIR)/.fpv-source.json: $(LINUX_FETCH_STAMP) $(SOURCE_IDENTITY_INPUTS) scripts/prepare_board_source.py | validate-source-lock
	python3 scripts/prepare_board_source.py --root "$(CURDIR)" --component linux --output "$(LINUX_DIR)"
	touch $@

$(UBOOT_DIR)/.fpv-source.json: $(UBOOT_FETCH_STAMP) $(SOURCE_IDENTITY_INPUTS) scripts/prepare_board_source.py | validate-source-lock
	python3 scripts/prepare_board_source.py --root "$(CURDIR)" --component u-boot --output "$(UBOOT_DIR)"
	touch $@

$(BUILDROOT_DIR)/.fpv-source.json: $(BUILDROOT_FETCH_STAMP) $(SOURCE_IDENTITY_INPUTS) scripts/prepare_board_source.py | validate-source-lock
	python3 scripts/prepare_board_source.py --root "$(CURDIR)" --component buildroot --output "$(BUILDROOT_DIR)"
	touch $@
endif

$(DTC_COMPAT_STAMP): $(UBOOT_FETCH_STAMP) $(LINUX_FETCH_STAMP) $(DTC_PATCH_SCRIPT) | check-source-board $(STAMP_DIR)
	bash "$(DTC_PATCH_SCRIPT)" "$(UBOOT_DIR)/scripts/dtc/dtc-lexer.l"
	bash "$(DTC_PATCH_SCRIPT)" "$(LINUX_DIR)/scripts/dtc/dtc-lexer.l"
	touch $@

$(CONSOLE_STAMP): | $(STAMP_DIR)
	printf '%s\n' \
		'CONSOLE_UART=$(CONSOLE_UART)' \
		'CONSOLE_TTY=$(CONSOLE_TTY)' \
		'CONSOLE_UART_BASE=$(CONSOLE_UART_BASE)' \
		'FSBL_STDIO=$(FSBL_STDIO)' \
		'PMUFW_STDIO=$(PMUFW_STDIO)' \
		'FSBL_DEBUG_LEVEL=$(FSBL_DEBUG_LEVEL)' \
		'PMUFW_ENABLE_EFUSE_ACCESS=$(PMUFW_ENABLE_EFUSE_ACCESS)' \
		'ATF_MAKE_FLAGS=$(ATF_MAKE_FLAGS)' > $@

$(CONSOLE_DIR):
	mkdir -p $@

$(RENDERED_SYSTEM_USER_DTSI): $(SYSTEM_USER_DTSI) $(CONSOLE_RENDER_SCRIPT) | $(CONSOLE_DIR)
	python3 "$(CONSOLE_RENDER_SCRIPT)" linux-dtsi --source "$<" --output "$@" --console-uart "$(CONSOLE_UART)" --console-tty "$(CONSOLE_TTY)" --console-uart-base "$(CONSOLE_UART_BASE)" $(CONSOLE_RENDER_FLAGS)

$(RENDERED_UENV_FILE): $(UENV_FILE) $(CONSOLE_RENDER_SCRIPT) | $(CONSOLE_DIR)
	python3 "$(CONSOLE_RENDER_SCRIPT)" uenv --source "$<" --output "$@" --console-uart "$(CONSOLE_UART)" --console-tty "$(CONSOLE_TTY)" --console-uart-base "$(CONSOLE_UART_BASE)" $(CONSOLE_RENDER_FLAGS)

$(RENDERED_EMMC_UENV_FILE): $(EMMC_UENV_FILE) $(CONSOLE_RENDER_SCRIPT) | $(CONSOLE_DIR)
	python3 "$(CONSOLE_RENDER_SCRIPT)" uenv --source "$<" --output "$@" --console-uart "$(CONSOLE_UART)" --console-tty "$(CONSOLE_TTY)" --console-uart-base "$(CONSOLE_UART_BASE)" $(CONSOLE_RENDER_FLAGS)

$(RENDERED_UBOOT_CONFIG_FRAGMENT): $(UBOOT_CONFIG_FRAGMENT) $(CONSOLE_RENDER_SCRIPT) | $(CONSOLE_DIR)
	python3 "$(CONSOLE_RENDER_SCRIPT)" uboot-fragment --source "$<" --output "$@" --console-uart "$(CONSOLE_UART)" --console-tty "$(CONSOLE_TTY)" --console-uart-base "$(CONSOLE_UART_BASE)" $(CONSOLE_RENDER_FLAGS)

$(RENDERED_BUILDROOT_POST_BUILD_SCRIPT): $(BOARD_BUILDROOT_DIR)/post-build.sh $(CONSOLE_RENDER_SCRIPT) | $(CONSOLE_DIR)
	python3 "$(CONSOLE_RENDER_SCRIPT)" post-build --source "$<" --output "$@" --console-uart "$(CONSOLE_UART)" --console-tty "$(CONSOLE_TTY)" --console-uart-base "$(CONSOLE_UART_BASE)" $(CONSOLE_RENDER_FLAGS)
	chmod +x "$@"

$(RENDERED_BUILDROOT_DEFCONFIG_FILE): $(BUILDROOT_DEFCONFIG_FILE) $(RENDERED_BUILDROOT_POST_BUILD_SCRIPT) $(CONSOLE_RENDER_SCRIPT) | $(CONSOLE_DIR)
	python3 "$(CONSOLE_RENDER_SCRIPT)" buildroot-defconfig \
		--source "$<" \
		--output "$@" \
		--console-uart "$(CONSOLE_UART)" \
		--console-tty "$(CONSOLE_TTY)" \
		--console-uart-base "$(CONSOLE_UART_BASE)" $(CONSOLE_RENDER_FLAGS) \
		--post-build-script "$(abspath $(RENDERED_BUILDROOT_POST_BUILD_SCRIPT))"

ifneq ($(strip $(RENDERED_UBOOT_DTS_SOURCE)),)
$(RENDERED_UBOOT_DTS_SOURCE): $(UBOOT_DTS_SOURCE) $(CONSOLE_RENDER_SCRIPT) | $(CONSOLE_DIR)
	python3 "$(CONSOLE_RENDER_SCRIPT)" uboot-dts --source "$<" --output "$@" --console-uart "$(CONSOLE_UART)" --console-tty "$(CONSOLE_TTY)" --console-uart-base "$(CONSOLE_UART_BASE)" $(CONSOLE_RENDER_FLAGS)
endif

$(UBOOT_DTS_STAMP): $(UBOOT_FETCH_STAMP) $(ACTIVE_UBOOT_DTS_SOURCE) $(UBOOT_DTS_SYNC_SCRIPT) | check-source-board $(STAMP_DIR)
	if [ -n "$(UBOOT_DTS_NAME)" ] && [ -n "$(UBOOT_DTS_SOURCE)" ]; then \
		bash "$(UBOOT_DTS_SYNC_SCRIPT)" \
			"$(ACTIVE_UBOOT_DTS_SOURCE)" \
			"$(UBOOT_DIR)/arch/arm/dts/$(UBOOT_DTS_NAME).dts" \
			"$(UBOOT_DIR)/arch/arm/dts/Makefile" \
			"$(UBOOT_DTS_NAME)"; \
	fi
	touch $@

toolchain: $(BUILDROOT_TOOLCHAIN_FIX_STAMP)

$(BUILDROOT_OUTPUT_PATH_STAMP): $(BUILDROOT_RELOCATION_CHECK_SCRIPT)
	bash "$(BUILDROOT_RELOCATION_CHECK_SCRIPT)" "$(BUILDROOT_O)" "$@"

$(BUILDROOT_DEFCONFIG_STAMP): $(SOURCE_IDENTITY_INPUTS) $(BUILDROOT_FETCH_STAMP) $(RENDERED_BUILDROOT_DEFCONFIG_FILE) $(BUILDROOT_BOARD_FILES) $(BOARD_DIR)/apply-buildroot-patches.sh $(BUILDROOT_PATCH_FILES) | check-source-board $(BUILDROOT_OUTPUT_PATH_STAMP)
	if [ -f "$(BOARD_DIR)/apply-buildroot-patches.sh" ]; then \
		bash "$(BOARD_DIR)/apply-buildroot-patches.sh" "$(BUILDROOT_DIR)"; \
	fi
	flock "$(BUILDROOT_LOCK)" $(SAFE_ENV) $(SUBMAKE_ENV) $(MAKE) -C $(BUILDROOT_DIR) O=$(BUILDROOT_O) BR2_JLEVEL=$(BUILDROOT_JOBS) BR2_EXTERNAL=$(BUILDROOT_EXTERNAL) BR2_DEFCONFIG=$(RENDERED_BUILDROOT_DEFCONFIG_FILE) defconfig
	touch $@

$(BUILDROOT_TOOLCHAIN_STAMP): $(BUILDROOT_DEFCONFIG_STAMP) | $(BUILDROOT_OUTPUT_PATH_STAMP)
	flock "$(BUILDROOT_LOCK)" $(SAFE_ENV) $(SUBMAKE_ENV) $(MAKE) -C $(BUILDROOT_DIR) O=$(BUILDROOT_O) BR2_JLEVEL=$(BUILDROOT_JOBS) toolchain
	touch $@

$(BUILDROOT_TOOLCHAIN_FIX_STAMP): $(BUILDROOT_TOOLCHAIN_STAMP) scripts/fix_vitis_toolchain_wrappers.sh | $(BUILDROOT_OUTPUT_PATH_STAMP)
	bash scripts/fix_vitis_toolchain_wrappers.sh "$(BUILDROOT_HOST_DIR)/bin" "$(VITIS_AARCH64_LINUX_DIR)"
	touch $@

$(BUILDROOT_DROPBEAR_REFRESH_STAMP): $(BUILDROOT_DEFCONFIG_STAMP) $(BUILDROOT_TOOLCHAIN_FIX_STAMP) | $(BUILDROOT_OUTPUT_PATH_STAMP)
	@if grep -q '^BR2_PACKAGE_DROPBEAR=y' "$(BUILDROOT_O)/.config"; then \
		flock "$(BUILDROOT_LOCK)" $(SAFE_ENV) $(SUBMAKE_ENV) $(MAKE) -C $(BUILDROOT_DIR) O=$(BUILDROOT_O) BR2_JLEVEL=$(BUILDROOT_JOBS) dropbear-dirclean; \
	fi
	touch $@

$(BUILDROOT_O)/.local-package-%.stamp: $(BUILDROOT_DEFCONFIG_STAMP) $(BUILDROOT_TOOLCHAIN_FIX_STAMP) $(ROOTFS_PACKAGE_FILES) | $(BUILDROOT_OUTPUT_PATH_STAMP)
	# Buildroot's *-rebuild target reuses the already-rsynced local source tree.
	# Remove it first so changes under board/ and buildroot-external/package/ are
	# copied into the package build directory before rebuilding the rootfs.
	flock "$(BUILDROOT_LOCK)" $(SAFE_ENV) $(SUBMAKE_ENV) $(MAKE) -C $(BUILDROOT_DIR) O=$(BUILDROOT_O) BR2_JLEVEL=$(BUILDROOT_JOBS) $*-dirclean
	flock "$(BUILDROOT_LOCK)" $(SAFE_ENV) $(SUBMAKE_ENV) $(MAKE) -C $(BUILDROOT_DIR) O=$(BUILDROOT_O) BR2_JLEVEL=$(BUILDROOT_JOBS) $*
	touch $@

$(ROOTFS_CPIO): $(BUILDROOT_DEFCONFIG_STAMP) $(BUILDROOT_TOOLCHAIN_FIX_STAMP) $(BUILDROOT_DROPBEAR_REFRESH_STAMP) $(BUILDROOT_LOCAL_PACKAGE_STAMPS) $(EMBEDDEDSW_FETCH_STAMP) $(RENDERED_BUILDROOT_POST_BUILD_SCRIPT) $(ROOTFS_OVERLAY_FILES) $(ROOTFS_PACKAGE_FILES) | $(BUILDROOT_OUTPUT_PATH_STAMP)
	flock "$(BUILDROOT_LOCK)" $(SAFE_ENV) $(SUBMAKE_ENV) $(MAKE) -C $(BUILDROOT_DIR) O=$(BUILDROOT_O) BR2_JLEVEL=$(BUILDROOT_JOBS)

$(ROOTFS_EXT4): $(ROOTFS_CPIO)
	test -s "$@"

$(HARDWARE_FILE): $(HW_FILE) $(HW_MANIFEST) scripts/hardware_input.py | $(BUILD_DIR)
	python3 scripts/hardware_input.py --board "$(BOARD)" --xsa "$(HW_FILE)" --manifest "$(HW_MANIFEST)"
	cp "$(HW_FILE)" "$@"

$(BITSTREAM_FILE): $(HARDWARE_FILE) $(HW_MANIFEST) scripts/hardware_input.py | $(BUILD_DIR)
	python3 scripts/hardware_input.py --board "$(BOARD)" --xsa "$(HARDWARE_FILE)" --manifest "$(HW_MANIFEST)" --output-bit "$@"

hardware: $(HARDWARE_FILE) $(BITSTREAM_FILE)

$(LINUX_CONFIG_STAMP): $(SOURCE_IDENTITY_INPUTS) $(LINUX_FETCH_STAMP) $(DTC_COMPAT_STAMP) $(BUILDROOT_TOOLCHAIN_FIX_STAMP) $(BOARD_DIR)/apply-linux-patches.sh scripts/install_capture_driver.py $(CURDIR)/../app/include/linux/fpv_capture.h $(CURDIR)/../app/include/linux/fpv_spectrum.h $(LINUX_PATCH_FILES) $(BOARD_LINUX_FILES) | check-source-board $(STAMP_DIR)
	# Apply Linux kernel patches if available
	if [ -f "$(BOARD_DIR)/apply-linux-patches.sh" ]; then \
		bash "$(BOARD_DIR)/apply-linux-patches.sh" "$(LINUX_DIR)"; \
	fi
	$(HOST_PATH) $(SUBMAKE_ENV) $(MAKE) MAKEOVERRIDES= -C "$(LINUX_DIR)" ARCH=$(LINUX_ARCH) CROSS_COMPILE=$(CROSS_COMPILE) $(LINUX_DEFCONFIG)
	if [ -n "$(strip $(LINUX_CONFIG_ENABLES))" ]; then \
		for cfg in $(LINUX_CONFIG_ENABLES); do \
			"$(LINUX_DIR)/scripts/config" --file "$(LINUX_DIR)/.config" --enable "$$cfg"; \
		done; \
		for cfg in $(LINUX_CONFIG_DISABLES); do \
			"$(LINUX_DIR)/scripts/config" --file "$(LINUX_DIR)/.config" --disable "$$cfg"; \
		done; \
		$(HOST_PATH) $(SUBMAKE_ENV) $(MAKE) MAKEOVERRIDES= -C "$(LINUX_DIR)" ARCH=$(LINUX_ARCH) CROSS_COMPILE=$(CROSS_COMPILE) olddefconfig; \
	fi

	touch $@

$(KERNEL_IMAGE): $(LINUX_CONFIG_STAMP)
	$(HOST_PATH) $(SUBMAKE_ENV) $(MAKE) MAKEOVERRIDES= -C "$(LINUX_DIR)" -j $(NPROC) ARCH=$(LINUX_ARCH) CROSS_COMPILE=$(CROSS_COMPILE) $(LINUX_IMAGE_TARGET)
	touch $@

$(U_BOOT_CONFIG_STAMP): $(SOURCE_IDENTITY_INPUTS) $(UBOOT_FETCH_STAMP) $(DTC_COMPAT_STAMP) $(UBOOT_DTS_STAMP) $(BUILDROOT_TOOLCHAIN_FIX_STAMP) $(RENDERED_UBOOT_CONFIG_FRAGMENT) | $(STAMP_DIR)
	$(HOST_PATH) $(SUBMAKE_ENV) $(MAKE) MAKEOVERRIDES= -C "$(UBOOT_DIR)" ARCH=arm CROSS_COMPILE=$(CROSS_COMPILE) $(UBOOT_BUILD_FLAGS) $(UBOOT_DEFCONFIG)
	cd "$(UBOOT_DIR)" && ./scripts/kconfig/merge_config.sh -m .config $(RENDERED_UBOOT_CONFIG_FRAGMENT)
	$(HOST_PATH) $(SUBMAKE_ENV) $(MAKE) MAKEOVERRIDES= -C "$(UBOOT_DIR)" ARCH=arm CROSS_COMPILE=$(CROSS_COMPILE) $(UBOOT_BUILD_FLAGS) olddefconfig
	touch $@

$(U_BOOT_STAMP): $(U_BOOT_CONFIG_STAMP) | $(STAMP_DIR)
	flock "$(UBOOT_LOCK)" env PATH="$(BUILDROOT_HOST_DIR)/bin:$(PATH)" $(SUBMAKE_ENV) $(MAKE) MAKEOVERRIDES= -C "$(UBOOT_DIR)" -j $(UBOOT_JOBS) ARCH=arm CROSS_COMPILE=$(CROSS_COMPILE) $(UBOOT_BUILD_FLAGS) u-boot.elf tools
	touch $@

$(UBOOT_ELF): $(U_BOOT_STAMP) | $(BUILD_DIR)
	cp "$(UBOOT_DIR)/u-boot.elf" $@


$(BL31_ELF):
	@echo "Skipping BL31 for Zynq-7000 board $(BOARD)"

$(PMUFW_ELF):
	@echo "Skipping PMUFW for Zynq-7000 board $(BOARD)"

$(BSP_STAMP): $(SOURCE_IDENTITY_INPUTS) $(HARDWARE_FILE) $(EMBEDDEDSW_FETCH_STAMP) scripts/build_zynq_fsbl.tcl $(CONSOLE_STAMP) | $(STAMP_DIR)
	test -n "$(VIVADO_SETTINGS)"
	flock "$(XILINX_TOOL_LOCK)" bash -c \
		'source "$$1" && tool="$$2" && command -v "$$tool" >/dev/null && shift 2 && exec "$$tool" "$$@"' \
		_ "$(VIVADO_SETTINGS)" xsct scripts/build_zynq_fsbl.tcl "$(HARDWARE_FILE)" "$(BUILD_DIR)/sdk" "$(CURDIR)/embeddedsw" "$(FSBL_PROC)" "$(FSBL_STDIO)" "$(FSBL_DEBUG_LEVEL)"
	touch $@

$(FSBL_ELF): $(BSP_STAMP)
	if [ ! -d "$(BUILD_DIR)/sdk/hw_0/zynq_fsbl" ]; then \
		echo "missing generated Zynq FSBL directory: $(BUILD_DIR)/sdk/hw_0/zynq_fsbl" >&2; \
		exit 1; \
	fi
	test -n "$(VIVADO_SETTINGS)"
	source "$(VIVADO_SETTINGS)" && command -v arm-none-eabi-gcc >/dev/null && \
		$(MAKE) -C "$(BUILD_DIR)/sdk/hw_0/zynq_fsbl" clean all
	mkdir -p "$(dir $@)"
	if [ -f "$(BUILD_DIR)/sdk/hw_0/zynq_fsbl/fsbl.elf" ]; then \
		cp "$(BUILD_DIR)/sdk/hw_0/zynq_fsbl/fsbl.elf" "$@"; \
	else \
		cp "$(BUILD_DIR)/sdk/hw_0/zynq_fsbl/executable.elf" "$@"; \
	fi


$(DT_SOURCE): $(SOURCE_IDENTITY_INPUTS) $(HARDWARE_FILE) $(DEVICE_TREE_FETCH_STAMP) scripts/gen_dt.tcl $(RENDERED_SYSTEM_USER_DTSI) | $(DT_DIR)
	test -n "$(VIVADO_SETTINGS)"
	flock "$(XILINX_TOOL_LOCK)" bash -c \
		'source "$$1" && tool="$$2" && command -v "$$tool" >/dev/null && shift 2 && exec "$$tool" "$$@"' \
		_ "$(VIVADO_SETTINGS)" xsct scripts/gen_dt.tcl "$(HARDWARE_FILE)" "$(CURDIR)/device-tree-xlnx" "$(DT_DIR)" "$(DT_PROC)" "$(RENDERED_SYSTEM_USER_DTSI)"

$(DTB_FILE): $(DT_SOURCE) | $(BUILD_DIR)
	cpp -nostdinc -undef -D__DTS__ -x assembler-with-cpp -P \
		-I $(DT_DIR) \
		-I $(LINUX_DIR) \
		-I $(LINUX_DIR)/include \
		-I $(LINUX_DIR)/arch/$(LINUX_ARCH)/boot/dts \
		-I $(LINUX_DIR)/arch/$(LINUX_ARCH)/boot/dts/xilinx \
		$(DT_SOURCE) > $(BUILD_DIR)/system-top.pp.dts
	dtc -@ -I dts -O dtb -o $@ $(BUILD_DIR)/system-top.pp.dts

$(FIT_ITS): scripts/gen_image_its.sh $(KERNEL_IMAGE) $(DTB_FILE) $(ROOTFS_CPIO) | $(BUILD_DIR)
	scripts/gen_image_its.sh "$@" "$(KERNEL_IMAGE)" "$(DTB_FILE)" "$(ROOTFS_CPIO)" "$(FIT_DESCRIPTION)" "$(FIT_KERNEL_LOAD_ADDR)" "$(FIT_KERNEL_ENTRY_ADDR)" "$(RAMDISK_IMAGE_ARCH)"

$(IMAGE_UB): $(FIT_ITS) $(U_BOOT_STAMP) $(KERNEL_IMAGE) $(DTB_FILE) $(ROOTFS_CPIO) | $(BUILD_DIR)
	$(HOST_PATH) "$(UBOOT_DIR)/tools/mkimage" -f $(FIT_ITS) $@ || \
	{ \
		test -s "$@" && \
		fdtget "$@" / description >/dev/null 2>&1 && \
		fdtget "$@" /configurations default >/dev/null 2>&1 && \
		fdtget "$@" /images/kernel@1 type >/dev/null 2>&1; \
	}

ifeq ($(RAMDISK_WRAP_MKIMAGE),y)
$(URAMDISK_FILE): $(ROOTFS_CPIO) $(U_BOOT_STAMP) | $(BUILD_DIR)
	$(HOST_PATH) "$(UBOOT_DIR)/tools/mkimage" -A $(RAMDISK_IMAGE_ARCH) -O linux -T ramdisk -C gzip -n "$(BOARD_DESC) initramfs" -d "$(ROOTFS_CPIO)" "$@"
else
$(URAMDISK_FILE): $(ROOTFS_CPIO) | $(BUILD_DIR)
	cp "$(ROOTFS_CPIO)" "$@"
endif

$(BOOT_BIF): $(FSBL_ELF) $(UBOOT_ELF) $(if $(filter y,$(BOOT_BIN_INCLUDE_BITSTREAM)),$(BITSTREAM_FILE)) | $(BUILD_DIR)
	{ \
		echo 'the_ROM_image:'; \
		echo '{'; \
		echo '  [bootloader] $(FSBL_ELF)'; \
		if [ "$(BOOT_BIN_INCLUDE_BITSTREAM)" = "y" ]; then \
			echo '  $(BITSTREAM_FILE)'; \
		fi; \
		echo '  $(UBOOT_ELF)'; \
		echo '}'; \
	} > $@

$(BOOT_BIN): $(BOOT_BIF) $(FSBL_ELF) $(UBOOT_ELF) $(if $(filter y,$(BOOT_BIN_INCLUDE_BITSTREAM)),$(BITSTREAM_FILE)) | $(BUILD_DIR)
	test -n "$(VIVADO_SETTINGS)"
	source "$(VIVADO_SETTINGS)" && command -v bootgen >/dev/null && \
		bootgen -arch $(BOOTGEN_ARCH) -image "$(BOOT_BIF)" -o "$@" -w on
	printf '%s\n' "$(FSBL_BOOT_PROFILE)|$(BOOT_BIN_INCLUDE_BITSTREAM)" > "$(BOOT_PROFILE_STAMP)"

$(SD_DIR)/$(SD_BOOT_IMAGE_NAME): FORCE | $(SD_DIR)
	if [ "$$(cat "$(BOOT_PROFILE_STAMP)" 2>/dev/null)" != "$(FSBL_BOOT_PROFILE)|$(BOOT_BIN_INCLUDE_BITSTREAM)" ]; then \
		rm -rf "$(BUILD_DIR)/sdk" "$(BOOT_BIN)" "$(BOOT_BIF)" \
			"$(FSBL_ELF)" "$(PMUFW_ELF)" "$(BSP_STAMP)"; \
	fi
	$(MAKE) BOARD="$(BOARD)" FSBL_BOOT_PROFILE="$(FSBL_BOOT_PROFILE)" \
		BOOT_BIN_INCLUDE_BITSTREAM="$(BOOT_BIN_INCLUDE_BITSTREAM)" "$(BOOT_BIN)"
	cp "$(BOOT_BIN)" $@

$(SD_DIR)/$(SD_KERNEL_IMAGE_NAME): $(KERNEL_IMAGE) | $(SD_DIR)
	cp $< $@

$(SD_DIR)/$(SD_DTB_NAME): $(DTB_FILE) | $(SD_DIR)
	cp $< $@

$(SD_DIR)/$(SD_RAMDISK_NAME): $(URAMDISK_FILE) | $(SD_DIR)
	cp $< $@

ifneq ($(strip $(SD_BITSTREAM_NAME)),)
$(SD_BITSTREAM_FILE): $(BITSTREAM_FILE) | $(SD_DIR)
	find "$(SD_DIR)" -maxdepth 1 -type f -name '*.bit' ! -name "$(SD_BITSTREAM_NAME)" -delete
	cp $< $@
endif

$(SD_DIR)/uEnv.txt: $(RENDERED_UENV_FILE) | $(SD_DIR)
	cp $< $@

$(SD_BUILD_INFO): $(SD_DIR)/$(SD_BOOT_IMAGE_NAME) $(SD_DIR)/$(SD_KERNEL_IMAGE_NAME) $(SD_DIR)/uEnv.txt $(SD_DIR)/$(SD_DTB_NAME) $(SD_DIR)/$(SD_RAMDISK_NAME) $(SD_BITSTREAM_FILE) | $(SD_DIR)
	{ \
		echo "format=$(BOARD)-sd-boot-manifest-v1"; \
		echo "board=$(BOARD)"; \
		echo "version=$$(git describe --abbrev=8 --dirty --always --tags 2>/dev/null || echo unknown)"; \
		echo "created_utc=$$(date -u +%Y-%m-%dT%H:%M:%SZ)"; \
		for f in "$(SD_DIR)"/*; do \
			[ -f "$$f" ] || continue; \
			name="$$(basename "$$f")"; \
			[ "$$name" != "$$(basename "$@")" ] || continue; \
			echo "file.$$name.size=$$(stat -c '%s' "$$f")"; \
			echo "file.$$name.sha256=$$(sha256sum "$$f" | awk '{print $$1}')"; \
		done; \
	} > $@

sd: check-source-board $(SD_DIR)/$(SD_BOOT_IMAGE_NAME) $(SD_DIR)/$(SD_KERNEL_IMAGE_NAME) $(SD_DIR)/uEnv.txt $(SD_DIR)/$(SD_DTB_NAME) $(SD_DIR)/$(SD_RAMDISK_NAME) $(SD_BITSTREAM_FILE) $(SD_BUILD_INFO)

kernel: check-source-board $(KERNEL_IMAGE)
u-boot: check-source-board $(UBOOT_ELF)
fsbl: check-source-board $(FSBL_ELF)
dt: check-source-board $(DTB_FILE)
imageub: check-source-board $(IMAGE_UB)
bootbin: check-source-board $(BOOT_BIN)
rootfs: check-source-board $(ROOTFS_CPIO)
rootfs-ext4: check-source-board $(ROOTFS_EXT4)
ramdisk: check-source-board $(URAMDISK_FILE)

$(BUILD_DIR):
	mkdir -p $@

$(DT_DIR):
	mkdir -p $@

$(SD_DIR):
	mkdir -p $@

$(EMMC_DIR):
	mkdir -p $@

$(QSPI_DIR):
	mkdir -p $@

$(FIRMWARE_DIR):
	mkdir -p $@

$(STAMP_DIR):
	mkdir -p $@

$(SOURCE_STAMP_DIR):
	mkdir -p $@

clean:
	rm -rf "$(BUILD_DIR)" "$(BUILDROOT_O)"
