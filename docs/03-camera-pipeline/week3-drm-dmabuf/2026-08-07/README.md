# 第三周：DRM/KMS、DMA-BUF 与零拷贝显示测试报告

测试日期：2026-08-07～2026-08-08
测试平台：RK3568 + OV5695 MIPI，板端 Linux 5.10
采集节点：`/dev/video0`，实际输出 `1280x720 NV12`
实现源码：[`tools/drm_v4l2_pipeline`](../../../../tools/drm_v4l2_pipeline/README.md)

## 1. 结论

第三周的主链路已经实现并在板端实测：DRM Atomic 色条、V4L2→CPU copy→DRM 基线，以及 V4L2 MMAP buffer 经 `VIDIOC_EXPBUF`、DRM PRIME import 后直接交给 KMS overlay plane 的 DMA-BUF 后端均能显示。

1000 帧受控对比中，两种后端使用同一 `/dev/video0`、同一 `1280x720 NV12`、4 个采集 buffer、同一 connector/CRTC/overlay plane：

| 后端 | 帧数 | 实测 FPS | CPU copy 总耗时 | 平均 copy | page flip | sequence 异常 | fd 增长 |
|---|---:|---:|---:|---:|---:|---:|---:|
| CPU copy | 1000 | 30.040916 | 906.842 ms | 906.842 µs/帧 | 1000 | 0 | 本轮旧版未采集 |
| DMA-BUF | 1000 | 30.033033 | 0 ms | 0 µs/帧 | 1000 | 0 | 0（SIGINT 回归实测） |

这里的“零拷贝”严格指应用热路径中没有 CPU `memcpy()/memmove()`：ISP DMA 写 V4L2 buffer，DRM/VOP 扫描同一 dma-buf。它不表示系统中完全不存在 DMA 传输，也不包含 sensor→ISP 的数据搬运。

## 2. Day 15：DRM topology 与 test pattern

逐卡枚举确认：

| 对象 | 实测值 |
|---|---|
| DRM 显示设备 | `/dev/dri/card0`，rockchip-drm |
| 非显示 DRM 设备 | `/dev/dri/card1`，RKNPU |
| connector | 161，`LVDS-1`，connected |
| encoder | 160 |
| CRTC | 90，index 1 |
| mode | `1280x800@60` |
| primary plane | 76，possible CRTCs=`0x2`，不支持 NV12 |
| overlay plane | 112，possible CRTCs=`0x2`，支持 `NV12:LINEAR` |

`pattern` 后端完成 DRM device/resource 枚举、connected connector/CRTC/plane 自动选择、XRGB8888 dumb buffer、`drmModeAddFB2()` 和彩条生成。在停止 LightDM、获得 DRM master 后，经 Atomic modeset 稳定显示 8 秒，退出码为 0；测试脚本随后恢复 LightDM。

证据：`raw/modetest-*.txt`、`raw/pattern-atomic-test.txt`。

## 3. Day 16：Atomic KMS

程序依次启用 `DRM_CLIENT_CAP_UNIVERSAL_PLANES` 与 `DRM_CLIENT_CAP_ATOMIC`，读取 connector、CRTC、plane 的属性 ID，创建 mode blob，并设置：

```text
connector.CRTC_ID -> CRTC 90
CRTC.MODE_ID      -> 1280x800 mode blob
CRTC.ACTIVE       -> 1
plane.FB_ID       -> framebuffer
plane.CRTC_ID     -> CRTC 90
SRC_X/Y/W/H       -> 16.16 定点源矩形
CRTC_X/Y/W/H      -> 屏幕目标矩形
```

显示更新使用 `DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT`，收到 page-flip event 后才认为本次扫描切换完成。

## 4. Day 17：CPU copy 基线

`copy` 后端创建两个 NV12 DRM dumb framebuffer。每次 `DQBUF` 后按实际 stride 逐行复制 Y、UV，再将采集 buffer 立即 `QBUF`，随后 Atomic page flip 到另一显示 buffer。采集与显示均为 NV12，本轮没有颜色转换，因此 906.842 µs/帧是纯 CPU copy 的计时，不混入 YUV→RGB 转换成本。

程序不假设 `bytesperline == width` 或 `sizeimage == width*height*1.5`，实际采用 `VIDIOC_S_FMT` 返回的：

```text
width=1280 height=720 fourcc=NV12
bytesperline=1280 sizeimage=1382400
Y offset=0, UV offset=921600
```

证据：`raw/copy-1000frames.txt`。

## 5. Day 18：一次性 DMA-BUF export/import

初始化阶段对 4 个 V4L2 MMAP buffer 各执行且仅执行一次：

```text
VIDIOC_EXPBUF -> dma-buf fd
drmPrimeFDToHandle() -> GEM handle
drmModeAddFB2() -> framebuffer ID
```

1000 帧日志只出现 4 条 `DMABUF_INIT`，对应 index 0～3；运行热路径只执行 DQBUF、Atomic commit、page-flip wait 和 QBUF，不重复 export/import/AddFB2。

## 6. Day 19：buffer 生命周期

实际状态机为：

```text
FREE --QBUF--> CAPTURE_QUEUED --DQBUF--> CAPTURE_DONE
     --atomic commit--> DISPLAY_PENDING --flip event--> DISPLAYED
     --下一帧 flip 释放--> FREE --QBUF--> CAPTURE_QUEUED
```

当前正在扫描的 buffer 保留在 `DISPLAYED`，下一次 page flip 完成后才把上一 buffer 重新 QBUF。`queue_capture()` 会检查状态，若试图把 `DISPLAY_PENDING`/`DISPLAYED` buffer 提前交回 ISP，会报状态错误并停止，而不是冒险覆盖扫描中的图像。

