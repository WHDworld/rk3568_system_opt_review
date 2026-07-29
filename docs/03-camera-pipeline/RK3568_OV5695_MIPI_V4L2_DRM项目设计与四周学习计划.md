# RK3568 + OV5695 MIPI：V4L2／RKISP／DMA-BUF／DRM 零拷贝项目设计与四周学习计划

> 面向岗位：Linux BSP、Linux 系统、嵌入式 Linux、Linux 驱动  
> 计划周期：4 周（28 天）  
> 推荐投入：每天 4～6 小时，每周 6 天实作 + 1 天复盘  
> 当前平台：iTOP-RK3568、Rockchip Linux 5.10.160、OV5695 MIPI CSI-2 摄像头  
> 文档性质：执行手册。每天先完成实验，再按本手册定向学习；不要先看完整套课程。

---

## 1. 项目最终目标

项目建议命名为：

> **基于 RK3568 的 OV5695 MIPI Camera BSP 调试与 V4L2–DRM DMA-BUF 零拷贝显示系统**

最终要完成一条可演示、可测量、可解释的链路：

```text
OV5695（RAW10）
    │ MIPI CSI-2，2 lanes
    ▼
RK3568 CSI2 DPHY
    ▼
RKISP（Bayer → NV12，曝光/增益等控制）
    ▼
V4L2 capture node / videobuf2
    │ VIDIOC_EXPBUF
    ▼
DMA-BUF fd
    │ drmPrimeFDToHandle
    ▼
DRM GEM handle / DRM framebuffer
    │ KMS Atomic Commit
    ▼
VOP overlay plane → HDMI/显示器
```

项目不是“调用 OpenCV 打开摄像头”，核心工作包括：

1. 理解并验证 OV5695 从 I²C probe 到 Media Controller 异步绑定的完整过程。
2. 调通 OV5695 → MIPI DPHY → RKISP → V4L2 video node。
3. 编写不依赖 OpenCV/GStreamer 的 V4L2 采集诊断程序。
4. 编写 DRM/KMS 显示程序。
5. 通过 videobuf2 DMA-BUF export 和 DRM PRIME import 完成 NV12 buffer 直接显示。
6. 正确管理采集 DMA 与显示 scanout 之间的 buffer 所有权。
7. 对 copy 与 DMA-BUF 两条路径进行 CPU、延迟、丢帧与内存对比。
8. 结合 CMA、IOMMU、DMA-BUF、mmap 建立多媒体内存模型。
9. 使用 perf、ftrace/trace-cmd 和运行时统计定位瓶颈。
10. 输出 Git patch、实验日志、性能报告、长稳报告和演示视频。

---

## 2. 当前源码基线：哪些已经存在，哪些才是你的工作

### 2.1 已确认的工程事实

当前工作区：

```text
/home/whd/experiment/rk3568_linux_5.10_20250211/rk3568_linux_5.10
```

已经确认：

- 内核版本是 Linux 5.10.160。
- 当前板级目标为 `topeet-rk3568-linux`。
- 内核、DTB、resource.img、boot.img 已经成功构建过。
- `CONFIG_VIDEO_OV5695=y` 已经启用。
- 内核已有 Rockchip 版 OV5695 sensor 驱动：

  ```text
  kernel/drivers/media/i2c/ov5695.c
  ```

- 板级 DTS 已经存在 `ov5695@36`：

  ```text
  kernel/arch/arm64/boot/dts/rockchip/topeet-rk3568-linux.dts
  ```

- 已配置：
  - I²C2；
  - 地址 `0x36`；
  - `CLK_CIF_OUT`；
  - reset/pwdn GPIO；
  - RK3568 VI power domain；
  - 2-lane MIPI；
  - `csi2_dphy0`；
  - `rkisp_vir0`。
- OV5695 驱动识别的芯片 ID 为 `0x005695`，从 `0x300a` 开始读取。
- OV5695 sensor pad 输出固定为：

  ```text
  MEDIA_BUS_FMT_SBGGR10_1X10
  ```

- 驱动 link frequency 为 420 MHz。
- DRM、DMA-BUF、RKCIF、RKISP、RKISPP、CMA 和 DMA CMA 已启用。
- 默认 CMA 配置为 16 MiB。
- SDK 中已有 `VIDIOC_EXPBUF`、`drmPrimeFDToHandle()`、`drmModeAddFB2()` 和
  `drmModeAtomicCommit()` 的参考实现。

### 2.2 重要的真实性边界

当前 BSP 已经带有 OV5695 驱动和对应设备树节点，因此现在不能直接声称：

> “从零完成 OV5695 驱动移植。”

只有实际做过并有证据的内容才能写入简历。项目初始定位应是：

> 基于已有 Rockchip BSP 完成 OV5695 硬件链路验证、板级配置纠错、Media Pipeline
> 调试、用户态 V4L2 采集、DMA-BUF/DRM 零拷贝显示及性能优化。

如果上板后发现 DTS 的 GPIO、lane、clock、供电、sensor mode 或驱动时序不适配你的模组，
并且你确实修改、验证了它们，届时可以据实写“完成板级移植与适配”。

### 2.3 当前 DTS 的风险点

同一 DTS 中 OV5695 和 OV13850 都为 `status = "okay"`，并且：

- 共享 I²C2；
- 共享 reset/pwdn GPIO；
- 都接入 `csi2_dphy0`；
- 分别声明 2 lane 和 4 lane endpoint。

实际只连接 OV5695 时，应检查是否需要禁用 OV13850。否则可能出现：

- 无意义的 I²C probe error；
- GPIO 被两个 sensor 驱动重复控制；
- async subdev 绑定等待；
- media graph 中出现不需要的实体；
- pipeline 选择混乱。

是否禁用必须以上板日志和硬件原理图为依据。

### 2.4 OV5695 与原 OV5640 方案的关键差异

OV5695 当前驱动输出 RAW10 Bayer，不是 YUYV/UYVY。因此推荐数据路径是：

```text
OV5695 RAW10 → RKISP → NV12 → V4L2 capture → DMA-BUF → DRM NV12 plane
```

不能把 sensor 的 RAW10 buffer 直接当成 NV12/YUYV 送给 DRM；DRM/VOP 只负责 scanout，
不会替你完成 Bayer 去马赛克、白平衡和颜色转换。

---

## 3. 一个月的范围控制

### 3.1 必须完成的 MVP

