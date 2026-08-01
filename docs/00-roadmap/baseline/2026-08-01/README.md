# RK3568 + OV5695 项目基线（2026-08-01）

本目录记录项目开始前的硬件、BSP、摄像头、显示和恢复基线。`raw/` 中保存命令原始输出；本文只给出基于现有证据可以确认的结论。没有原理图、模组规格书或实测证据的参数一律标为“待确认”。

## 1. 验收结论

| 任务 | 状态 | 结论或剩余动作 |
|---|---|---|
| 确认板卡型号和 RAM | 已完成 | 设备树 model 为 `TOPEET RK3568 EVB1 DDR4 V10 Board`；Linux `MemTotal` 为 7,853,964 kB，对应标称 8 GiB RAM |
| 确认 OV5695 模组信息 | 部分完成 | 芯片 OV5695、模组字符串 `TongJu`、镜头字符串 `CHT842-MD`、2 lane、24 MHz MCLK 已确认；精确模组料号、FPC pinout 和三路电源电压待确认 |
| 确认 I²C/CSI 连接 | 已完成 | Sensor 位于 I²C2（`i2c@fe5b0000`）地址 `0x36`，Media topology 显示连接 `rockchip-csi2-dphy0`，再进入 RKISP |
| 保存原始 boot/update 镜像 | 已完成 | 已逐字节导出板端 64 MiB boot 分区并校验；SDK 中现有 update.img 已记录路径、时间和 SHA-256 |
| 串口观察完整启动日志 | 部分完成 | 已确认 `earlycon` 和 `console=ttyFIQ0` 生效，内核早期日志会切换到 `ttyFIQ0`；尚缺一次从上电开始、同时包含 BootROM/Loader/U-Boot/Linux 的串口文件记录 |
| 替换内核/DTB | 已完成 | 历史记录证明已完成全量烧写并正常恢复 ADB；本次 `./build.sh kernel` 也成功生成包含 DTB 的 FIT `boot.img` |
| 失败后恢复 | 部分完成 | 当前可启动 boot 已备份，全量 update.img 和 boot 单分区烧写路径均存在；为避免无意义破坏当前可用系统，本次未主动制造失败并执行回滚，仍需做一次受控回滚演练 |
| 创建独立 Git 分支 | 已完成 | BSP 仓库分支 `feature/ov5695-v4l2-dmabuf` 已建立并推送，跟踪同名远端分支 |
| 收集板端基线 | 已完成 | `dmesg`、设备节点、I²C、V4L2、Media Controller、DRM、内存和串口配置均保存在 `raw/` |
| 实际采集验证 | 已完成 | `/dev/video0` 成功采集 60 帧 1920×1080 NV12，序号 0～59 连续，约 30.04 fps，未见新增摄像头错误 |

当前结论是：摄像头 BSP、Media topology 和 V4L2 采集已经具备继续开发的条件。Day 0 尚未完全关闭的两项是“硬件文档确认”和“受控回滚演练”。

## 2. 已确认硬件参数

| 项目 | 已确认值 | 证据 |
|---|---|---|
| SoC/板卡 | RK3568，TOPEET EVB1 DDR4 V10 | [`board-model.txt`](raw/board-model.txt)、[`hardware-summary.txt`](raw/hardware-summary.txt) |
| RAM | 标称 8 GiB；Linux 可见 7,853,964 kB | [`meminfo.txt`](raw/meminfo.txt) |
| Sensor | OV5695，驱动成功读取 chip ID | [`dmesg-camera-media-drm.txt`](raw/dmesg-camera-media-drm.txt) |
| DTS compatible | `ovti,ov5695` | [`ov5695-device-tree-runtime.txt`](raw/ov5695-device-tree-runtime.txt) |
| I²C 控制器 | I²C2，运行时节点 `i2c@fe5b0000` | [`ov5695-device-tree-runtime.txt`](raw/ov5695-device-tree-runtime.txt) |
| I²C 地址 | `0x36` | [`ov5695-device-tree-runtime.txt`](raw/ov5695-device-tree-runtime.txt) |
| MIPI data lanes | 2 lane，编号 1、2 | [`ov5695-device-tree-runtime.txt`](raw/ov5695-device-tree-runtime.txt) |
| CSI 接收端 | `rockchip-csi2-dphy0` | [`media-topology.txt`](raw/media-topology.txt) |
| MCLK | DTS 使用 `CLK_CIF_OUT`；驱动工作频率为 24 MHz | [`ov5695-device-tree-runtime.txt`](raw/ov5695-device-tree-runtime.txt)、[`ov5695-runtime-resources.txt`](raw/ov5695-runtime-resources.txt) |
| Reset | GPIO3_PD4，低有效 | [`ov5695-device-tree-runtime.txt`](raw/ov5695-device-tree-runtime.txt) |
| PWDN | GPIO3_PD5，高有效 | [`ov5695-device-tree-runtime.txt`](raw/ov5695-device-tree-runtime.txt) |
| 模组/镜头字符串 | `TongJu` / `CHT842-MD` | [`ov5695-device-tree-runtime.txt`](raw/ov5695-device-tree-runtime.txt) |
| 显示 | LVDS-1，当前连接，1280×800@60 | [`drm-connectors.txt`](raw/drm-connectors.txt)、[`drm-planes.txt`](raw/drm-planes.txt) |
| CMA | 16 MiB | [`meminfo.txt`](raw/meminfo.txt) |

