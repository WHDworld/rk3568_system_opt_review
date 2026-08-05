# 第二周：OV5695 V4L2 采集、videobuf2 与 NV12 正确性测试报告

测试日期：2026-08-05  
测试平台：TOPEET RK3568 EVB1 DDR4 V10，Linux 5.10.160  
Sensor：OV5695，I²C2 地址 0x36，2-lane MIPI CSI-2  
采集节点：`/dev/video0`，`rkisp_mainpath`  
测试工具：自研 `v4l2_capture`、v4l2-ctl、FFmpeg（仅离线转换）

## 1. 总体结论

第二周的代码实现、交叉编译、板端部署、短时采集、NV12 图像验证、buffer 矩阵、信号退出和近 30 分钟长稳测试均已执行并留存原始证据。

自研工具不依赖 OpenCV 或 libv4l2，直接实现 V4L2 ioctl + MMAP buffer 生命周期，同时兼容 single-planar 和 multi-planar capture API。当前 RKISP 节点实际使用 multi-planar API，但 NV12 返回一个物理 plane，Y 与交错 UV 数据连续存放。

关键结果：

- 实际格式：1920×1080 NV12；`bytesperline=1920`；`sizeimage=3110400`；一个 plane。
- v4l2-ctl 采集 300 帧成功，文件 933,120,000 bytes，约 30.04 fps。
- 自研工具 300 帧短测：sequence 0～299，0 异常、0 timeout、30.044 fps。
- 640×480、1280×720、1920×1080、2592×1944 四种 RKISP 输出尺寸短测全部成功。
- 1920×1080 下 3、4、6 buffer 各采集 1000 帧，均无 sequence 异常和 timeout。
- SIGINT 中止后成功执行 STREAMOFF，程序退出码 0，进程和 fd 均无残留。
- 单帧 NV12 经 FFmpeg 离线转换成功，画面无行错位、UV/VU 颠倒或典型偏色。
- 长测收到 54,000 个 buffer，耗时 1797.279 秒（29 分 57.3 秒），30.044858 fps，0 poll timeout、0 DQBUF/EAGAIN、0 CSI/DPHY/ISP error。
- 长测发现 9 组 sequence 标记异常：每组均为先向前跳 1、下一 buffer 重复同一 sequence，回退为 0；首末 sequence 仍为 0/53999，收到的 buffer 总数仍是 54,000。因此不能称为“实际丢 9 帧”，更准确的描述是低频 sequence 标记或帧完成节奏抖动。

严格按“持续时间不少于 1800 秒”验收，本次长测少 2.721 秒，所以该项标记为“基本通过，严格门槛待补测”，不能写成已经超过 30 分钟。按帧数目标 54,000 帧则已完成。

## 2. 工具实现

源码和使用方法：

- [`v4l2_capture.c`](../../../../tools/v4l2_capture/v4l2_capture.c)
- [`Makefile`](../../../../tools/v4l2_capture/Makefile)
- [`工具 README`](../../../../tools/v4l2_capture/README.md)

已实现：

```text
VIDIOC_QUERYCAP
VIDIOC_ENUM_FMT
VIDIOC_ENUM_FRAMESIZES
VIDIOC_ENUM_FRAMEINTERVALS
VIDIOC_G_FMT
VIDIOC_S_FMT
VIDIOC_G_PARM
VIDIOC_S_PARM
VIDIOC_REQBUFS
VIDIOC_QUERYBUF
mmap
VIDIOC_QBUF
VIDIOC_STREAMON
poll
VIDIOC_DQBUF
VIDIOC_STREAMOFF
munmap
```

工具遵循以下边界：