- 能可靠编译、更新、回滚内核和 DTB。
- OV5695 I²C probe 成功。
- Media Controller 拓扑完整。
- 至少完成 1080P 或驱动支持模式中的一个稳定 V4L2 NV12 采集。
- 自己实现 V4L2 MMAP 采集工具。
- 自己实现 DRM/KMS test-pattern 显示。
- 实现 CPU copy 显示基线。
- 实现 V4L2 buffer DMA-BUF export → DRM PRIME import。
- 正确实现 buffer 生命周期，持续预览不少于 30 分钟。
- 完成 copy 与 DMA-BUF 的性能对比。
- 完成至少 2 小时长稳测试。
- 输出 README、patch、原始日志、性能 CSV 和演示视频。

### 3.2 有余力再做

- 多分辨率运行时切换。
- DRM atomic out-fence 或 page-flip 完整同步。
- RGA 格式转换 fallback。
- CMA 参数矩阵和分配失败复现实验。
- LED-to-display 真实光子延迟。
- 程序异常恢复、显示热插拔。
- systemd 启动服务。
- 自动生成性能图表。

### 3.3 本月不主动扩张

- 重写完整 OV5695 sensor 驱动。
- 修改 RKISP 核心算法。
- 自研 AE/AWB/AF。
- 专业 IQ 标定。
- NPU 目标检测。
- H.264 编码和网络推流。
- Qt GUI。
- 多路摄像头同步。
- 同时迁移 RK3588。

---

## 4. 每日工作方法

每天按以下顺序执行：

1. 写下当天唯一的可验证目标。
2. 先上板获取现象，不凭感觉改代码。
3. 提出一个可证伪的假设。
4. 只修改一个变量。
5. 编译、部署、采集日志。
6. 结论成立后提交 Git。
7. 再看与当天问题直接相关的视频。
8. 用自己的话回答当天的面试问题。

推荐时间分配：

| 内容 | 比例 |
|---|---:|
| 上板实验与编程 | 60% |
| 阅读当前调用路径源码 | 20% |
| 定向课程 | 15% |
| 日志、Git、文档 | 5% |

每个 Git commit 只表达一个意图，例如：

```text
docs: collect rk3568 camera baseline
dts: disable unused ov13850 sensor
media: add v4l2 mmap capture utility
drm: add atomic nv12 display backend
pipeline: import v4l2 buffers through drm prime
tools: add copy-vs-dmabuf benchmark
```

---

# 5. 第零阶段：Day 0～1，硬件确认与可回滚基线

## 5.1 实作任务

- [ ] 确认板卡准确型号和 RAM 容量。
- [ ] 确认 OV5695 模组型号、FPC pinout、lane 数、供电电压和时钟要求。
- [ ] 确认当前摄像头是否接在 DTS 描述的 I²C2 和 CSI2 DPHY0。
- [ ] 保存可启动的原始 boot/update 镜像。
- [ ] 确认串口可观察完整启动日志。
- [ ] 验证能替换内核/DTB，失败后能恢复。
- [ ] 创建独立 Git 分支。
- [ ] 收集系统基线并保存在 `docs/baseline/`。

板端基线命令：

```bash
uname -a
cat /proc/device-tree/model; echo
cat /proc/cmdline
cat /proc/meminfo
cat /proc/iomem
dmesg > dmesg-baseline.txt
dmesg | grep -Ei 'ov5695|camera|i2c|csi|dphy|isp|cif|iommu|cma|drm|vop'
ls -l /dev/video* /dev/media* /dev/v4l-subdev* /dev/dri/*
i2cdetect -l
v4l2-ctl --list-devices
for dev in /dev/media*; do media-ctl -p -d "$dev"; done
modetest -c
modetest -p
```

## 5.2 验收门槛

- 能编译、部署和回滚。
- 有原始 `dmesg`、设备节点、media topology 和 DRM topology。
- 能指出摄像头 I²C 地址、lane 数、MCLK、reset/pwdn GPIO。
- 尚未确认硬件的信息明确标成“待确认”，不擅自填写。

## 5.3 定向学习

来自驱动视频：

- `1.驱动视频简介（一定要看）`
- `2.什么是Linux驱动？`
- `4.Linux下驱动模块编译讲解`
- `5.Linux下编译驱动模块实践`
- `6.make menuconfig图形化配置`
- `7.Linux下把驱动编译进内核`

来自 V4L2 项目实战：

- `01.html：第1章 V4L2框架概述`
- `02.html：第2章 摄像头硬件基础`
- `03.html：第3章 开发环境搭建`

来自奔跑吧社区：

- `459.git入门与实战1：建立本地的git仓库`
- `460.git入门与实战2：快速入门`
- `461.git入门与实战3：分支管理`
- `463.git入门和实战5：提交更改`
- `465.git入门和实战7：内核开发和实战`

## 5.4 必须掌握的知识点

- 内核镜像、DTB、resource.img、boot.img 分别是什么。
- built-in driver 与 kernel module 的区别。
- Kconfig、defconfig、`.config`、Makefile 的关系。
- 交叉编译器前缀、ARCH、CROSS_COMPILE 的意义。
- 如何确认板子实际运行的是刚编译的内核和 DTB。
- 为什么驱动调试的第一前提是可回滚。

## 5.5 面试自测

1. `CONFIG_VIDEO_OV5695=y` 和 `=m` 有什么区别？
2. 修改 DTS 后为什么不一定需要重新编译整个 rootfs？
3. 如何证明设备运行的是你刚刚编译的 DTB？
4. 驱动 probe 失败导致板子起不来时如何回滚？
5. BSP 工程和主线 Linux 内核工程有什么区别？

---

# 6. 第一周：OV5695 probe、设备树与 Media Pipeline

## 本周目标

建立并解释：

```text
OV5695 → csi2_dphy0 → rkisp_vir0 → V4L2 video node
```

本周结束时不追求实时显示，只追求 sensor、拓扑和第一帧。

## Day 1：读当前 BSP，而不是立即修改

### 实作

- [ ] 阅读 OV5695 DTS 节点。
- [ ] 阅读 `ov5695_probe()`、power on/off、chip ID、stream on/off。
- [ ] 找到 `of_device_id` 和 I²C driver 注册入口。
- [ ] 找出 sensor 的 supported modes、pixel rate、link frequency、mbus code。
- [ ] 画出 probe 调用与资源依赖图。

重点源码：

```text
kernel/drivers/media/i2c/ov5695.c
kernel/arch/arm64/boot/dts/rockchip/topeet-rk3568-linux.dts
kernel/drivers/media/platform/rockchip/
```

### 验收

能不用照稿解释：

```text
DTS compatible
→ I²C core match
→ ov5695_probe
→ clk/regulator/GPIO
→ chip ID
→ v4l2_subdev registration
→ async notifier bind
```

## Day 2：I²C、clock、GPIO、供电验证

