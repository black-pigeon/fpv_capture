# AntSDR Analog FPV Scanner

用于 **ANTSDR E200 和原版 ANTSDR E310** 的模拟 FPV 接收固件：通过用户态 ADI no-OS 控制 AD9361，在 FPGA 中完成四路 DDC 和 PAL/NTSC 灰度图像解调，使用板载网页操作，无需 libiio。

- **扫频识别**：在指定频段内扫描，自动识别 PAL/NTSC，显示检测频点及图像。
- **定频连续图像获取**：锁定一个频点，持续获取图像，并可通过网口发送给上位机。当前输出为解调后的 BMP 灰度图像帧，**不提供原始 IQ 数据流**。

## 下载与启动

两款设备使用同一主分支，通过 `BOARD=e200` 或 `BOARD=e310` 选择构建目标。预编译启动包将在 [GitHub Releases](https://github.com/black-pigeon/fpv_capture/releases) 中提供；仓库分支仅保存源码和 XSA。

下载时选择对应型号的 `e200-fpv-<版本>-bootfiles.tar.gz` 或 `e310-fpv-<版本>-bootfiles.tar.gz`。尚未发布预编译包时，可按本文末尾的步骤自行构建。

E310 指原版 ANTSDR E310，使用 PS MIO 网口；不适用于 Ettus E310 或 ANTSDR E310v2。两款设备的启动文件不能混用。

1. 下载对应压缩包，解压并验证：

   ```sh
   mkdir bootfiles
   tar -xzf e200-fpv-20261119-bootfiles.tar.gz -C bootfiles  # E310 替换文件名
   cd bootfiles
   sha256sum -c SHA256SUMS
   ```

2. 将解压后的文件直接复制到 SD 卡的 **FAT32 启动分区根目录**，不要套一层目录。它是启动文件包，无需整盘镜像写入工具。主要文件为 `BOOT.BIN`、`e200_fpv.bit`（或 `e310_fpv.bit`）、`zImage`、`devicetree.dtb`、`rootfs.cpio.gz` 和 `uEnv.txt`；同一包内的文件应一起使用。
3. 安全卸载 SD 卡，插入对应 SDR，设置为 SD 启动，连接接收天线、网线并上电。Linux 启动后绿色状态灯使用心跳闪烁。
4. 将电脑网卡设置为 `192.168.1.100/24`，浏览器打开 **http://192.168.1.10/**。需要命令行时，SSH 用户名和密码均为 `root`。

## 网页获取 PAL/NTSC 图像

- **扫频**：在 **Frequency ranges** 填入 MHz 范围，例如 `5650-5950`；可每行一个范围。**Search grid** 默认 6 MHz，点击 **Start scan**。检测结果出现在 **Records**，点击记录查看图像，使用 **Save BMP** 保存。
- **定频**：停止扫描，在 **Lock → Frequency** 输入频点，例如 `5800`，点击 **Lock frequency**；也可从已有结果点击 **Lock this frequency**。页面持续刷新该频点的有效图像，PAL/NTSC 自动识别。**Unlock frequency / Resume scan** 恢复扫频，**Stop capture** 停止采集。
- **增益**：扫描或锁频过程中均可修改 **RX gain**（0–60 dB），点击 **Apply** 生效。频段、搜索步进和手动锁频频率只在停止时编辑。

**Search grid** 是搜索中心的间隔；系统组合 AD9361 物理调谐和四路 DDC 基带调谐完成扫描。疑似有信号但没有图像时，可使用 **Advanced → Fine** 的 2 MHz 步进，并适当调整增益和观察时间。锁频时若信号消失，会留在该频点等待同步恢复。

上位机可通过 `GET /api/v1/stream` 接收图像及频率、制式、通道和序号。仓库内的 Python 接收程序按频点分别保存 BMP 和 JSON：

```sh
python3 tools/fpv_receiver/receive.py --url http://192.168.1.10 --output fpv-recordings
# 定频并持续接收，Ctrl+C 结束接收：
python3 tools/fpv_receiver/receive.py --url http://192.168.1.10 --lock-mhz 5800 --gain-db 30
```

## 编译 SD 卡启动固件

使用 Linux 主机，安装 **Vivado / Vitis 2022.2**（含 ARM hard-float 工具链）。默认路径为 `/opt/Xilinx`；若安装位置不同，设置 `VIVADO_SETTINGS_FILE`、`VITIS_SETTINGS_FILE`，并同步修改相应板卡 `firmware/board/<board>/buildroot/defconfig` 中的工具链路径。

安装主机依赖（Ubuntu/Debian 示例）：

```sh
sudo apt-get install build-essential git cmake python3 patch flex bison libssl-dev \
  device-tree-compiler cpio unzip rsync file bc wget util-linux libncurses-dev
```

在仓库根目录执行，首次构建下载 `firmware/sources.lock.json` 锁定版本的 U-Boot、Linux、Buildroot 等源码：

```sh
make -C firmware fetch-locked
make -C firmware BOARD=e200 sd NPROC=4 BUILDROOT_JOBS=4
make -C firmware BOARD=e310 sd NPROC=4 BUILDROOT_JOBS=4
```

对应输出为 `firmware/build/e200/sd/` 和 `firmware/build/e310/sd/`。将选定板卡输出目录中的文件复制到 SD 卡 FAT32 启动分区根目录，即可按上面的步骤启动。E310 使用隔离的上游源码目录，可与 E200 在同一仓库构建。

需要生成并校验压缩包时执行：

```sh
python3 firmware/scripts/package_test_sd.py --board e200 --name e200-fpv-local
python3 firmware/scripts/package_test_sd.py --board e310 --name e310-fpv-local
```

压缩包输出到 `firmware/build/<board>/images/`。

**FPGA 仅发布 XSA 和校验清单**（`firmware/board/<board>/hardware/`）。构建从 XSA 提取比特流、生成 FSBL 和设备树，并编译 U-Boot、Linux、公开采集驱动、应用及 rootfs；不需要 FPGA 源码。RTL、约束和 Vivado 工程保留在本地。第三方源码保留原许可证；Microphase 标志版权归原权利人，使用的原图来自 https://antsdr-docs.microphase.cn/en/latest/_images/microphase.png 。