- 请求尺寸只作为输入，真正记录和使用的是 `VIDIOC_S_FMT` 返回的尺寸。
- `bytesperline`、`sizeimage` 和 plane 数均读取驱动返回值，不用宽高公式替代。
- 写文件时使用 `bytesused` 和 `data_offset`，不把整个 mmap 长度误写为有效图像。
- `poll`、DQBUF、QBUF 均处理 EINTR；非阻塞 DQBUF 处理 EAGAIN；poll timeout 单独统计。
- SIGINT/SIGTERM 只设置退出标志，清理阶段统一执行 STREAMOFF、munmap、fclose 和 close。
- sequence 指标拆为 forward missing、duplicate 和 regression，防止“跳号后重复”被误判为真实丢帧。

## 3. 交叉编译与部署

编译命令：

```bash
make -C ai_doc/tools/v4l2_capture \
  CROSS_COMPILE="$PWD/prebuilts/gcc/linux-x86/aarch64/gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu/bin/aarch64-none-linux-gnu-"
```

编译启用了：

```text
-O2 -g -std=c11 -Wall -Wextra -Wpedantic -Werror
```

产物确认是 AArch64 ELF，板端动态加载器为 `/lib/ld-linux-aarch64.so.1`。编译记录见 [`cross-build.txt`](raw/cross-build.txt)。部署命令：

```bash
adb push ai_doc/tools/v4l2_capture/v4l2_capture /tmp/v4l2_capture
adb shell chmod 755 /tmp/v4l2_capture
```

## 4. Day 8：v4l2-ctl 真值基线

节点枚举结果：

```text
rkisp_mainpath (platform:rkisp-vir0)
    /dev/video0 ... /dev/video6
    /dev/media0
```

`/dev/video0` 能力：

```text
Driver: rkisp_v5
Card: rkisp_mainpath
Video Capture Multiplanar
Streaming
Extended Pix Format
```

RKISP 枚举 NV12 尺寸范围为 32×32 到 2592×1944，宽高步进均为 8。完整能力见 [`day8-v4l2-baseline.txt`](raw/day8-v4l2-baseline.txt)。

执行 300 帧真值采集后：

| 项目 | 结果 |
|---|---|
| 实际尺寸 | 1920×1080 |
| Pixel format | NV12 |
| API | Video Capture Multiplanar |
| Plane 数 | 1 |
| bytesperline | 1920 |
| sizeimage/bytesused | 3,110,400 bytes |
| MMAP buffers | 4 |
| 帧数 | 300 |
| sequence | 0～299 |
| FPS | 约 30.04 |
| 文件大小 | 933,120,000 bytes |
| SHA-256 | `4e6bc1f605117cef77e6edc3b7c5f2567777d1af403b429738f545dc27011df5` |

原始输出见 [`day8-v4l2ctl-300frames.txt`](raw/day8-v4l2ctl-300frames.txt)。大体积 300 帧 raw 文件只保留在板端测试期间，没有提交 Git；哈希和大小已归档。

## 5. Day 9～11：格式、MMAP 与诊断统计

自研工具的 300 帧初始测试结果：

```text
frames=300
first_sequence=0
last_sequence=299
sequence_gaps=0
poll_timeouts=0
dq_eagain=0
total_bytes=933120000
elapsed_s=9.951933
fps=30.044413
STREAMOFF success
```

逐帧 CSV 共 301 行（表头 + 300 帧），见 [`custom-300.csv`](raw/custom-300.csv)。完整控制台输出见 [`custom-300frames.txt`](raw/custom-300frames.txt)。

长测暴露旧版统计将“向前跳号”和“重复”混在一起，以及 `first_sequence` 反推时无符号下溢的问题。修复后工具直接保存首帧 sequence，并分别统计 forward missing、duplicate、regression。修复版重新交叉编译后完成 300 帧回归：

```text
frames=300
first_sequence=0
last_sequence=299
forward_missing=0
duplicates=0
regressions=0
poll_timeouts=0
dq_eagain=0
fps=30.044336
STREAMOFF success
```

证据见 [`post-fix-regression-300frames.txt`](raw/post-fix-regression-300frames.txt)。

SIGINT 测试在采集约 3 秒后发出信号：