### 实作

- [ ] 用 `i2cdetect` 和 dmesg 确认 `0x36`。
- [ ] 确认 MCLK 是否输出以及频率。
- [ ] 对照原理图检查 reset/pwdn GPIO 和有效电平。
- [ ] 检查 DTS supply 是否缺失、是否使用 dummy regulator。
- [ ] 记录 probe 成功或失败的完整日志。

排错顺序必须是：

```text
电源 → MCLK → reset/pwdn → I²C ACK → chip ID → subdev 注册
```

I²C 没有 ACK 时，不去调 MIPI timing。

## Day 3：清理双 sensor 冲突

### 实作

- [ ] 检查 OV13850 未连接时的 probe 日志。
- [ ] 判断是否需要将 OV13850 改为 `status = "disabled"`。
- [ ] 确认 OV5695 endpoint 和 DPHY endpoint 双向引用。
- [ ] 确认两端 `data-lanes = <1 2>` 一致。
- [ ] 生成单独 DTS commit。

## Day 4：Media Controller 拓扑

### 实作

- [ ] 保存所有 `/dev/mediaX` 的 `media-ctl -p`。
- [ ] 区分 media entity、pad、link、subdev、video node。
- [ ] 找到 OV5695 实际属于哪个 media device。
- [ ] 找到 RKISP mainpath/selfpath 对应的 video node。
- [ ] 记录 immutable/enabled link。

## Day 5：格式传播与 stream-on

### 实作

- [ ] 枚举 sensor subdev 的 RAW10 format。
- [ ] 枚举 capture video node 的输出格式。
- [ ] 确认 RKISP 输出是否包含 NV12。
- [ ] 使用 `media-ctl`/`v4l2-ctl` 设置格式。
- [ ] 尝试抓取第一批帧。
- [ ] 同时观察 CSI、DPHY、ISP error。

## Day 6：故障注入与复盘

在可以恢复的前提下做一次故障注入：

- 暂时写错一个 endpoint；
- 或禁用 sensor；
- 或修改错误 I²C 地址；

观察并记录：

- probe 是否发生；
- async notifier 报什么；
- media entity 是否出现；
- `/dev/videoX` 是否仍然存在；
- STREAMON 在哪一步失败。

实验后立即恢复正确配置。

## Day 7：周验收

本周交付：

- [ ] DTS diff/patch。
- [ ] probe 成功日志。
- [ ] chip ID 日志。
- [ ] Media Controller 拓扑。
- [ ] RAW10 → RKISP → NV12 格式传播说明。
- [ ] 第一帧或明确的阻塞原因。
- [ ] 第一周问题复盘。

### 第一周定向学习

#### 驱动视频：必看

- `18.平台总线模型介绍`
- `20.注册platform驱动`
- `21.平台总线probe函数编写`
- `22.平台总线模型总结和回顾`
- `23.设备树的由来以及基本概念`
- `24.设备树基本语法`
- `25.在设备树中添加自定义节点`
- `26.设备树中常用的of操作函数`
- `27.设备树下的platform总线`
- `28.pinctl和gpio子系统（一）`
- `29.pinctl和gpio子系统（二）`
- `30.pinctl和gpio子系统（三）`
- `43.应用层实现I2C通信`
- `44.I2C总线实现client设备`
- `45.I2C总线实现driver驱动`
- `46.驱动程序实现I2C通信`

说明：OV5695 是 I²C client 驱动，而 RKISP/DPHY 多为 platform device。平台总线视频用于理解
RKISP 等 SoC 设备；I²C 视频用于理解 sensor。

#### V4L2 项目实战：必读

- `01.html：V4L2框架概述`
- `02.html：摄像头硬件基础`
- `04.html：V4L2核心数据结构`
- `05.html：V4L2设备注册与注销`
- `11.html：I2C子设备驱动`
- `12.html：传感器驱动核心`
- `13.html：V4L2子设备操作集`
- `14.html：媒体控制器框架`
- `15.html：设备树绑定`
- `19.html：电源管理`
- `24.html：MIPI CSI接收器驱动`
- `25.html：ISP处理管线`

#### 第一周知识点

- Linux device/driver/bus 模型。
- I²C client 与 I²C driver 的匹配。
- `compatible`、`of_match_table`、`i2c_device_id`。
- probe、remove、runtime PM。
- `-EPROBE_DEFER` 的含义。
- pinctrl、GPIO descriptor、clock、regulator。
- MIPI CSI-2 lane、lane rate、virtual channel、data type。
- RAW10 如何在 CSI-2 包中传输。
- sensor subdev 为什么通常不产生 `/dev/videoX`。
- V4L2 subdev、media entity、pad、link。
- async notifier 和 endpoint graph。
- RKISP 为什么需要接收 Bayer、输出 NV12。

#### 第一周面试自测

1. OV5695 明明 probe 成功，为什么还可能没有可用的 capture node？
2. `remote-endpoint` 为什么需要双向引用？
3. `data-lanes = <1 2>` 表示什么？
4. MIPI lane 数和 link frequency 如何限制最大带宽？
5. RAW10 的每像素有效位数和内存存储位数一定相等吗？
6. `SBGGR10` 中的 BGGR 是什么意思？
7. sensor、CSI DPHY、ISP 和 capture node 分别负责什么？
8. `-EPROBE_DEFER` 和真正 probe failure 如何区分？
9. reset GPIO 的 active low 是逻辑极性还是寄存器电平？
10. 为什么两个 sensor 共享 reset GPIO 可能产生问题？
11. 设备树描述硬件还是执行驱动逻辑？
12. 如何从 chip ID 失败判断供电、时钟、I²C 或 GPIO 问题？

---

# 7. 第二周：V4L2 采集程序、videobuf2 与图像正确性

## 本周目标

不依赖 OpenCV 编写一个可诊断的 V4L2 采集工具，稳定抓取 RKISP 输出的 NV12 图像，
理解每一个 ioctl 和 buffer 状态变化。

## Day 8：用现成工具建立真值基线

```bash
v4l2-ctl --list-devices
v4l2-ctl -d /dev/videoX --all
v4l2-ctl -d /dev/videoX --list-formats-ext
v4l2-ctl -d /dev/videoX \
  --set-fmt-video=width=1920,height=1080,pixelformat=NV12 \
  --stream-mmap=4 \
  --stream-count=300 \
  --stream-to=ov5695-1080p.nv12
```

实际 video node、分辨率和格式以枚举结果为准。

记录：

- 实际 width/height；
- pixel format；
- `bytesperline`；
- `sizeimage`；
- fps；
- buffer 数；
- frame sequence；
- CSI/ISP error；
- 文件大小和 SHA256。

