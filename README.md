# AntSDR Analog FPV Scanner

**English** | [简体中文](README.zh-CN.md)

Analog FPV receiver firmware for **AntSDR E200 and AntSDR E310**, with automatic PAL/NTSC detection and grayscale video through an onboard web interface.

- **Frequency scanning**: scan selected bands and view detected frequencies, video standards and images.
- **Fixed-frequency reception**: lock to a frequency for continuous image capture, with an Ethernet stream for host applications.

## Download and boot

Prebuilt boot packages will be published in [GitHub Releases](https://github.com/black-pigeon/fpv_capture/releases). Choose `e200-fpv-<version>-bootfiles.tar.gz` or `e310-fpv-<version>-bootfiles.tar.gz` for your board.

1. Extract and verify the package. Replace the filename with your downloaded package; use the E310 package for an E310:

   ```sh
   mkdir bootfiles
   tar -xzf e200-fpv-spectrum-ui-20261120-bootfiles.tar.gz -C bootfiles
   cd bootfiles
   sha256sum -c SHA256SUMS
   ```

2. Copy all extracted files to the **root of the SD card's FAT32 boot partition**.
3. Insert the SD card, select SD boot mode, connect the receive antenna and Ethernet cable, then power on the SDR.
4. Set your computer's Ethernet address to `192.168.1.100/24` and open [http://192.168.1.10/](http://192.168.1.10/). The SSH username and password are both `root`.

## Web interface

- **Scan**: enter one `start-end` range per line in **Frequency ranges** (MHz, up to 8 ranges). Defaults are `2200-2500`, `5100-5400` and `5600-5900`. **Search grid** defaults to 6 MHz. Click **Start scan**, view detections in **Records**, and use **Save BMP** to save an image.
- **Lock**: while scanning, enter a frequency under **Lock → Frequency** and click **Lock frequency**, or select **Lock this frequency** on a detection. You can also change the frequency while locked. **Unlock frequency / Resume scan** returns to scanning; **Stop capture** stops acquisition.
- **Gain**: **RX gain** defaults to 50 dB. Adjust it during scanning or fixed-frequency reception (0–60 dB), then click **Apply**. Stop scanning before changing the scan ranges or search grid.

PAL/NTSC detection is automatic. Scanning combines AD9361 RF tuning with four DDC channels; the default search grid is 6 MHz.

**SPECTRUM** displays a 2048-point FFT of the raw IQ samples (61.44 MSPS, 30 kHz bin spacing). Scanning stitches the center 54 MHz of each RF window; fixed-frequency reception refreshes the spectrum continuously. Gray traces indicate older data. Cyan lines mark band edges; shaded `//` gaps are not scanned and are compressed to leave more space for scanned bands. The vertical axis is in dBFS.

## Host reception

`GET /api/v1/stream` provides BMP grayscale images with frequency, video standard and other metadata. The Python receiver saves images and JSON by frequency:

```sh
python3 tools/fpv_receiver/receive.py --url http://192.168.1.10 --output fpv-recordings
# Lock to a frequency and receive continuously; press Ctrl+C to exit.
python3 tools/fpv_receiver/receive.py --url http://192.168.1.10 --lock-mhz 5800 --gain-db 30
```

`GET /api/spectrum` returns the stitched spectrum, frequency coordinates, receive gain and data age.

## Build the firmware

Use a Linux host with **Vivado / Vitis 2022.2**, including the ARM hard-float toolchain. The default installation path is `/opt/Xilinx`. For other locations, set `VIVADO_SETTINGS_FILE` and `VITIS_SETTINGS_FILE`, and update the toolchain path in `firmware/board/<board>/buildroot/defconfig`.

Install the dependencies on Ubuntu/Debian:

```sh
sudo apt-get install build-essential git cmake python3 patch flex bison libssl-dev \
  device-tree-compiler cpio unzip rsync file bc wget util-linux libncurses-dev
```

Run these commands from the repository root. The first build downloads the pinned source dependencies:

```sh
make -C firmware fetch-locked
make -C firmware BOARD=e200 sd NPROC=4 BUILDROOT_JOBS=4
make -C firmware BOARD=e310 sd NPROC=4 BUILDROOT_JOBS=4
```

Boot files are generated in `firmware/build/<board>/sd/`. Copy them to the root of the SD card's FAT32 boot partition. To create boot archives:

```sh
python3 firmware/scripts/package_test_sd.py --board e200 --name e200-fpv-local
python3 firmware/scripts/package_test_sd.py --board e310 --name e310-fpv-local
```

Archives are generated in `firmware/build/<board>/images/`.

FPGA hardware is provided as an XSA and checksum manifest in `firmware/board/<board>/hardware/`. FPGA source code is not required to build the complete firmware. Third-party sources retain their original licenses; the [Microphase logo](https://antsdr-docs.microphase.cn/en/latest/_images/microphase.png) belongs to its respective copyright holder.
