# Public source and FPGA release boundary

The public firmware build uses application and capture-driver source plus one reviewed FPGA hardware handoff, `firmware/board/e200/hardware/e200_fpv.xsa`. It does not require the FPGA source project. The adjacent `manifest.json` pins the XSA, its embedded bitstream and the supported interface versions.

## Included

- Userspace ADI no-OS AD9361 control, scanner, frame parser, capture CLI, HTTP service and English offline web assets.
- `firmware/board/e200/linux/fpv_capture/`: the active GPL-2.0 DMAengine capture client, its Kconfig/Makefile and license text. It builds into the kernel as `CONFIG_FPV_CAPTURE=y`; upstream `CONFIG_XILINX_DMA=y` supplies the AXI DMA provider.
- `app/include/linux/fpv_capture.h`: the canonical userspace ioctl/data interface, copied into the kernel driver during integration.
- Host application and capture-driver tests.
- E200 device-tree/boot configuration, Buildroot packages/overlay, source version locks and firmware/packaging scripts.
- One XSA and its public hash/compatibility manifest. The XSA contains the bitstream, hardware/address descriptions and generated PS initialization files. It is not an opaque container and reveals hardware integration information; it does not include the FPGA RTL project.

Third-party sources retain their existing notices. The capture driver retains its GPL-2.0 declaration; its `COPYING` provides the GPLv2 text. No blanket license is added to other project-owned files.

## Retained locally

| Local path | Contents |
|---|---|
| `fpga/` | FPGA RTL, IP, constraints, block design, reconstruction scripts and simulations |
| `hardware/` | Internal exports, source-hash inventories, reports and debug products |
| `firmware/board/e200/linux/axi_dma/` | Unused legacy driver with conflicting third-party distribution notices; not required by the active capture path |
| `firmware/board/e200/patches/autosave/` | Historical source patches and source archives |
| Most of `docs/` | Internal designs, plans and engineering records |
| FPGA-dependent verification, upstream checkouts and build/device data | Local development inputs and outputs |

Ignoring a path does not delete it or back it up. Keep a separate private backup of the FPGA project. Never include `.ltx`, `.dcp`, Vivado projects, source patches or private source inventories in a public release.

## Build complete SD boot files

Use a Linux host with AMD Vivado and Vitis **2022.2**, including the ARM hard-float compiler. The default installation is `/opt/Xilinx`; the environment script also supports `VIVADO_SETTINGS_FILE` and `VITIS_SETTINGS_FILE`. Buildroot's external-toolchain path is configured separately in the E200 defconfig and must match the installation.

Install host build dependencies such as a C/C++ compiler, make, Git, Python 3, CMake, patch, flex, bison, OpenSSL development headers, device-tree-compiler (`dtc`, `fdtget`, `fdtput`), cpio, unzip, rsync, file, bc, wget and util-linux (`flock`). Buildroot reports additional host dependencies if needed. XSA validation/extraction alone only requires Python 3 and make.

From the repository root:

```sh
make -C firmware show-config
make -C firmware hardware
make -C firmware fetch-locked
make -C firmware sd
```

`fetch-locked` downloads the exact commits in `firmware/sources.lock.json`; run it before the first build in a clean checkout. Do not run source-fetch/reset operations blindly over modified upstream checkouts.

The build validates the XSA, extracts its bitstream into `firmware/build/e200/`, generates FSBL and the device tree from the XSA, compiles U-Boot/Linux/the capture driver, and builds the application and rootfs. No FPGA synthesis/implementation runs, and no precompiled `.ko` is needed. Kernel configuration changes are possible because the active capture driver is provided as source; keep the capture/DMA/device-tree interfaces compatible.

The six boot files are written to `firmware/build/e200/sd/`:

```text
BOOT.BIN
e200_fpv.bit
zImage
devicetree.dtb
rootfs.cpio.gz
uEnv.txt
```

The `.bit` is a generated output extracted from the XSA, not a second FPGA input to distribute. Keep all six files from the same build together. The canonical Ethernet device tree uses the board-tested `rgmii-id` setting.

To select another reviewed handoff, supply **both** files consistently on every build command:

```sh
make -C firmware hardware HW_FILE=/absolute/path/e200_fpv.xsa HW_MANIFEST=/absolute/path/manifest.json
make -C firmware sd HW_FILE=/absolute/path/e200_fpv.xsa HW_MANIFEST=/absolute/path/manifest.json
```

XSA membership is intentionally restricted to the current E200 handoff format. A new FPGA release requires a reviewed manifest and compatible register/frame interfaces. Changing a hash does not prove the design works: validate new hardware privately before publishing it.

The optional packaging scripts validate the released XSA instead of private FPGA source hashes:

```sh
python3 firmware/scripts/package_test_sd.py --verify-only
python3 firmware/scripts/package_rootfs_update.py --verify-only
```

Both accept `--xsa` and `--hardware-manifest` for an alternate handoff. Host tests and archive checks are not board/RF qualification. A rootfs-only update must preserve the currently working kernel, bitstream, device tree and boot files.

## Check the publication boundary

```sh
git config --local core.hooksPath .githooks
python3 tools/check_public_tree.py
git diff --cached --stat
```

The guard inspects the **Git index**. It permits only the named capture-driver files and the exact XSA/manifest pair; the indexed XSA must match its manifest and pass archive-member checks. It rejects other hardware exports, RTL, internal paths, binary/archive content, private keys and byte-identical copies of known local FPGA sources. Review the staged diff as well: this guard is a practical safeguard, not a proof that arbitrary text contains no confidential information.

Publishing this source and XSA allows rebuilding the firmware software and reusing the released FPGA design. It does not allow rebuilding or modifying FPGA logic without the private FPGA project.