## Day 9：实现能力枚举和格式设置

实现：

- `VIDIOC_QUERYCAP`
- `VIDIOC_ENUM_FMT`
- `VIDIOC_ENUM_FRAMESIZES`
- `VIDIOC_ENUM_FRAMEINTERVALS`
- `VIDIOC_G_FMT`
- `VIDIOC_S_FMT`
- `VIDIOC_G_PARM`
- `VIDIOC_S_PARM`

程序绝不能假设：

```text
bytesperline == width
sizeimage == width × height × 1.5
驱动一定接受请求的 width/height
```

必须读取 `S_FMT` 返回的实际值。

## Day 10：实现 MMAP buffer

实现：

- `VIDIOC_REQBUFS`
- `VIDIOC_QUERYBUF`
- `mmap`
- `VIDIOC_QBUF`
- `VIDIOC_STREAMON`
- `poll`
- `VIDIOC_DQBUF`
- `VIDIOC_STREAMOFF`
- `munmap`

## Day 11：加入统计与错误处理

每帧打印或保存：

```text
frame number
V4L2 sequence
buffer index
driver timestamp
DQBUF monotonic timestamp
bytesused
inter-frame interval
dropped sequence count
```

处理：

- `EINTR`
- `EAGAIN`
- poll timeout
- STREAMON failure
- 中途 SIGINT
- partial initialization cleanup

## Day 12：NV12 正确性

验证：

- Y plane 和 UV plane 的布局；
- single-planar 与 multi-planar API；
- pitch/stride；
- offset；
- 色彩范围；
- UV/VU 顺序；
- 行错位、偏色、花屏。

使用 ffmpeg 仅作为离线验证工具，不作为项目核心：

```bash
ffmpeg -f rawvideo -pixel_format nv12 -video_size 1920x1080 \
  -i ov5695-1080p.nv12 -frames:v 1 frame.png
```

## Day 13：模式矩阵

对驱动实际支持的模式逐项测试：

| Sensor/RKISP 模式 | 输出格式 | buffers | 帧数 | 实际 FPS | 丢帧 | CSI error | 结果 |
|---|---|---:|---:|---:|---:|---:|---|
| 待枚举 | NV12 | 3 | 1000 |  |  |  |  |
| 待枚举 | NV12 | 4 | 1000 |  |  |  |  |
| 待枚举 | NV12 | 6 | 1000 |  |  |  |  |

## Day 14：周验收

- [ ] V4L2 capture 程序可交叉编译。
- [ ] 至少一种模式稳定采集 30 分钟。
- [ ] 能保存并正确查看 NV12 图像。
- [ ] 能统计 fps、sequence gap 和 timeout。
- [ ] 所有 fd、mmap 和 buffer 在异常退出时正确释放。
- [ ] README 写清使用方法和输出。

### 第二周定向学习

#### 驱动视频

- `10.应用层和内核层数据传输`
- `11.linux物理地址到虚拟地址映射`
- `31.ioctl接口（一）`
- `32.ioctl接口（二）`
- `33.中断基础概念`
- `36.中断下文之tasklet`
- `37.等待队列`
- `38.工作队列`

其中 tasklet 只需理解传统下半部概念，不要求在本项目中使用。

#### V4L2 项目实战

- `06.html：V4L2 ioctl框架`
- `07.html：视频采集缓冲区管理`
- `08.html：帧格式与裁剪`
- `09.html：流控制`
- `10.html：中断处理与数据流`
- `16.html：DMA引擎集成`
- `17.html：帧同步与VSYNC`
- `18.html：控制框架`
- `20.html：调试与日志`
- `26.html：用户空间测试`

#### 奔跑吧社区内存课

本周只看：

- `05～08：Linux内存管理概述`
- `16～17：内存布局`
- `18～20：页面分配机制`
- `23：vmalloc分配`
- `24：VMA操作`
- `27：mmap分析`
- `30：page数据结构`

#### 第二周知识点

- ioctl 编码和用户态/内核态参数传递。
- V4L2 streaming I/O 的状态机。
- MMAP、USERPTR、DMABUF 三种 memory type。
- `REQBUFS`、`QUERYBUF`、`QBUF`、`DQBUF` 的职责。
- videobuf2 core、queue、buffer、plane、mem_ops。
- `poll()` 为什么会被唤醒。
- sensor 帧中断、ISP DMA 完成与 `vb2_buffer_done()`。
- single-planar 和 multi-planar API。
- NV12 的 plane、stride、offset、sizeimage。
- V4L2 timestamp、sequence 与 userspace timestamp。
- mmap 只建立映射，不等于复制数据。

#### 第二周面试自测

1. 为什么必须先 QBUF 再 STREAMON？
2. DQBUF 得到的是新分配的内存吗？
3. MMAP 采集发生了几次 CPU memcpy？
4. V4L2 MMAP 为什么不等于 V4L2→DRM 零拷贝？
5. `bytesused`、`sizeimage`、`bytesperline` 有什么区别？
6. `poll()` 返回可读后为什么仍要处理 `EAGAIN`？
7. vb2 queue 的 `mem_ops` 和 driver queue ops 分别负责什么？
8. 驱动如何把 DMA 完成的 buffer 交还用户态？
9. NV12 为什么是 1.5 bytes/pixel？stride 对总大小有什么影响？
10. sequence gap 能说明什么，不能说明什么？
11. V4L2 buffer timestamp 代表曝光、DMA 完成还是 DQBUF 时刻？
12. multi-planar API 的 plane 与 NV12 图像 plane 是否总是一一对应？

---

# 8. 第三周：DRM/KMS、DMA-BUF 与零拷贝显示

## 本周目标

先独立完成 DRM 显示，再把 V4L2 capture buffer 作为 DMA-BUF 导入 DRM。禁止一开始把
摄像头、ISP、DMA-BUF、DRM 四个问题混在一起调。

## Day 15：DRM topology 和 test pattern

使用 `modetest`：

```bash
modetest -c
modetest -p
modetest -e
```

找到：

- `/dev/dri/cardX`；
- connector；
- encoder；
- CRTC；
- primary/overlay plane；
- plane 支持的 pixel formats；
- NV12 是否支持；
- plane 的 possible CRTCs。

编写 test pattern 程序：

- 打开 DRM device；
- 获取 resources；
- 选择 connected connector；
- 选择 CRTC；
- 创建 dumb buffer；
- `drmModeAddFB2()`；
- 显示纯色或色条；
- 正确释放 framebuffer 和 GEM。

## Day 16：Atomic KMS

实现或理解：