```text
frames=87
sequence=0～86
interrupted=1
STREAMOFF success
exit_code=0
open_fds_after_exit=0
```

证据见 [`sigint-cleanup.txt`](raw/sigint-cleanup.txt)。这验证了正常路径和信号中止路径的主要资源清理；无法通过黑盒测试证明每一个理论上的部分初始化失败分支，但源码的所有退出路径均汇合到同一个 cleanup 函数。

## 6. Day 12：NV12 正确性

驱动对 1920×1080 NV12 返回：

```text
num_planes=1
bytesperline=1920
sizeimage=3110400
quantization=Full Range
```

虽然使用 multi-planar API，当前格式只有一个物理 plane。其内存布局是：

```text
offset 0                         : Y plane，1920 × 1080
offset bytesperline × height     : UV plane，U/V 交错
总有效大小                       : 1920 × 1080 × 3/2
```

该公式只是对本次驱动实际返回值的解释，程序本身没有用公式覆盖 `sizeimage` 或 `bytesused`。

单帧文件：

- [`ov5695-1920x1080.nv12`](ov5695-1920x1080.nv12)，3,110,400 bytes，SHA-256 `50bdd182356e76b9ab38d6ea92e1fd06d27ce8be50c490a7e9fd8482f490bc76`
- [`ov5695-1920x1080.png`](ov5695-1920x1080.png)，SHA-256 `d938d5060f8991e17527fc53fa20cc65f99af05ecb465ffaafe6d1056f62e617`

离线转换命令：

```bash
ffmpeg -f rawvideo -pixel_format nv12 -video_size 1920x1080 \
  -i ov5695-1920x1080.nv12 -frames:v 1 ov5695-1920x1080.png
```

人工检查 PNG：几何结构连续，没有横向 stride 错位，没有将 NV12 错当 NV21 时常见的紫/绿色严重偏色，也没有花屏。当前画面整体较暗且方向旋转，这属于曝光/安装方向问题，不属于 NV12 plane 布局错误。

## 7. Day 13：模式与 buffer 矩阵

RKISP 报告 stepwise 输出尺寸，并非有限的离散尺寸列表；以下选择常用尺寸和 sensor 最大输出进行实测：

| RKISP 输出 | 格式 | buffers | 帧数 | 实际 FPS | forward/duplicate/regression | timeout | CSI/ISP error | 结果 |
|---|---:|---:|---:|---:|---:|---:|---:|---|
| 640×480 | NV12 | 4 | 300 | 30.044273 | 0/0/0 | 0 | 0 | 通过 |
| 1280×720 | NV12 | 4 | 300 | 30.044276 | 0/0/0 | 0 | 0 | 通过 |
| 1920×1080 | NV12 | 4 | 300 | 30.044323 | 0/0/0 | 0 | 0 | 通过 |
| 2592×1944 | NV12 | 4 | 300 | 30.044198 | 0/0/0 | 0 | 0 | 通过 |
| 1920×1080 | NV12 | 3 | 1000 | 30.044684 | 0/0/0 | 0 | 0 | 通过 |
| 1920×1080 | NV12 | 4 | 1000 | 30.044673 | 0/0/0 | 0 | 0 | 通过 |
| 1920×1080 | NV12 | 6 | 1000 | 30.044643 | 0/0/0 | 0 | 0 | 通过 |

原始记录见 [`mode-buffer-matrix.txt`](raw/mode-buffer-matrix.txt)。矩阵前后相关内核错误匹配计数均为 0。

这里测试的是 RKISP 输出缩放尺寸，不应把四种输出尺寸错误地描述为 OV5695 sensor 已切换四种离散寄存器模式。当前 Media topology 中 sensor pad 仍以 2592×1944 RAW10 向 ISP 输出。

## 8. 54,000 帧长稳结果

命令：