“DTS 写了某个参数”只能证明当前软件配置，不能单独证明 FPC 实际走线和电源电压正确。不过，本次 sensor chip ID、Media graph 和连续采集均成功，能够证明当前 I²C 与 MIPI 数据链路在这一工作模式下实际可用。

## 3. 明确待确认项

以下信息在当前源码、板端运行信息和本地学习资料中没有足够证据，不能凭 OV5695 裸芯片的典型参数代替模组规格：

- OV5695 模组的精确商品型号或 PCB 版本；`TongJu` 和 `CHT842-MD` 只是 DTS 中的模组/镜头描述字符串。
- FPC 针数、每个 pin 的定义、连接器方向和 pin 1 方向。
- 模组实际 AVDD、DOVDD、DVDD 电压以及是否由模组板载 LDO 转换。
- FPC 上时钟、I²C、reset、pwdn、GND 和各电源脚与 RK3568 板端连接器的逐针对应关系。

板端日志包含 `supply avdd/dovdd/dvdd not found, using dummy regulator`，其含义是当前 DTS 没有为驱动声明可控 regulator，并不等于 sensor 不需要供电，也不能据此推出供电电压。关闭这些待确认项需要索取摄像头模组规格书和 iTOP-RK3568 摄像头接口原理图，再用万用表/示波器核对关键电源与 MCLK。

## 4. 摄像头实际工作验证

Media Controller 显示的主要数据路径为：

```text
ov5695 2-0036
  -> rockchip-csi2-dphy0
  -> rkisp-csi-subdev / rkisp-isp-subdev
  -> rkisp_mainpath
  -> /dev/video0
```

通过 `/dev/video0` 请求 4 个 MMAP buffer，并采集 60 帧 1920×1080 NV12。结果为：

- `VIDIOC_REQBUFS`、`VIDIOC_QBUF`、`VIDIOC_STREAMON` 全部成功；
- 帧序号从 0 连续到 59，没有序号跳变；
- 每帧 `bytesused=3110400`，符合 1920×1080 NV12；
- 帧间隔约 33.3 ms，最终统计约 30.04 fps；
- 采集后相关 `dmesg` 未出现新增错误。

完整结果见 [`v4l2-capture-1920x1080-nv12-60frames.txt`](raw/v4l2-capture-1920x1080-nv12-60frames.txt) 和 [`dmesg-after-capture.txt`](raw/dmesg-after-capture.txt)。这已经超过“只看到 `/dev/video*` 节点”的验证强度。

## 5. 构建、部署与恢复基线

### 5.1 本次构建

在 BSP 分支 `feature/ov5695-v4l2-dmabuf` 执行：

```bash
./build.sh kernel
```

命令成功退出，实际目标是 `topeet-rk3568-linux.img`，生成了：

```text
kernel/arch/arm64/boot/dts/rockchip/topeet-rk3568-linux.dtb
kernel/resource.img
kernel/boot.img
```

本次完整构建输出见 [`kernel-build-2026-08-01.txt`](raw/kernel-build-2026-08-01.txt)，产物哈希见 [`build-artifacts.txt`](raw/build-artifacts.txt)。`boot.img` 是包含 Linux Image 和带板级 DTB 的 `resource.img` 的 FIT 镜像。

### 5.2 已有部署证据

此前项目记录已经完成过以下路径：选择 iTOP RK3568 配置、编译内核/DTB、检查 FIT 中的 FDT、进入 Loader、全量烧写并启动后恢复 ADB。详见 [`RK3568 USB OTG 与 ADB 修复说明`](../../../05-debug-cases/RK3568_USB_OTG与ADB修复说明.md)。因此“这套板卡能够替换内核/DTB并重新启动”已有实际证据，不是只根据脚本推断。

