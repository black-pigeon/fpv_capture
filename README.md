# AntSDR Analog FPV Scanner

面向 **AntSDR E200 和 AntSDR E310** 的模拟 FPV 接收固件，通过板载网页自动识别 PAL/NTSC 并获取灰度图像。

- **扫频识别**：扫描指定频段，显示检测频点、制式和图像。
- **定频接收**：锁定频点，持续获取图像，也可通过网口发送给上位机。

## 下载与启动

预编译启动包将在 [GitHub Releases](https://github.com/black-pigeon/fpv_capture/releases) 提供，选择对应设备的 `e200-fpv-<版本>-bootfiles.tar.gz` 或 `e310-fpv-<版本>-bootfiles.tar.gz`。

1. 解压启动包并校验（E310 替换文件名）：

   ```sh
   mkdir bootfiles
   tar -xzf e200-fpv-20261119-bootfiles.tar.gz -C bootfiles
   cd bootfiles
   sha256sum -c SHA256SUMS
   ```

2. 将解压后的全部文件复制到 SD 卡 **FAT32 启动分区根目录**。
3. 插入 SD 卡，设置 SDR 为 SD 启动，连接接收天线和网线后上电。
4. 电脑网卡设置为 `192.168.1.100/24`，浏览器打开 **http://192.168.1.10/**。SSH 用户名和密码均为 `root`。

## 网页操作

- **扫频**：在 **Frequency ranges** 输入 MHz 范围，例如 `5650-5950`；**Search grid** 默认 6 MHz。点击 **Start scan**，在 **Records** 查看检测结果，点击 **Save BMP** 保存图像。
- **定频**：停止扫描，在 **Lock → Frequency** 输入频点并点击 **Lock frequency**，或在检测结果中点击 **Lock this frequency**。**Unlock frequency / Resume scan** 恢复扫描，**Stop capture** 停止采集。
- **增益**：扫描和定频接收时均可修改 **RX gain**（0–60 dB），点击 **Apply** 生效。频段、搜索步进和手动锁频频率在停止时修改。

PAL/NTSC 自动识别。搜索组合 AD9361 射频调谐与四路 DDC 基带调谐，搜索步进默认 6 MHz。

## 上位机接收

`GET /api/v1/stream` 输出 BMP 灰度图像及频点、制式等信息。Python 接收程序按频点保存图像和 JSON：

```sh
python3 tools/fpv_receiver/receive.py --url http://192.168.1.10 --output fpv-recordings
# 锁定频点并持续接收，Ctrl+C 结束
python3 tools/fpv_receiver/receive.py --url http://192.168.1.10 --lock-mhz 5800 --gain-db 30
```

## 编译固件

使用 Linux 主机和 **Vivado / Vitis 2022.2**（含 ARM hard-float 工具链），默认安装路径为 `/opt/Xilinx`。其他路径需设置 `VIVADO_SETTINGS_FILE`、`VITIS_SETTINGS_FILE`，并修改 `firmware/board/<board>/buildroot/defconfig` 中的工具链路径。

安装依赖（Ubuntu/Debian）：

```sh
sudo apt-get install build-essential git cmake python3 patch flex bison libssl-dev \
  device-tree-compiler cpio unzip rsync file bc wget util-linux libncurses-dev
```

在仓库根目录执行，首次构建会下载锁定版本的依赖源码：

```sh
make -C firmware fetch-locked
make -C firmware BOARD=e200 sd NPROC=4 BUILDROOT_JOBS=4
make -C firmware BOARD=e310 sd NPROC=4 BUILDROOT_JOBS=4
```

启动文件输出到 `firmware/build/<board>/sd/`，复制到 SD 卡 FAT32 启动分区根目录即可。生成压缩包：

```sh
python3 firmware/scripts/package_test_sd.py --board e200 --name e200-fpv-local
python3 firmware/scripts/package_test_sd.py --board e310 --name e310-fpv-local
```

压缩包输出到 `firmware/build/<board>/images/`。

FPGA 仅提供 XSA 和校验清单，位于 `firmware/board/<board>/hardware/`；无需 FPGA 源码即可编译完整固件。第三方源码保留原许可证；[Microphase 标志](https://antsdr-docs.microphase.cn/en/latest/_images/microphase.png)版权归原权利人。