## 7. Day 20：NV12 framebuffer 映射

RKISP 当前采用 V4L2 single-planar API：一个物理 buffer 内连续存放 Y 和交错 UV；DRM 的 NV12 framebuffer 则描述两个图像 plane。因此映射参数为：

| DRM 图像 plane | handle | pitch | offset | modifier |
|---|---|---:|---:|---|
| Y | 同一个 PRIME GEM handle | 1280 | 0 | LINEAR |
| UV | 同一个 PRIME GEM handle | 1280 | 921600 | LINEAR |

DRM format 为 `DRM_FORMAT_NV12`，overlay plane 112 明确支持 NV12 LINEAR。这里两个 DRM image plane 不是两个 V4L2 physical plane，也不是两个 dma-buf；它们用同一 handle 加不同 offset 描述。

如果后续模式的 stride、offset 或 modifier 与该 plane 不兼容，应优先让 RKISP 输出 VOP 支持的格式，其次选择其他 overlay；需要 RGA 转换时必须称为“DMA-BUF + RGA 硬件转换”，不能写成同一 buffer 直显。

## 8. Day 21：稳定性与异常退出

已完成：

- 1000 帧 CPU copy 与 DMA-BUF 对照，两轮均约 30 FPS、1000 次 page flip。
- SIGINT 回归：177 帧后退出，177 次 flip；`steady_fds_start=9`、`steady_fds_end=9`、`fd_growth=0`，`STREAMOFF success`，退出码 0，LightDM 恢复 active。
- 首次 SIGINT 测试暴露“信号到达后未消费已提交 flip event”的退出错误；修改为已提交 Atomic commit 后必须收完 event，再进入清理，第二次回归通过。失败日志也保留为问题定位证据。
- 第一轮长测在约 26.6 分钟时被 USB gadget reset 切断 ADB 会话；板子未重启，驱动执行 stream-off。该轮不计作 30 分钟通过，随后改为 `systemd-run` 托管板端测试，避免 ADB 断连杀死测试进程。

脱离 ADB 的最终长测由 systemd transient unit 托管，实测结果为：

| 指标 | 结果 |
|---|---:|
| 连续时间 | 1997.029513 秒（33.28 分钟） |
| 完成帧 / page-flip event | 60000 / 60000 |
| 实际 FPS | 30.044123 |
| forward gap event | 4（占 60000 帧的 0.0067%） |
| duplicate sequence event | 3（占 60000 帧的 0.0050%） |
| regression | 0 |
| CPU copy | 0 ms |
| steady fd | 9 → 9，增长 0 |
| 外部 RSS 采样 | 5272 KiB，运行期间未增长 |
| 程序/服务退出 | 0 / 0 |
| 清理与桌面 | `STREAMOFF success`，LightDM active |

`last_sequence=60000` 而帧数为 60000，是 4 次 forward 与 3 次 duplicate 抵消后的净增量；不能把它简单相加写成“丢 7 帧”。严格丢帧候选是 4 次 forward gap，但 duplicate 说明 RKISP sequence 标记本身存在低频抖动，后续仍需结合 timestamp 和驱动 trace 定位。长测期间内核日志只有本轮正常的 DPHY stream on/off，没有新增 CSI/ISP error、timeout 或 IOMMU fault。

长测期间主机侧确实又发生了一次 USB gadget reset/ADB 重连，但 systemd 托管的预览未受影响并完成 60,000 帧，这验证了修改后的测试方法。证据：`raw/dmabuf-60000frames-detached.txt`、`raw/dmabuf-detached-runtime-samples.txt`、`raw/dmesg-longrun-focus.txt`、`raw/dmesg-after-longrun.txt`。

## 9. 验收清单

- [x] DRM test pattern 稳定显示。
- [x] 打印 connector/CRTC/plane/property 选择过程。
- [x] CPU copy backend 可工作。
- [x] DMA-BUF backend 可工作。
- [x] export/import/framebuffer 只在初始化发生。
- [x] DMA-BUF 热路径无 `memcpy()/memmove()`。
- [x] 等待 page-flip 后再释放并 QBUF 上一显示 buffer。
- [x] 脱离 ADB 连续预览 33.28 分钟，60,000 帧，fd 无增长，完成最终日志复核。
- [x] SIGINT 后释放资源并由测试脚本恢复 LightDM。

## 10. 可复现实验

板端编译和短测命令见工具 README。30 分钟测试使用 systemd transient unit 托管：

```bash
adb push tools/drm_v4l2_pipeline/run-longtest-board.sh /tmp/
adb shell 'systemd-run --unit=drm-v4l2-longtest --collect /tmp/run-longtest-board.sh'
adb shell 'systemctl status drm-v4l2-longtest --no-pager -l'
```

直接 KMS 测试需要 DRM master，所以脚本会临时停止 LightDM，结束后无论程序成功或失败都会重新启动 LightDM。若主机异常断电，应在板端执行 `systemctl start lightdm`。

## 11. 尚未完成与下一步

本周完成的是功能正确性和 CPU copy 微基准，并未完成完整性能结论。下一周需要在相同模式下采集进程 CPU%、perf/ftrace、端到端延迟、CMA/内存占用，并做多轮 warm-up 后统计中位数和波动范围。长测中出现的 V4L2 sequence gap/duplicate 要结合 ISP/CSI 日志、调度延迟和采集时间戳继续定位，不能直接归因于 DMA-BUF 或摄像头硬件。
