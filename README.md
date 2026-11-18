# E200 FPV capture application and firmware integration

E200 analog FPV reception uses userspace ADI no-OS control of the AD9361, four FPGA DDC/video detectors and a shared DMA capture interface. The application provides PAL/NTSC grayscale image capture, hybrid RF LO/DDC scanning and an English offline web dashboard. It does not depend on libiio.

This Git history contains the application and firmware integration files. **FPGA source and internal engineering records remain private. The active capture-driver source and one XSA hardware export are included, so complete firmware can be built without the FPGA source project.** See [Repository contents and private dependencies](docs/public-repository.md).

## Contents

- `app/`: no-OS radio integration, capture interface/packet parser, scanner, HTTP service, English UI and application tests. Vendored third-party files retain their license notices.
- `firmware/`: pinned upstream repositories, E200 XSA, public capture-driver source, board configuration, Buildroot packages/overlay and build/packaging scripts. Upstream worktrees are fetched locally, not vendored into this Git history.
- `tests/`: source pinning, patching, board configuration and publication-boundary checks.
- `tools/u220_fpv_tx/`: host PAL/NTSC grayscale FM generation, desktop GUI and U220 UHD replay; see [test-source instructions](tools/u220_fpv_tx/README.md).
- `tools/check_public_tree.py` and `.githooks/pre-commit`: checks for excluded paths and private implementation copies before a commit.

## Build and test the application

```sh
cmake -S app -B build/host -DCMAKE_BUILD_TYPE=Release
cmake --build build/host -j8
ctest --test-dir build/host --output-on-failure
python3 -m unittest discover -s tests -v
```

No RF hardware is needed for these host tests. They include the public capture-driver control/read functions exercised with fake I/O; they do not model FPGA logic or qualify a hardware release.

Use explicit simulation for local UI development:

```sh
build/host/fpv-webd --simulate --listen 127.0.0.1 --port 8080 --web-root app/web
```

Simulation creates synthetic images and does not operate the receiver.

## On the E200

With the matching FPGA bitstream and kernel installed, `S95fpv-web` starts the web service. Open `http://192.168.1.10/` on the directly connected network. Click **Start scan** to initialize RF, set the ranges and gain, and view validated grayscale frames. Stop the service before using diagnostic command-line tools that access the same radio/device.

The green status LED (`led0:green`, PS MIO0) uses the Linux heartbeat trigger after the GPIO LED driver starts. It indicates kernel activity; it does not indicate RF lock, a running scan or a healthy web service. No userspace blinking daemon is required.

The default search grid is **6 MHz** with 150 ms observation. **Fine** uses a 2 MHz grid and 150 ms observation; **Fast** uses an 8 MHz grid and 100 ms observation. The grid sets the spacing between search centers; each DDC mixes a center into baseband. The planner groups centers into RF windows and changes the AD9361 LO when moving to another window. Each RF window includes every search center that fits within the 40 MHz center span, even when the count is not divisible by four. At the default 6 MHz grid, a full window covers seven centers in two digital phases: offsets -18, -6, +6 and +18 MHz first, then -12, 0 and +12 MHz at the same LO. Only after both phases does the next full window move the LO by 42 MHz. RF settling applies only to an RF change; each digital phase gets its own FPGA tuning epoch. Scan progress shows a percentage and progress bar. Coarse settings can miss off-center signals; use Fine and longer observation for a suspected range. Confirmed frequencies represent DDC centers, and adjacent records can represent one transmitter. Configuration and images are held in RAM.

**Fixed frequency:** enter a center in **Fixed frequency → Frequency (MHz)** and click
**Lock frequency**, or select a detection and click **Lock this frequency**.
This can interrupt an active scan without a separate Stop command. The receiver
sets the RF LO to the requested frequency, enables CH 0 with zero DDC offset,
and keeps that tune epoch while repeatedly capturing validated grayscale frames.
PAL/NTSC detection remains automatic. Signal loss leaves the receiver on that
frequency waiting for video to return. Locking does not require the Repeat scan
checkbox. **Unlock frequency** (or **Resume scan**) resumes scanning with the range settings; **Stop capture** ends
acquisition. Changing the locked frequency starts a new fixed capture session.

The fixed mode displays the held frequency, video-sync status and captured-frame
count. The browser checks for new frames every 200 ms and waits for an image
request to finish before loading another. Actual capture/display rates depend
on the RF signal, DMA completion and browser/network throughput. Images remain
in RAM, using the existing BMP endpoint. Fixed mode requires only an application
and web-assets update; it uses the existing v6 FPGA/kernel interface.

The current board was verified after the 2026-11-17 rootfs update: Ethernet uses the working `rgmii-id` DT, the English service starts, and real NTSC frames were received. New full builds use the same `rgmii-id` PHY delay setting. Preserve the working boot files when applying a rootfs-only update.

## Firmware integration

```sh
make -C firmware show-config
make -C firmware hardware
make -C firmware fetch-locked
make -C firmware sd
```

Use Vivado/Vitis 2022.2 and the configured ARM hard-float toolchain. See the repository contents guide for host dependencies and build details. The build extracts the bitstream from the released XSA, generates FSBL/device tree, and compiles U-Boot, Linux, the capture driver and rootfs. The six SD boot files appear in `firmware/build/e200/sd/`. FPGA source is not required; rebuilding FPGA logic still requires the private project. Source-fetch scripts use `firmware/sources.lock.json`; never replace locally modified upstream worktrees blindly.

## Commit checks

```sh
git config --local core.hooksPath .githooks
python3 tools/check_public_tree.py
```

The check examines the Git index, including content staged from patches/archives. Ignored private files stay available locally. A local commit does not publish a repository or grant new license terms to project-owned code.
