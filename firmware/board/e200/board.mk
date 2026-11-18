BOARD_NAME := e200
BOARD_DESC := ANTSDR E200 Zynq-7000 firmware builder

SOC_FAMILY := zynq
BOOTGEN_ARCH := zynq

# The public firmware build consumes a single reviewed XSA, not FPGA source.
HW_ARTIFACT_DIR := $(CURDIR)/board/e200/hardware
HW_FILE := $(HW_ARTIFACT_DIR)/e200_fpv.xsa
HW_MANIFEST := $(HW_ARTIFACT_DIR)/manifest.json
# Generated from the XSA; never a separately supplied hardware input.
HW_BIT = $(BUILD_DIR)/e200_fpv.bit

FSBL_PROC := ps7_cortexa9_0
DT_PROC := ps7_cortexa9_0
FSBL_STDIO := ps7_uart_0

LINUX_ARCH := arm
LINUX_DEFCONFIG := xilinx_zynq_defconfig
LINUX_IMAGE_TARGET := zImage
LINUX_IMAGE_PATH := arch/arm/boot/zImage

UBOOT_DEFCONFIG := xilinx_zynq_virt_defconfig
UBOOT_DTS_NAME := zynq-e200

LINUX_CONFIG_ENABLES := DMADEVICES XILINX_DMA FPV_CAPTURE MACB XILINX_GMII2RGMII REALTEK_PHY SPI_SPIDEV GPIO_CDEV NEW_LEDS LEDS_CLASS LEDS_GPIO LEDS_TRIGGERS LEDS_TRIGGER_HEARTBEAT MTD MTD_SPI_NOR SPI_ZYNQ_QSPI

BUILDROOT_CROSS := arm-linux-gnueabihf-
VITIS_AARCH64_LINUX_DIR := /opt/Xilinx/Vitis/2022.2/gnu/aarch32/lin/gcc-arm-linux-gnueabi
CONSOLE_UART := 0
CONSOLE_TTY := ttyPS0
CONSOLE_UART_BASE := 0xe0000000
CONSOLE_EARLYCON := cdns,mmio,$(CONSOLE_UART_BASE)

SD_BOOT_IMAGE_NAME := BOOT.BIN
SD_KERNEL_IMAGE_NAME := zImage
SD_RAMDISK_NAME := rootfs.cpio.gz
RAMDISK_IMAGE_ARCH := arm
RAMDISK_WRAP_MKIMAGE := n
BOOT_BIN_INCLUDE_BITSTREAM := n
SD_BITSTREAM_NAME := e200_fpv.bit
FIRMWARE_FORMAT := e200-firmware-v1

BUILDROOT_LOCAL_PACKAGES += e200-radio-probe