- `DRM_CLIENT_CAP_UNIVERSAL_PLANES`
- `DRM_CLIENT_CAP_ATOMIC`
- object properties；
- mode blob；
- CRTC/connector/plane 属性；
- `drmModeAtomicCommit()`；
- page-flip event。

## Day 17：CPU copy 基线

实现基线后端：

```text
V4L2 DQBUF → CPU copy/必要的格式转换 → DRM buffer → KMS
```

如果 V4L2 和 DRM 都支持 NV12，优先做同格式 memcpy，对比才公平。若必须做 CPU
颜色转换，报告中要将“复制 + 转换”成本单独说明。

## Day 18：一次性 DMA-BUF export/import

初始化时对每个 V4L2 buffer 执行一次：

```text
VIDIOC_EXPBUF
→ 得到 dma-buf fd
→ drmPrimeFDToHandle()
→ 得到 GEM handle
→ drmModeAddFB2()
→ 得到 framebuffer ID
```

禁止每帧重复 export、import 和创建 framebuffer。

## Day 19：buffer 生命周期

实现明确状态机：

```text
FREE
  │ QBUF
  ▼
CAPTURE_QUEUED
  │ camera/ISP DMA done + DQBUF
  ▼
CAPTURE_DONE
  │ atomic commit
  ▼
DISPLAY_PENDING
  │ page-flip / fence complete
  ▼
DISPLAY_RELEASED
  │ QBUF
  └──────────────► CAPTURE_QUEUED
```

关键规则：

> DRM 仍在扫描某 buffer 时，不能把它 QBUF 回 V4L2 让 ISP DMA 重写。

否则会出现 tearing、局部花屏、闪屏或偶发旧帧。

## Day 20：NV12 framebuffer 参数

逐项验证：

- DRM format 是 `DRM_FORMAT_NV12`；
- Y 和 UV plane 的 handle；
- pitches；
- offsets；
- modifier；
- width/height；
- V4L2 single-planar/multi-planar 对 DRM 两个图像 plane 的映射；
- 同一个 dma-buf 是否承载两个 offset plane；
- VOP plane 的对齐限制。

如果 DRM plane 不支持当前格式：

1. 首选 RKISP 输出 DRM 支持的 NV12；
2. 其次选择另一个 overlay plane；
3. 保底使用 DMA-BUF + RGA → DRM；
4. 明确区分“直显”和“RGA 硬件转换”，不要都称为同一 buffer 零拷贝。

## Day 21：周验收

- [ ] DRM test pattern 稳定显示。
- [ ] 能打印 connector/CRTC/plane/property 选择过程。
- [ ] copy backend 可工作。
- [ ] DMA-BUF backend 可工作。
- [ ] export/import/framebuffer 只在初始化发生。
- [ ] 热路径没有 `memcpy()`/`memmove()`。
- [ ] 正确等待 page-flip/fence 后再 QBUF。
- [ ] 连续预览 30 分钟，无 fd 持续增长。
- [ ] SIGINT 后恢复显示或至少完整释放资源。

### 第三周定向学习

#### V4L2 项目实战

- 复习 `07.html：视频采集缓冲区管理`
- 复习 `09.html：流控制`
- 复习 `16.html：DMA引擎集成`
- `17.html：帧同步与VSYNC`
- `21.html：性能优化`
- `25.html：ISP处理管线`
- `26.html：用户空间测试`

说明：给定 V4L2 课程目录没有独立 DRM/KMS 和 DMA-BUF PRIME 专章。本部分必须结合源码学习：

```text
external/camera_engine_rkaiq/rkisp_demo/demo/rkdrm_display.c
external/camera_engine_rkaiq/rkisp_demo/demo/rkisp_demo.cpp
kernel/drivers/media/common/videobuf2/
kernel/drivers/gpu/drm/
kernel/drivers/gpu/drm/rockchip/
```

#### 奔跑吧社区内存课

- `12～15：页表映射`
- `23：vmalloc分配`
- `24：VMA操作`
- `27：mmap分析`
- `30：page数据结构`
- `39：页面迁移`
- `40：内存规整`
- `42～43：内存管理框架导读`
- `49：分配物理页面`
- `60～62：初学者对内存管理的常见疑惑`

#### 第三周知识点

- 物理地址、内核虚拟地址、用户虚拟地址、DMA address、IOVA。
- cache coherent 与 non-coherent DMA。
- scatter-gather 和 IOMMU。
- DMA-BUF exporter、importer、attachment、sg_table。
- fd 只是引用，不是内存本身。
- V4L2 buffer index、dma-buf fd、GEM handle、FB ID 的关系。
- DRM connector、encoder、CRTC、plane、framebuffer。
- primary plane 与 overlay plane。
- legacy modeset 与 atomic modeset。
- page flip、vblank、fence。
- pitch、offset、modifier。
- 为什么“可以 import”不代表“可以 scanout”。
- 零 CPU memcpy、零硬件复制和零延迟是三个不同概念。

#### 第三周面试自测

1. DMA-BUF 解决的核心问题是什么？
2. dma-buf fd 能否直接当物理地址使用？
3. exporter 和 importer 分别负责什么？
4. `drmPrimeFDToHandle()` 返回的 handle 是全局的吗？
5. 为什么同一 dma-buf fd 在不同 DRM fd 中可能有不同 handle？
6. GEM handle、framebuffer ID 和 plane ID 有什么区别？
7. 为什么不能 DQBUF→commit 后立即 QBUF？
8. page-flip event 和 atomic commit 返回分别说明什么？
9. implicit fence 和 explicit fence 有什么区别？
10. NV12 为什么需要两个 pitches/offsets，但可能只有一个 dma-buf fd？
11. 为什么 DRM plane 支持 NV12 仍可能 AddFB2/commit 失败？
12. IOMMU 存在时为什么还可能需要 CMA？
13. MMAP 采集没有 memcpy，为什么 copy-display 路径仍然有 memcpy？
14. RGA 转换是否可以称为零拷贝？应该如何准确描述？
15. 如何从代码和 perf 证明热路径没有 CPU memcpy？

---

# 9. 第四周：perf/ftrace、CMA、延迟与稳定性

## 本周目标

把“画面显示出来”升级为“有可重复数据的系统优化项目”。

## Day 22：建立可重复 benchmark

固定：

- sensor mode；
- V4L2 输出格式；
- 分辨率和 fps；
- buffer 数；
- DRM connector/mode/plane；
- CPU governor；
- 环境温度；
- 运行时间；
- 是否运行桌面合成器；
- 预热 30 秒。

每组至少重复 5 次，报告：