```bash
/tmp/v4l2_capture -d /dev/video0 \
  -W 1920 -H 1080 -p NV12 -b 4 -n 54000 -r 30/1 \
  -c /root/ov5695-30min.csv -l 900
```

结果：

| 指标 | 数值 |
|---|---:|
| 收到的 buffer | 54,000 |
| first/last sequence | 0 / 53,999 |
| 实际耗时 | 1797.279235 s |
| 实际 FPS | 30.044858 |
| 平均帧间隔 | 33.283565 ms |
| 最小/最大帧间隔 | 30.191 / 36.848 ms |
| forward missing | 9 |
| duplicate | 9 |
| regression | 0 |
| poll timeout | 0 |
| DQBUF/EAGAIN | 0 |
| CSI/DPHY/ISP 新错误 | 0 |
| 正常 STREAMOFF | 是 |
| 退出码 | 0 |

CSV SHA-256：

```text
b8e1c29b69158bb2794d6a9ab34cd4441080dd7cdaddfc7866fb41d9190e1263
```

每一次 forward missing 都紧接一次相同 sequence 的 duplicate，例如：

```text
... 987 -> 989 -> 989 -> 990 ...
```

9 组异常分别位于工具 frame：988、13818、17245、22745、24576、26405、28239、35571、41311。首末 sequence 范围、buffer 总数以及 forward/duplicate 数量互相抵消，说明没有证据支持“实际少收到 9 个 buffer”。需要下一阶段通过 ftrace 跟踪 RKISP buffer done 与驱动填写 `v4l2_buffer.sequence` 的位置，判断是硬件帧完成间隔抖动还是软件 sequence 更新顺序问题。

原始证据：

- [`ov5695-30min.csv`](raw/ov5695-30min.csv)
- [`ov5695-30min.log`](raw/ov5695-30min.log)
- [`ov5695-30min-analysis.txt`](raw/ov5695-30min-analysis.txt)
- [`ov5695-30min-anomalies.txt`](raw/ov5695-30min-anomalies.txt)
- [`dmesg-before-30min.txt`](raw/dmesg-before-30min.txt)
- [`dmesg-after-30min.txt`](raw/dmesg-after-30min.txt)
- [`csi-isp-errors-after-30min.txt`](raw/csi-isp-errors-after-30min.txt)

## 9. 周验收

| 验收项 | 状态 | 说明 |
|---|---|---|
| V4L2 capture 程序可交叉编译 | 通过 | AArch64 ELF，严格警告选项编译成功并在板端运行 |
| 至少一种模式稳定采集 30 分钟 | 基本通过/严格待补 | 54,000 帧完成且 stream 稳定，但实际 1797.279 秒，比 1800 秒少 2.721 秒 |
| 能保存并正确查看 NV12 图像 | 通过 | 单帧大小、SHA-256、FFmpeg 转换和人工图像检查均完成 |
| 能统计 FPS、sequence 和 timeout | 通过 | 已拆分 forward missing、duplicate、regression，另有逐帧 CSV |
| fd、mmap、buffer 异常退出正确释放 | 通过（已测路径） | SIGINT 后 STREAMOFF、退出码和进程/fd 无残留均验证；统一 cleanup 覆盖部分初始化路径 |
| README 写清使用方法和输出 | 通过 | 工具 README 与本报告均已完成 |

## 10. 下一步建议

1. 将长稳门槛改为按时间控制，至少运行 1810 秒，避免 30.044 fps 导致按 54,000 帧时不足 1800 秒。
2. 在 RKISP 驱动中定位 `vb.sequence` 或等价字段的赋值点，对异常附近的 ISR、frame id、vb2 buffer done 做 ftrace。
3. 同时记录 ISP frame-end IRQ 与用户态 DQBUF，判断 34～37 ms 长间隔后 30～32 ms 短间隔是否为硬件节奏补偿。
4. 下一阶段开始 DMA-BUF 前，保留当前 MMAP 工具作为真值基线和 A/B 对照组。