### 5.3 已保存恢复材料

当前板端 boot 分区已导出到 Git 仓库之外：

```text
/home/whd/experiment/rk3568_linux_5.10_20250211/device-backups/
└── topeet-rk3568/2026-08-01/boot-current-emmc.img
```

文件大小为 67,108,864 bytes。板端 `/dev/block/by-name/boot` 与本地文件 SHA-256 均为：

```text
8b285a8bb93f61c8440c4047a44933714a89c00d460ab22cddad8d713aa1a253
```

SDK 中现有全量升级包为 `output/update/Image/update.img`，大小 3,744,008,778 bytes，SHA-256 为 `d50d79baf33d71e849e79ae5a3d0a523f957b36745ae20483cc67d8046d7ef58`。它生成于 2026-07-05，只可作为当时系统的恢复包，不能声称包含本次重新编译的 boot 或之后更新的 rootfs。详情见 [`backup-manifest.txt`](raw/backup-manifest.txt)。

### 5.4 推荐的受控 boot 更新/回滚演练

先保持串口连接并确认 Loader 可枚举，再只烧写 boot 分区：

```bash
adb reboot bootloader
lsusb | grep 2207
sudo ./rkflash.sh boot kernel/boot.img
sudo ./rkflash.sh rd
```

若新 boot 无法启动，重新进入 Loader/MaskROM 后，用同一个脚本把已备份镜像写回 boot：

```bash
sudo ./rkflash.sh boot /home/whd/experiment/rk3568_linux_5.10_20250211/device-backups/topeet-rk3568/2026-08-01/boot-current-emmc.img
sudo ./rkflash.sh rd
```

不要为普通 DTB/内核实验优先执行 `rkflash.sh all`，因为全量烧写会覆盖更多分区和用户数据。正式关闭“回滚验证”前，应在串口在线、供电稳定且 Loader/MaskROM 进入方法已实际掌握的条件下完成一次“新 boot → 启动确认 → 原 boot → 启动确认 → 新 boot”的受控演练，并保存两次完整串口日志。

## 6. 串口基线

板端命令行包含：

```text
earlycon=uart8250,mmio32,0xfe660000 console=ttyFIQ0
```

日志确认 early console 启用后切换到 `ttyFIQ0`，所以 Linux 早期启动日志的输出通道已经配置。证据见 [`serial-console.txt`](raw/serial-console.txt)。但 `dmesg` 只能证明 Linux 内核部分，不能替代从物理 UART 捕获的 BootROM/Loader/U-Boot/Linux 冷启动日志；后者仍需在上述回滚演练时补齐。

## 7. 原始证据索引

- 系统：[`uname.txt`](raw/uname.txt)、[`cmdline.txt`](raw/cmdline.txt)、[`meminfo.txt`](raw/meminfo.txt)、[`iomem.txt`](raw/iomem.txt)
- 内核日志：[`dmesg-baseline.txt`](raw/dmesg-baseline.txt)、[`dmesg-camera-media-drm.txt`](raw/dmesg-camera-media-drm.txt)
- 节点与总线：[`device-nodes.txt`](raw/device-nodes.txt)、[`i2c-buses.txt`](raw/i2c-buses.txt)、[`v4l2-devices.txt`](raw/v4l2-devices.txt)
- 拓扑：[`media-topology.txt`](raw/media-topology.txt)、[`drm-connectors.txt`](raw/drm-connectors.txt)、[`drm-planes.txt`](raw/drm-planes.txt)
- OV5695：[`ov5695-device-tree-runtime.txt`](raw/ov5695-device-tree-runtime.txt)、[`ov5695-runtime-resources.txt`](raw/ov5695-runtime-resources.txt)、[`v4l2-mainpath-capabilities.txt`](raw/v4l2-mainpath-capabilities.txt)
- 备份与构建：[`backup-manifest.txt`](raw/backup-manifest.txt)、[`build-artifacts.txt`](raw/build-artifacts.txt)、[`kernel-build-2026-08-01.txt`](raw/kernel-build-2026-08-01.txt)

## 8. Day 0 关闭条件

继续功能开发不必等待待确认项全部关闭，但在修改电源时序、GPIO 极性、MCLK 或 MIPI lane 配置之前，必须先拿到硬件资料。Day 0 完全关闭还需补充：

1. 模组规格书与板卡摄像头接口原理图照片/文件，并形成逐 pin 对照表。
2. 一份从上电开始的完整串口启动日志。
3. 一次只写 boot 分区的更新与回滚演练记录。