- mean；
- P50；
- P95；
- P99；
- min/max；
- 标准差；
- 样本数。

对照组：

```text
A：V4L2 → CPU memcpy → DRM
B：V4L2 → DMA-BUF → DRM
C（可选）：V4L2 → DMA-BUF → RGA → DRM
```

## Day 23：CPU 与调度指标

```bash
perf stat -p <pid> \
  -e cycles,instructions,cache-references,cache-misses,context-switches,page-faults \
  -- sleep 60
```

同时记录：

- 进程 CPU；
- 系统 CPU；
- 实际 fps；
- sequence gap；
- poll timeout；
- DRM commit failure；
- 温度和 CPU 频率；
- RSS/PSS；
- fd 数量。

## Day 24：ftrace/trace-cmd

先确认真实函数名：

```bash
grep -E 'vb2_buffer_done|drm_atomic_commit|rockchip.*commit' \
  /sys/kernel/debug/tracing/available_filter_functions
```

可能关注：

- `vb2_buffer_done`
- capture/ISP IRQ handler
- `drm_atomic_commit`
- Rockchip atomic commit tail
- VOP plane update
- `dma_fence_signal`
- sched switch/wakeup
- IRQ events
- CMA events

不能在简历中预写没有实际 trace 到的函数。

## Day 25：CMA 与 buffer 内存预算

先计算：

```text
capture buffer = 对齐后的 sizeimage × buffer count
display buffer = pitch/offset 计算出的 FB size × display buffer count
ISP internal   = 从驱动/日志/实际占用验证
RGA buffer     = 若启用 RGA，计入 input/output
安全余量       = 20%～50%
```

观察：

```bash
grep -i cma /proc/meminfo
dmesg | grep -i cma
mount -t debugfs none /sys/kernel/debug
find /sys/kernel/debug/cma -maxdepth 2 -type f -print
```

测试矩阵：

| 分辨率 | V4L2 buffers | DRM buffers | CMA | 冷启动分配 | 压力后分配 | 结论 |
|---|---:|---:|---:|---|---|---|
| 实际模式1 | 3 | 3 | 16M |  |  |  |
| 实际模式1 | 4 | 4 | 16M |  |  |  |
| 实际模式1 | 6 | 6 | 16M |  |  |  |
| 实际模式1 | 6 | 6 | 32M/64M |  |  |  |

只有出现以下完整证据链才能写“解决 CMA 分配失败”：

```text
vb2/DRM 分配失败
→ dmesg/trace 证明 CMA 或连续页分配失败
→ 修改 CMA
→ 同一测试条件成功
→ 长稳验证成功
```

如果没有失败，应写：

> 建立多媒体 buffer 内存预算模型，结合 CMA debugfs/trace 验证不同 buffer 深度下的
> 连续内存占用并给出配置建议。

## Day 26：延迟

区分三种延迟：

1. `DQBUF → atomic commit 返回`：用户态提交延迟。
2. `V4L2 timestamp/DQBUF → page-flip event`：软件显示调度延迟。
3. `LED 点亮 → 屏幕显示变化`：真实光子端到端延迟。

前两种不能冒充第三种。

软件程序记录：

```text
t_driver
t_dqbuf
t_commit_begin
t_commit_return
t_page_flip
t_requeue
```

## Day 27：长稳与异常测试

- [ ] 连续预览至少 2 小时。
- [ ] 启动/退出 100 次。
- [ ] 若支持，模式切换 100 次。
- [ ] 检查 fd、RSS、CMA 使用是否持续增长。
- [ ] 统计总帧、丢帧、timeout、commit failure、CSI error。
- [ ] 测试 SIGINT。
- [ ] 可选：测试 HDMI 断开/重连。

## Day 28：交付和演示

演示顺序：

1. 展示硬件和项目架构。
2. 展示 DTS 与 OV5695 probe。
3. 展示 chip ID 和 media topology。
4. 展示 RAW10 → RKISP → NV12。
5. 展示 V4L2 连续采集。
6. 展示 DRM plane formats。
7. 展示 dma-buf fd → GEM handle → framebuffer。
8. 展示实时预览。
9. 在 copy/dmabuf backend 间切换。
10. 展示 CPU、延迟和丢帧数据。
11. 展示 ftrace/perf。
12. 展示 CMA 与长稳报告。

### 第四周定向学习

#### V4L2 项目实战

- `17.html：帧同步与VSYNC`
- `20.html：调试与日志`
- `21.html：性能优化`
- `26.html：用户空间测试`
- `29.html：内核模块打包`
- `30.html：项目实战`

暂时跳过：

- `22.html：多路摄像头同步`
- `23.html：JPEG编码器集成`
- `27.html：热插拔支持`
- `28.html：安全与权限`

只有主线提前完成时再看。

#### 奔跑吧社区内存课

- `35～37：页面回收`
- `39：页面迁移`
- `40：内存规整`
- `63：查看系统内存信息的工具（一）`
- `64：查看系统内存信息的工具（二）`
- `65：读懂内核 log 中的内存管理信息`
- `66：读懂 /proc/meminfo`
- `68～70：Linux 内存管理参数调优`

#### 奔跑吧社区中断/锁课程

从“第2季 进程锁机制与中断管理三合一”选择：

- `94.基础课程---中断管理基本概念1`
- `95.基础课程---中断管理中断处理2`
- `96.基础课程---中断管理中断处理3`
- `97.基础课程---中断管理下半部机制4`
- `98.基础课程---中断管理面试题目5`
- `99.锁机制入门基本概念1`
- `100.锁机制入门Linux常用的锁2`

重点是理解 IRQ 上下文、下半部、并发、锁和等待，不要求学完整调度器系列。

#### 第四周知识点

- perf stat、perf record/report 的差异。
- ftrace function/function_graph、tracepoint。
- IRQ 延迟、调度延迟、buffer 排队延迟。
- 平均值为什么会隐藏长尾。
- CMA reservation、migration、compaction。
- CMA 与普通 buddy allocator 的关系。
- IOMMU 与 scatter-gather。
- buffer 深度对吞吐和延迟的相反影响。
- CPU governor、热降频对 benchmark 的影响。
- 软件时间戳和真实端到端延迟的边界。

#### 第四周面试自测

