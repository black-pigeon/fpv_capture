# Repository contents and private dependencies

This commit tracks the userspace application and firmware integration. It does not include project-owned FPGA or custom Linux driver implementation. The exclusion applies to source copies inside patches, archives and generated/debug artifacts as well as original files. Existing files remain in the private development workspace; ignoring a path does not delete it.

## Included

- Userspace no-OS radio integration, scanner, frame parser, capture CLI and HTTP service.
- English offline web assets and application tests.
- `app/include/linux/fpv_capture.h`: ioctl/data interface declarations required by applications, not the driver implementation.
- E200 device-tree integration, boot configuration, Buildroot overlay/packages and public upstream version lock/build scripts.
- Generic source/build tests, this contents guide and Git publication checks.

Third-party source files keep their existing copyright/license headers. This commit does not add a blanket open-source license or change the terms of project-owned files.

## Excluded and retained locally

| Local path | Reason |
|---|---|
| `fpga/` | All RTL, IP, constraints, block design, reconstruction scripts and simulations |
| `hardware/` | Bit/XSA/LTX, PS initialization, manifests and reports with implementation/debug details |
| `firmware/board/e200/linux/*/` | Custom `axi_dma` and `fpv_capture` kernel implementations, headers, Kconfig and Makefiles |
| `firmware/board/e200/patches/autosave/` | Historical source patches and untracked-source archives |
| Most of `docs/` | Internal design specs, plans, source manifests and engineering/board records |
| Private driver/FPGA verification files | Tests coupled to excluded implementations |
| Upstream worktrees, build outputs, editor settings and captured packets/images | Independent repositories and local development/device data |

The board-level `linux/system-user.dtsi` is a device-tree integration file; it is included. Vendor userspace files named `axi_adc_core`/`axi_dac_core` belong to no-OS and are not the excluded custom AXI DMA kernel implementation.

## Building from this source subset

The README host commands build and test the application without private FPGA/kernel sources. Test binaries use fake devices or explicit simulation. A real receiver still needs the matching private bitstream, kernel and capture ABI.

For a full firmware build, provide these inputs separately through an authorized private channel:

1. `fpga/e200_fpv/` and matching exports in `hardware/e200_fpv/` when regenerating hardware or using hardware-manifest verification.
2. Custom kernel input directories under `firmware/board/e200/linux/axi_dma/` and `firmware/board/e200/linux/fpv_capture/`.
3. A matching working Ethernet/device-tree/boot configuration for the board. Do not replace a verified SD device tree merely to rebuild userspace.

`apply-linux-patches.sh` reports missing private driver inputs before modifying the kernel tree. Firmware/packaging scripts that verify private manifests also require their private inputs. The public subset intentionally cannot rebuild the whole system independently.

## Check each commit

Enable the provided local hook once per checkout:

```sh
git config --local core.hooksPath .githooks
```

Stage explicit application/integration paths, then run:

```sh
python3 tools/check_public_tree.py
git diff --cached --stat
```

The guard inspects indexed paths and content and rejects hardware/kernel source, private archive/binary artifacts, internal documentation, credential files/private keys and source copies matching local private implementation. It supplements `.gitignore` by also rejecting a forced add. It does not make untracked private files recoverable from Git and does not publish anything. Preserve those files using a separate private backup process.
