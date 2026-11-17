# E200 FPV capture application and firmware integration

E200 analog FPV reception uses userspace ADI no-OS control of the AD9361, four FPGA DDC/video detectors and a shared DMA capture interface. The application provides PAL/NTSC grayscale image capture, hybrid RF LO/DDC scanning and an English offline web dashboard. It does not depend on libiio.

This Git history contains the application and firmware integration files. **FPGA source, hardware exports, custom Linux capture/DMA implementations and internal engineering records are maintained separately and are not included.** See [Repository contents and private dependencies](docs/public-repository.md).

## Contents

- `app/`: no-OS radio integration, capture interface/packet parser, scanner, HTTP service, English UI and application tests. Vendored third-party files retain their license notices.
- `firmware/`: pinned upstream repositories, E200 board configuration, Buildroot packages/overlay and build/packaging scripts. Upstream worktrees are fetched locally, not vendored into this Git history.
- `tests/`: source pinning, patching, board configuration and publication-boundary checks.
- `tools/check_public_tree.py` and `.githooks/pre-commit`: checks for excluded paths and private implementation copies before a commit.

## Build and test the application

```sh
cmake -S app -B build/host -DCMAKE_BUILD_TYPE=Release
cmake --build build/host -j8
ctest --test-dir build/host --output-on-failure
python3 -m unittest discover -s tests -v
```

No RF hardware is needed for these host tests. The driver-implementation test is enabled only in a private workspace containing its test source and driver source. A public-only checkout runs the application tests without that private test.

Use explicit simulation for local UI development:

```sh
build/host/fpv-webd --simulate --listen 127.0.0.1 --port 8080 --web-root app/web
```

Simulation creates synthetic images and does not operate the receiver.

## On the E200

With the matching private FPGA and kernel installed, `S95fpv-web` starts the web service. Open `http://192.168.1.10/` on the directly connected network. Click **Start scan** to initialize RF, set the ranges and gain, and view validated grayscale frames. Stop the service before using diagnostic command-line tools that access the same radio/device.

**Fine** uses a 2 MHz grid and 150 ms observation; **Fast** uses an 8 MHz grid and 100 ms observation. The scanner holds one RF LO while four digital tuners visit distributed centers. Coarse settings can miss off-center signals; use Fine and longer observation for a suspected range. Confirmed frequencies represent DDC centers, and adjacent records can represent one transmitter. Configuration and images are held in RAM.

The current board was verified after the 2026-11-17 rootfs update: Ethernet uses the working `rgmii-id` DT, the English service starts, and real NTSC frames were received. The checked-in canonical DT remains an integration input and is not a replacement for a known-working device tree. Preserve the working boot files when applying a rootfs-only update.

## Firmware integration

```sh
make -C firmware show-config
make -C firmware help
```

Complete firmware/SD construction requires the private dependencies listed in the repository contents guide. This source subset alone cannot regenerate the FPGA or custom kernel driver. Source-fetch scripts use `firmware/sources.lock.json`; never replace locally modified upstream worktrees blindly.

## Commit checks

```sh
git config --local core.hooksPath .githooks
python3 tools/check_public_tree.py
```

The check examines the Git index, including content staged from patches/archives. Ignored private files stay available locally. A local commit does not publish a repository or grant new license terms to project-owned code.