1. 为什么增加 buffer 数可能减少丢帧却增加延迟？
2. CMA 是开机后完全不能被普通内存使用的保留区吗？
3. CMA 分配为什么在 `CmaFree` 看似足够时仍可能失败？
4. 页面迁移和内存规整在 CMA 分配中有什么作用？
5. 如何证明问题来自 CMA，而不是 fd 泄漏或 ISP 配置？
6. perf 看到 cache misses 降低能直接证明零拷贝吗？
7. ftrace function graph 对实时性能有什么扰动？
8. P95/P99 为什么比平均延迟更重要？
9. `DQBUF→page-flip` 为什么还不是真实端到端延迟？
10. 如何保证 copy 和 dmabuf 两组 benchmark 公平？
11. 如何检查程序是否发生 fd、GEM handle 或 framebuffer 泄漏？
12. 一个 30 fps pipeline 的理论帧周期是多少？三缓冲最坏排队可能带来多少帧延迟？

---

# 10. 三套本地资料的使用索引

## 10.1 资料根目录

```text
资料 A：/media/whd/系统/奔跑吧社区
资料 B：/media/whd/系统/04-嵌入式学习之Linux驱动篇/驱动视频
资料 C：/home/whd/Downloads/《Linux+V4L2摄像头驱动项目实战》/《Linux V4L2摄像头驱动项目实战》
```

## 10.2 按问题查课，而不是按目录顺序看

| 当前问题 | 先看资料 |
|---|---|
| 不理解 probe/match | 驱动视频 18、20～22、44～46 |
| 不会写 DTS | 驱动视频 23～30；V4L2 15 |
| I²C 无 ACK/chip ID 失败 | 驱动视频 43～46；V4L2 11～12 |
| 不懂 Media topology | V4L2 1、4、13、14、24、25 |
| 不懂 V4L2 ioctl | 驱动视频 31～32；V4L2 6～9 |
| 不懂 MMAP/vb2 buffer | V4L2 7、10、16；内存课 24、27、30 |
| 不懂 RAW10/NV12 | V4L2 2、8、12、24、25 |
| 不懂中断到 DQBUF | 驱动视频 33、36～38；V4L2 10、17 |
| 不懂 DMA-BUF | V4L2 7、16、21 + videobuf2/DRM 源码 |
| 不懂物理/虚拟/DMA 地址 | 驱动视频 11；内存课 5～8、12～17、30 |
| CMA 分配失败 | 内存课 18～20、35～40、63～70 |
| 性能抖动/长尾 | V4L2 17、20～21；中断课 94～100 |
| 不会管理补丁 | Git 465、467、468 |

## 10.3 暂时不需要完整学习的内容

本项目第一个月可以暂缓：

- 字符设备号、杂项设备全套练习；
- input 子系统和触摸校准；
- RMAP 全系列；
- KSM；
- Meltdown；
- 系统调用表案例；
- 完整启动汇编；
- CFS/EAS 调度器完整代码；
- 多摄像头同步；
- JPEG 编码；
- 专业 ISP IQ 调校。

并不是这些内容没价值，而是它们不能直接解除当前项目的阻塞。

---

# 11. 建议的软件目录与模块设计

建议在仓库中新增独立项目目录：

```text
app/ov5695_zero_copy/
├── README.md
├── Makefile
├── include/
│   ├── camera.h
│   ├── display.h
│   ├── buffer_pool.h
│   └── metrics.h
├── src/
│   ├── main.c
│   ├── camera_v4l2.c
│   ├── display_drm.c
│   ├── buffer_pool.c
│   └── metrics.c
├── scripts/
│   ├── collect_board_info.sh
│   ├── inspect_media.sh
│   ├── benchmark.sh
│   ├── trace_pipeline.sh
│   └── stress_test.sh
├── docs/
│   ├── hardware.md
│   ├── media-topology.md
│   ├── buffer-lifecycle.md
│   ├── debug-diary.md
│   ├── performance.md
│   └── stability.md
└── results/
    ├── raw/
    └── summary/
```

程序接口建议：

```text
--video /dev/videoX
--drm /dev/dri/cardX
--width N
--height N
--format NV12
--fps N
--buffers N
--backend capture-only|copy|dmabuf|rga
--frames N
--duration N
--csv FILE
--verbose
```

模块职责：

| 模块 | 职责 |
|---|---|
| `camera_v4l2.c` | 格式枚举、REQBUFS、Q/DQBUF、EXPBUF |
| `display_drm.c` | DRM topology、plane、FB、atomic commit、page flip |
| `buffer_pool.c` | buffer 状态机和所有权 |
| `metrics.c` | timestamp、fps、P50/P95/P99、丢帧和 CSV |
| `main.c` | 参数解析、后端选择、事件循环和退出清理 |

---

# 12. 性能实验规范

## 12.1 每次实验必须记录

```text
日期：
Git commit：
kernel version：
DTB hash：
板卡/内存：
sensor mode：
V4L2 node：
V4L2 format：
width/height：
bytesperline/sizeimage：
fps：
V4L2 buffer count：
DRM card/connector/CRTC/plane：
DRM mode：
backend：
CPU governor：
CMA：
预热时长：
采样时长：
总帧：
丢帧：
CPU：
P50/P95/P99：
温度：
错误日志：
结论：
```

## 12.2 不能接受的测试方式

- copy 测 1080P、dmabuf 测 720P。
- 一组运行 10 秒，另一组运行 10 分钟。
- 只报告最好的一次。
- 未固定 CPU governor。
- 把颜色转换成本藏在 memcpy 中。
- 用 `top` 瞄一眼就写 CPU 降低比例。
- 把 `DQBUF→commit` 称为 sensor-to-display 端到端延迟。

---

# 13. 调试记录模板

每个问题都按下表记录：

```markdown
## 问题：一句话描述

### 环境

- commit：
- kernel/DTB：
- 硬件连接：
- 复现命令：

### 现象

- 用户态错误：
- dmesg：
- media topology：
- 复现概率：

### 假设

1. 假设 A：
2. 假设 B：

### 最小验证

- 只修改的变量：
- 预期结果：
- 实际结果：

### 根因

### 修复

### 回归验证

### 可形成的面试知识点
```

常见问题排查顺序：

```text
probe 不成功：
供电 → 时钟 → GPIO → I²C → chip ID → 驱动匹配

probe 成功但无 pipeline：
endpoint → async bind → media link → subdev format

STREAMON 失败：
格式传播 → sensor mode → DPHY/lane → ISP node → buffer queue

有帧但花屏：
format → Bayer order → stride → plane/offset → CSI error

DMA-BUF import 失败：
EXPBUF → fd 生命周期 → PRIME capability → GEM import

AddFB/commit 失败：
plane format → pitch/offset → modifier → CRTC compatibility

偶发撕裂：
buffer 所有权 → page-flip/fence → QBUF 时机

分配失败：
sizeimage → buffer 数 → fd 泄漏 → IOMMU/SG → CMA/compaction
```

---

# 14. 最终交付清单

## 内核/BSP

