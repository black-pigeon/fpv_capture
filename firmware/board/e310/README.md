# Original ANTSDR E310 firmware

Build with `make -C firmware BOARD=e310 sd`. The default board remains E200.
Use `NPROC=4 BUILDROOT_JOBS=4 UBOOT_JOBS=2` alongside another FPGA build.
`BUILDROOT_JOBS` defaults to `NPROC` and survives Buildroot's umask restart.
The PS reference is `v0.39_antsdr/plutosdr-fw` with `TARGET=ant`, not
`e310v2` or the Ettus E31x platform. The current LVDS radio shell follows the
user-selected `SDR-Firmware/fmcomms_module/antsdr_standalone/hdl/projects/antsdre310`
reference; its PS Ethernet/UART/DDR configuration matches `ant`.

The board uses xc7z020clg400-2, 32-bit MT41J256M16 RE-125 DDR (1 GiB),
PS GEM0 on MIO16..27 and MDIO52/53, and a Marvell 88E1512 at PHY address 0.
The PHY uses `rgmii-id`, with active-low reset on PS GPIO46. There is no PL
Ethernet bridge. UART1 on MIO12/13 is `ttyPS1`, base `0xe0001000`, in the
FSBL, U-Boot, Linux, boot environment and rootfs getty.

AD9361 is controlled by the shared userspace no-OS applications through
`/dev/spidev0.0` and GPIO character devices. Controller offsets are reset100,
sync99, en_agc98, ctl94 and status86. RX path switches54/55/58/59 remain grouped
under application control. Linux IIO is disabled. The current user-selected
FPGA target is LVDS **2R2T** at 61.44 MSPS, `ADC_INIT_DELAY=29`, using the LVDS
reference's LVDS_25 data pins and mixed LVCMOS25/LVCMOS33 control pins.
RX1 feeds the four shared DDC channels. The shell omits the inherited E200
PPS/10 MHz/DAC auxiliary interface, which is absent from this LVDS reference.
PS initialization and DDR timing come from the matching reviewed XSA.

## Hardware input

Supply only the implementation-validated export and its matching manifest:

- `hardware/e310_fpv.xsa`
- `hardware/manifest.json`

No placeholder XSA or fabricated manifest is provided. `make ... hardware`
uses `scripts/hardware_input.py --board e310` to validate the input and
extract the bitstream. The manifest Ethernet object is:

```json
{"interface":"ps-gem-mio","phy_mode":"rgmii-id","phy_address":0,"converter_address":null,"forced_phy_id":false}
```

Firmware configuration, kernel, U-Boot and rootfs can be built independently.
FSBL, generated DT and final SD assembly require the matching hardware pair.

## Source and output isolation

E310 extracts pristine sources from the **local Git objects at the commits in
`sources.lock.json`** into `build/e310/sources/{linux,u-boot,buildroot}`.
Existing E200 source patches and in-tree build products are not copied or
modified. The existing source-board guard records E310 ownership separately
in `build/e310/.source-stamps/active-board`; E200 retains its shared-source
marker and original source paths. Alternating E200/E310 builds is safe with
these default paths. Use distinct output roots for concurrent independent
builds of the same board.

An ownership marker `.fpv-source.json` records each private source's locked
commit. Repeated builds preserve patches and build products in that private
copy. A changed lock or an unowned destination fails without discarding it;
preserve the old output and select a new `BUILD_DIR` (and a matching fresh
`BUILDROOT_O`), or deliberately remove only disposable E310 output yourself.

The capture driver and UAPI remain canonical in
`board/e200/linux/fpv_capture/` and `../app/include/linux/fpv_capture.h`.
E310 installs those sources without applying E200 Ethernet patches. Capture
register address `0x43c30000`, DMA channel and IRQ routing remain compatible
with E200; generated DMA/interrupt nodes come from the XSA.

Buildroot retains the historical `e200-radio-probe` package name for the shared
applications. The second external tree at `firmware/package/` passes
`-DFPV_BOARD=e310` from E310's `BR2_FPV_BOARD` setting. It defaults to E200.
Rootfs identity, UART getty, network configuration and service labels are local
to E310. Dropbear local options are a regular board-local copy of the E200 header. The initial static Ethernet address is `192.168.1.10/24`.

Outputs are in `firmware/build/e310/`, Buildroot output in
`firmware/build/buildroot/e310/`. SD files are `BOOT.BIN`, `e310_fpv.bit`,
`zImage`, `devicetree.dtb`, `rootfs.cpio.gz`, `uEnv.txt` and
`e310-firmware.manifest`. U-Boot loads the reviewed bitstream from SD before
booting Linux. Board link, RAM and RF behavior still require physical validation.

The packaging scripts accept `--board e310` and default to E200:

```sh
python3 firmware/scripts/package_test_sd.py --board e310 --verify-only
python3 firmware/scripts/package_rootfs_update.py --board e310 --verify-only
```

They resolve board-specific hardware inputs through the public validator and
check the default board-specific build output paths. Packaging a different
output root requires arranging those default paths, as with the existing E200
packaging workflow. The shared web asset directory remains `usr/share/e200-fpv/web`.
