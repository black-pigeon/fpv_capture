# ANTSDR E200 / E310 FPV capture and firmware integration

Analog FPV reception uses userspace ADI no-OS control of the AD9361, four FPGA DDC/video detectors and a shared DMA capture interface. The application provides PAL/NTSC grayscale image capture, hybrid RF LO/DDC scanning and an English offline web dashboard. It does not depend on libiio.

This Git history contains the application and firmware integration files. **FPGA source and internal engineering records remain private. The active capture-driver source and board-specific XSA hardware exports are included, so complete firmware can be built without the FPGA source project.** See [Repository contents and private dependencies](docs/public-repository.md).

## Contents

- `app/`: no-OS radio integration, capture interface/packet parser, scanner, HTTP service, English UI and application tests. Vendored third-party files retain their license notices.
- `firmware/`: pinned upstream repositories, board-specific XSAs, public capture-driver source, board configuration, Buildroot packages/overlay and build/packaging scripts. Upstream worktrees are fetched locally, not vendored into this Git history.
- `tests/`: source pinning, patching, board configuration and publication-boundary checks.
- `tools/fpv_receiver/`: Python host SDK/recorder and versioned HTTP frame-stream protocol; see [network API and receiver instructions](tools/fpv_receiver/README.md).
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

**Live RX gain:** edit **RX gain** and click **Apply** while scanning or locked;
no stop/restart is needed. The range is 0–60 dB in 1 dB increments. The worker
applies the change between captures; video sync may briefly reacquire.
Frequency ranges, search grid and manual lock frequency are editable after
stopping acquisition. Advanced timing and receiver details are collapsed by default.

**Fixed frequency:** stop acquisition, enter a center in **Lock → Frequency (MHz)** and click
**Lock frequency**, or select a detection and click **Lock this frequency**.
Locking a selected detection can interrupt an active scan without a separate Stop command. The receiver
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

## Host frame streaming

`GET /api/v1/stream` on the existing HTTP port delivers captured BMP frames with
matching frequency, PAL/NTSC, sequence, monotonic time and channel metadata.
One connection can receive every detected center; the host recorder separates
files by nominal frequency, standard and device-process session. Fixed-frequency
mode continuously forwards successful captures without waiting for web polling.
Scanning gives each frequency images when its window is revisited. The bounded
history can drop frames for slow clients, with explicit gap reporting; capture
and web controls keep running.

```sh
python3 tools/fpv_receiver/receive.py --url http://192.168.1.10 --output fpv-recordings
# Optionally request fixed acquisition and receive its frames:
python3 tools/fpv_receiver/receive.py --url http://192.168.1.10 --lock-mhz 5800 --gain-db 40
```

See [the network API and Python SDK](tools/fpv_receiver/README.md) for frequency
subscriptions, remote scans, replay cursors and integration examples. This
feature requires an updated rootfs/application; the released XSA and kernel
capture interface remain compatible.

## E310 board support

Select the original **ANTSDR E310 (`ant`)**, with PS MIO Ethernet and a switched
RF front end. This target does not describe Ettus E310 or ANTSDR E310v2.

| Setting | E200 | E310 |
|---|---|---|
| Application build | `-DFPV_BOARD=e200` (default) | `-DFPV_BOARD=e310` |
| AD9361 digital interface | 6-bit LVDS, 2R2T | 6-bit LVDS, 2R2T |
| FPV input / channelization | 61.44 MSPS / four 15.36 MSPS DDCs | Same |
| Ethernet | PS GEM0 via EMIO and PL bridge | PS GEM0 via MIO, Marvell PHY at address 0 |
| Console | UART0 / ttyPS0 | UART1 / ttyPS1 |
| RX path | A balanced | B below 3000 MHz; A at 3000–6000 MHz |

The E310 hardware shell and no-OS configuration use the `antsdre310` LVDS
reference, including its GPIO map and delay settings. One physical RX stream
feeds the existing four DDCs at their full input rate. The application holds
the RX switch GPIOs while it owns the radio, updates both switch paths together,
and selects the matching AD9361 input before changing the LO. Scan windows split
at 3000 MHz while retaining the requested center grid. Detection, automatic
PAL/NTSC selection, fixed-frequency capture, the web dashboard and the host
stream API are shared with E200. `/api/status` includes `board` and `rx_port`.

Build the matching board with:

```sh
make -C firmware BOARD=e310 show-config
make -C firmware BOARD=e310 hardware
make -C firmware BOARD=e310 sd
```

E310 SD boot files are written to `firmware/build/e310/sd/`. Use the matching
E310 XSA, FSBL, bitstream, device tree, kernel and rootfs as a set. Host tests,
RTL simulation and successful builds do not replace E310 Ethernet, RF-path,
PAL/NTSC and live DMA validation on hardware.

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