- [ ] DTS patch。
- [ ] 若有，OV5695 驱动适配 patch。
- [ ] 若有，defconfig/debug config patch。
- [ ] 编译与部署步骤。
- [ ] 回滚步骤。

## 用户态

- [ ] V4L2 capture-only backend。
- [ ] DRM test-pattern。
- [ ] copy backend。
- [ ] DMA-BUF backend。
- [ ] 参数、错误处理和资源释放。
- [ ] CSV metrics。

## 证据

- [ ] probe/chip ID 日志。
- [ ] `media-ctl -p`。
- [ ] `v4l2-ctl --list-formats-ext`。
- [ ] 正确 NV12 帧。
- [ ] `modetest -p`。
- [ ] DMA-BUF fd/GEM handle/FB ID 映射日志。
- [ ] perf 原始输出。
- [ ] ftrace/trace-cmd 原始 trace。
- [ ] CMA 数据。
- [ ] copy vs dmabuf 表格。
- [ ] 2 小时长稳数据。
- [ ] 演示视频。

## 文档

- [ ] 系统架构。
- [ ] Media topology。
- [ ] buffer 生命周期。
- [ ] 调试日记。
- [ ] 性能方法和结果。
- [ ] 已知限制。
- [ ] 面试问答。

---

# 15. 简历表述模板

在数据未测出前不要填写虚假数字。完成后按实际结果替换方括号。

> **基于 RK3568 的 OV5695 MIPI Camera BSP 调试与 DMA-BUF 零拷贝显示系统**  
> 技术栈：Linux 5.10、Device Tree、I²C、V4L2、Media Controller、MIPI CSI-2、
> RKISP、videobuf2、DMA-BUF、DRM/KMS、CMA、perf、ftrace、C
>
> - 基于 iTOP-RK3568 BSP 完成 OV5695 MIPI Camera 硬件链路验证与板级配置适配，
>   调试 I²C、MCLK、reset/pwdn、2-lane CSI-2 endpoint 及异步 subdev 绑定，联通
>   `OV5695 → DPHY → RKISP → V4L2` pipeline，并通过 Media Controller 拓扑和
>   stream 日志完成验证。
> - 开发 V4L2 采集诊断工具，支持格式/模式枚举、MMAP buffer、帧序号和时间戳统计、
>   NV12 图像保存及异常恢复，在 `[模式/分辨率]@[FPS]` 下连续采集 `[时长/帧数]`，
>   丢帧率为 `[数据]`。
> - 基于 `VIDIOC_EXPBUF` 导出 videobuf2 buffer，经 DRM PRIME import 和 KMS Atomic
>   Commit 直接提交至 VOP NV12 overlay plane；设计 capture/page-flip buffer 状态机，
>   避免 scanout 期间 buffer 被 ISP DMA 重写。
> - 使用 perf、ftrace 和 monotonic timestamp 对 capture、atomic commit、page-flip
>   路径进行剖析；在同条件下，DMA-BUF 相比 CPU-copy 路径将进程 CPU 从 `[A]%`
>   降至 `[B]%`，`DQBUF→page-flip` P95 从 `[C] ms` 降至 `[D] ms`。
> - 建立分辨率、stride 和 buffer 深度对应的 DMA 内存预算，结合 CMA debugfs/trace
>   分析连续内存占用，并在 `[真实条件]` 下完成 `[真实调优结果]`。

如果最后没有修改 OV5695 驱动或 DTS，第一条要改成：

> 基于现有 Rockchip BSP 完成 OV5695 MIPI Camera pipeline 的系统化验证与故障定位……

不要写“独立开发/从零移植”。

---

# 16. 最终面试总检查

项目结束时，应能在白板上完整回答：

## 驱动与设备树

1. Linux 如何从 DTS 找到 OV5695 驱动？
2. I²C sensor 与 platform ISP 的 probe 模型有何区别？
3. clk、regulator、pinctrl、GPIO 分别如何获取和释放？
4. runtime PM 为什么适合 camera sensor？
5. sensor probe 成功为什么不等于 pipeline 可用？

## Camera/V4L2

6. V4L2 device、subdev、media entity 和 video node 的关系。
7. OV5695 RAW10 如何经 RKISP 变为 NV12。
8. Media Controller pad/link 如何描述数据流。
9. vb2 的 buffer 状态和 QBUF/DQBUF 状态机。
10. MMAP、USERPTR、DMABUF 的区别。
11. 为什么 stride、sizeimage 和图像可见宽度不同。
12. sequence/timestamp 如何用于判断丢帧和延迟。

## DMA-BUF/DRM

13. DMA-BUF exporter/importer/attachment 的职责。
14. 物理地址、DMA address 和 IOVA 的区别。
15. fd、GEM handle、FB ID、plane ID 的区别。
16. 为什么 import 成功不代表 VOP 能 scanout。
17. Atomic KMS 相比 legacy KMS 的优势。
18. 为什么要等 page-flip/fence 才能重新 QBUF。
19. 什么条件下项目才可以称为零 CPU-copy。

## 内存与性能

20. CMA 和 buddy allocator 的关系。
21. IOMMU 与 CMA 各自解决什么问题。
22. 为什么内存碎片会影响连续内存分配。
23. 如何计算 NV12 buffer 内存预算。
24. buffer 越多为什么吞吐可能更稳、延迟却更高。
25. 如何用 perf/ftrace 证明优化有效。
26. 如何设计公平的 A/B benchmark。
27. 软件延迟和真实光子端到端延迟如何区分。
28. 如何判断性能下降来自 CPU、内存、IRQ、ISP、VOP 还是热降频。

只要能结合自己的代码、日志和数据回答，而不是背定义，这个项目就已经具备较强的
BSP/嵌入式 Linux 面试价值。

---

# 17. 第一天立即执行的任务

不要继续顺序看视频。第一天只做下面这些：

1. 建立 Git 分支并保存当前 commit。
2. 备份可启动镜像。
3. 上板执行第 5.1 节全部基线命令。
4. 保存完整 `dmesg`。
5. 确认是否出现：

   ```text
   Detected OV005695 sensor
   ```

6. 保存所有 `media-ctl -p` 输出。
7. 保存 `v4l2-ctl --list-devices`。
8. 找到 RKISP mainpath/selfpath 对应 video node。
9. 尝试用 `v4l2-ctl` 抓取 100 帧 NV12。
10. 将结果分为：

    ```text
    A. probe 失败
    B. probe 成功、media bind 失败
    C. topology 成功、STREAMON 失败
    D. 已经可以正常取帧
    ```

下一步只根据实际所属分支继续，不预判故障。

