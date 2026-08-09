# 可诊断 V4L2 MMAP 采集工具

该工具不依赖 OpenCV 或 libv4l2，直接使用 Linux V4L2 ioctl，兼容 single-planar 与 multi-planar capture API。它不会假设驱动接受请求的尺寸，也不会假设 `bytesperline == width` 或 `sizeimage == width × height × 1.5`，所有实际参数均打印自 `VIDIOC_S_FMT` 的返回值。

实现的主要调用包括：

```text
VIDIOC_QUERYCAP
VIDIOC_ENUM_FMT
VIDIOC_ENUM_FRAMESIZES
VIDIOC_ENUM_FRAMEINTERVALS
VIDIOC_G_FMT / VIDIOC_S_FMT
VIDIOC_G_PARM / VIDIOC_S_PARM
VIDIOC_REQBUFS / VIDIOC_QUERYBUF
mmap / VIDIOC_QBUF / VIDIOC_STREAMON
poll / VIDIOC_DQBUF / VIDIOC_STREAMOFF / munmap
```

## 设计目标

这个程序不是只为“抓到一张图”编写的最小示例，而是一个用于定位 V4L2 pipeline 问题的基线工具。设计上将问题拆成五层：

1. 设备节点是否真的是支持 streaming 的 capture node；
2. 驱动实际支持哪些格式、尺寸和帧间隔；
3. 驱动最终接受了什么格式、stride、sizeimage 和帧率；
4. MMAP buffer 是否按照正确的所有权顺序循环；
5. 收到的 buffer 在 sequence、timestamp、payload 和退出清理方面是否正常。

程序只依赖内核 UAPI 和标准 C 库，直接调用 ioctl，避免 OpenCV/libv4l2 自动协商、颜色转换和内部缓冲掩盖驱动返回值。源码入口是 `main()`，核心采集循环是 `run_capture()`。

## 源码结构与职责

| 源码对象/函数 | 作用 |
|---|---|
| `struct options` | 保存命令行请求值，例如设备、分辨率、FOURCC、buffer 数、帧数和超时 |
| `struct capture` | 保存驱动实际返回的 API 类型、格式参数、映射表、文件句柄和 streaming 状态 |
| `struct mapped_buffer` | 描述一个 V4L2 buffer；内部可包含一个或多个物理 plane |
| `xioctl()` | ioctl 的统一包装；普通 `EINTR` 自动重试，收到退出信号后停止重试 |
| `query_capabilities()` | `QUERYCAP`，选择 single-planar 或 multi-planar capture API |
| `enumerate_capabilities()` | 枚举 format、frame size 和 frame interval |
| `configure_format()` | 先读 `G_FMT`，再提交 `S_FMT`，保存驱动返回的实际格式 |
| `configure_frame_rate()` | 用 `G_PARM/S_PARM` 请求帧率；节点不支持时给出提示而不是直接失败 |
| `request_and_map_buffers()` | `REQBUFS→QUERYBUF→mmap`，建立 index/plane 到用户虚拟地址的映射表 |
| `queue_buffer()` | 按当前 API 类型构造 `v4l2_buffer`，执行 `QBUF` |
| `run_capture()` | 完成初始 QBUF、STREAMON、poll/DQBUF/处理/QBUF 循环和最终统计 |
| `write_payload()` | 校验 `data_offset/bytesused/映射长度`，按 plane 顺序可选写入 raw 文件 |
| `cleanup()` | 按 STREAMOFF、munmap、释放堆内存、关闭文件和设备 fd 的顺序统一清理 |

## 完整工作流程

`main()` 的实际执行顺序是：

```text
解析命令行 parse_options()
        │
        ▼
open(video node, O_RDWR | O_NONBLOCK | O_CLOEXEC)
        │
        ▼
VIDIOC_QUERYCAP
        │
        ├─ V4L2_CAP_VIDEO_CAPTURE_MPLANE
        │       → VIDEO_CAPTURE_MPLANE
        │
        └─ V4L2_CAP_VIDEO_CAPTURE
                → VIDEO_CAPTURE
        │
        ▼
ENUM_FMT → ENUM_FRAMESIZES → ENUM_FRAMEINTERVALS
        │
        ├─ --enumerate：打印后退出
        │
        ▼
G_FMT → S_FMT → 保存驱动返回的实际参数
        │
        ▼
G_PARM → S_PARM（节点不支持时继续）
        │
        ▼
REQBUFS → 对每个 index 执行 QUERYBUF → 对每个 plane 执行 mmap
        │
        ▼
所有 buffer 先 QBUF → STREAMON
        │
        ▼
poll → DQBUF → 诊断/可选保存/可选预览 → QBUF
        │                                      │
        └──────────────────────────────────────┘
        │ 达到帧数、SIGINT、SIGTERM 或窗口退出
        ▼
STREAMOFF → munmap → fclose → close
```

### 1. 打开节点并确定 API 类型

`main()` 使用非阻塞方式打开设备：

```c
open(opt.device, O_RDWR | O_NONBLOCK | O_CLOEXEC)
```

`O_NONBLOCK` 使 `DQBUF` 在暂时没有完成 buffer 时返回 `EAGAIN`，程序可以统计这种情况，而不是无条件睡死在 ioctl 内；真正的等待由 `poll()` 管理。`O_CLOEXEC` 避免将摄像头 fd 意外泄漏给以后执行的其他程序。

`query_capabilities()` 不根据设备名猜测 API，而是读取 `device_caps`：优先选择 `V4L2_CAP_VIDEO_CAPTURE_MPLANE`，否则退回 `V4L2_CAP_VIDEO_CAPTURE`。后续所有 format、buffer 和 plane 字段都由 `cap->multiplanar` 决定，因而同一套采集循环可以覆盖两种 API。

需要区分两个容易混淆的概念：

- multi-planar API 指 ioctl 使用 `v4l2_pix_format_mplane` 和 `v4l2_plane[]`；
- `num_planes=1` 表示当前图像只有一个物理 buffer plane。

当前 RKISP 的 NV12 节点属于 multi-planar API，但实际返回一个物理 plane，Y 和 UV 连续存放在同一块 buffer 中。这不等于传统 single-planar API。

### 2. 能力枚举与格式协商

`enumerate_capabilities()` 的嵌套关系是：

```text
每个 VIDIOC_ENUM_FMT
  └─ 每个 VIDIOC_ENUM_FRAMESIZES
       └─ 每个 VIDIOC_ENUM_FRAMEINTERVALS
```

离散尺寸逐项枚举；遇到 continuous/stepwise 尺寸则打印 min、max 和 step。枚举结束时驱动通常以 `EINVAL` 表示“没有下一个条目”，所以代码只在 errno 不是 `EINVAL` 时报告错误。

`configure_format()` 先用 `VIDIOC_G_FMT` 保存修改前的状态，再将用户请求写入 `VIDIOC_S_FMT`。关键原则是：`S_FMT` 的结构体既是输入也是输出。驱动可以调整 width、height、FOURCC、plane 数、bytesperline 和 sizeimage，因此程序只把 ioctl 返回值写入 `struct capture`，后续 mmap、payload 和预览都使用这些实际值。

也就是说，以下假设在代码中被明确禁止：

```text
actual width == requested width
actual height == requested height
bytesperline == width
sizeimage == width × height × 1.5
multi-planar API 一定返回多个物理 plane
```

`configure_frame_rate()` 同样读取 `S_PARM` 返回的 `timeperframe`。部分 RKISP video node 不实现 `G_PARM/S_PARM`，此时 `ENOTTY/EINVAL` 被记录为“不支持”，不会破坏本来可工作的采集链路。

### 3. MMAP buffer 如何建立

`request_and_map_buffers()` 先通过 `VIDIOC_REQBUFS` 请求 MMAP buffer。驱动返回的 `req.count` 可能和请求数不同，所以程序重新读取并保存实际数量，而不是继续按命令行数量访问。

随后对每一个 buffer index：

```text
VIDIOC_QUERYBUF(index)
  └─ 对每个物理 plane 读取 length 和 mem_offset
       └─ mmap(fd, mem_offset, length)
```

映射完成后的关系是：

```text
buffers[index].planes[plane].addr
buffers[index].planes[plane].length
```

`mmap()` 只把驱动/videobuf2 管理的 buffer 映射到进程虚拟地址空间，不会因为映射本身复制整帧数据。ISP DMA 完成后，用户态通过这段虚拟地址读取同一 buffer。只有启用 `--output` 时的 `fwrite()` 或启用 X11 预览时的 NV12→RGB 才会产生额外 CPU 数据处理。

### 4. Buffer 所有权循环

程序没有用一个枚举变量显式保存 buffer 状态，但 ioctl 顺序隐含了严格的所有权状态机：

```text
用户态已映射/可提交
        │ VIDIOC_QBUF
        ▼
驱动队列所有：QUEUED
        │ ISP DMA 填充完成
        ▼
驱动完成队列：DONE
        │ poll 可读 + VIDIOC_DQBUF
        ▼
用户态所有：DEQUEUED
        │ 统计、保存或预览
        │ VIDIOC_QBUF
        └──────────────────► QUEUED
```

STREAMON 前必须先把所有 buffer QBUF，否则驱动没有可供 DMA 写入的目标。DQBUF 后，代码只访问 `buf.index` 对应的映射；处理结束立即把同一 index QBUF 回驱动。用户态不能在 QBUF 后继续修改该 buffer，因为此时所有权已经交给驱动和 ISP。

主循环采用 `poll(POLLIN | POLLPRI)`，而不是不停调用 DQBUF：

- timeout：累计 `poll_timeouts`，打印错误后继续观察链路；
- `EINTR`：普通信号中断时重试，SIGINT/SIGTERM 时退出；
- `POLLERR/POLLHUP/POLLNVAL`：认为节点或 fd 状态异常，停止测试；
- `DQBUF/EAGAIN`：累计计数后继续 poll；
- 非法 `buf.index`：立即失败，避免越界访问映射表。

### 5. Payload 为什么必须检查 bytesused 和 data_offset

`write_payload()` 不直接把 `sizeimage` 字节全部写入文件。`sizeimage` 是分配容量，而 `bytesused` 才是本帧有效范围；multi-planar buffer 还可能在开头保留 `data_offset`。

每个 plane 的有效载荷为：

```text
payload address = mapped base + data_offset
payload length  = bytesused - data_offset
```

写入前会验证：

```text
bytesused >= data_offset
bytesused <= mmap length
```

验证失败会停止程序，防止越界读。多个物理 plane 按驱动返回顺序连续写入文件。对当前单物理 plane NV12，文件内部仍是完整的 Y plane 后接交错 UV plane；“一个物理 plane”和“NV12 有两个图像分量”并不矛盾。

### 6. 可诊断信息如何产生

每次 DQBUF 后同时保存两个时间：

```text
driver_timestamp_ns = buf.timestamp
dq_monotonic_ns     = DQBUF 后立即读取 CLOCK_MONOTONIC
```

驱动 timestamp 用来计算相邻采集帧间隔；用户态 monotonic 时间描述应用实际拿到 buffer 的时刻。30 FPS 的理想驱动时间戳间隔约为 `33.333 ms`，但调度和驱动行为会带来抖动。

Sequence 连续性分成三类：

```text
current == previous        → duplicate
current < previous         → regression
current > previous + 1     → forward_missing += 差值 - 1
```

三者不能混成一个“丢帧数”。例如一次 forward 后紧跟一次 duplicate，工具实际收到的 buffer 总数没有减少，可能只是驱动填写 sequence 时发生低频标记抖动。必须结合首末 sequence、总 buffer 数、timestamp、CSI/ISP 日志和 trace 一起判断。

最终 FPS 使用第一帧和最后一帧的用户态 monotonic 时间计算：

```text
fps = (frames - 1) / (last_dq_time - first_dq_time)
```

使用 `frames-1` 是因为 N 个时间戳之间只有 N-1 个间隔。

### 7. 统一错误处理和退出清理

`main()` 的各阶段失败都会跳到同一个 `out:`，由 `cleanup()` 根据当前已初始化状态释放资源。因此即使只完成了一部分初始化，也不会要求每个错误分支复制一套清理代码。

清理顺序为：

```text
如果 streaming：VIDIOC_STREAMOFF
→ munmap 每个已映射 buffer/plane
→ free buffer 描述数组
→ 释放 X11 资源（若启用）
→ fclose raw/CSV
→ close video fd
```

SIGINT/SIGTERM 的 handler 只设置 `volatile sig_atomic_t stop_requested`，不在异步信号上下文里调用 ioctl、stdio 或 free。采集循环观察标志后退出，再在正常进程上下文执行完整清理，这是信号安全设计的关键。

## 一次采集对应的核心 ioctl 时序

```text
QUERYCAP
  ↓
ENUM_FMT / ENUM_FRAMESIZES / ENUM_FRAMEINTERVALS
  ↓
G_FMT → S_FMT
  ↓
G_PARM → S_PARM
  ↓
REQBUFS
  ↓
QUERYBUF(index 0..N-1) → mmap(each plane)
  ↓
QBUF(index 0..N-1)
  ↓
STREAMON
  ↓
┌─ poll
│   ↓
│  DQBUF(index)
│   ↓
│  timestamp/sequence/payload 诊断
│   ↓
│  可选 fwrite / X11 preview
│   ↓
└─ QBUF(index)
  ↓
STREAMOFF → munmap → close
```

## 交叉编译

在 BSP 根目录执行：

```bash
make -C ai_doc/tools/v4l2_capture \
  CROSS_COMPILE="$PWD/prebuilts/gcc/linux-x86/aarch64/gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu/bin/aarch64-none-linux-gnu-"
```

确认架构：

```bash
file ai_doc/tools/v4l2_capture/v4l2_capture
```

部署：

```bash
adb push ai_doc/tools/v4l2_capture/v4l2_capture /tmp/v4l2_capture
adb shell chmod 755 /tmp/v4l2_capture
```

## 使用方法

只枚举能力：

```bash
/tmp/v4l2_capture -d /dev/video0 --enumerate
```

采集 300 帧并保存逐帧 CSV：

```bash
/tmp/v4l2_capture -d /dev/video0 \
  --width 1920 --height 1080 --pixfmt NV12 \
  --buffers 4 --frames 300 --fps 30/1 \
  --output /tmp/ov5695-1080p.nv12 \
  --csv /tmp/ov5695-1080p.csv \
  --log-every 30
```

不保存图像、只做长时间统计：

```bash
/tmp/v4l2_capture -d /dev/video0 \
  -W 1920 -H 1080 -p NV12 -b 4 -n 54000 \
  -c /root/ov5695-longrun.csv -l 900
```

以 30 fps 计算，54,000 帧约为 30 分钟。程序收到 `SIGINT` 或 `SIGTERM` 后会退出采集循环，执行 `VIDIOC_STREAMOFF`、`munmap`、`fclose` 和 `close`。

## 输出解释

每条 `FRAME` 或 CSV 记录包含：

- 工具内部 frame number；
- 驱动返回的 V4L2 sequence；
- dequeued buffer index；
- 驱动 timestamp；
- `DQBUF` 后读取的 `CLOCK_MONOTONIC` 时间；
- 各 plane `bytesused` 总和；
- 相邻驱动时间戳间隔；
- forward missing、duplicate 和 regression 三类 sequence 连续性指标；
- V4L2 buffer flags。

最终 `SUMMARY` 给出总帧数、首末 sequence、向前缺号数、重复数、回退数、poll timeout、`DQBUF/EAGAIN` 次数、payload 字节数、耗时、实际 fps 和是否被信号中断。将三类 sequence 异常分开是必要的：一次“向前缺号”后若紧接同号重复，收到的 buffer 总数并没有减少，它更可能是驱动 sequence 标记抖动，而不是可以直接认定的真实丢帧。

保存到 raw 文件时，各 plane 按 V4L2 返回顺序紧邻写入，并尊重 `data_offset` 与 `bytesused`。对于当前 RKISP 的 NV12 multi-planar API，驱动返回一个物理 plane，其中连续包含 Y 与交错 UV 数据。

## X11 实时预览窗口

预览功能是可选编译项，不影响无图形依赖的基线程序。在当前 RK3568 Ubuntu/XFCE 板端原生编译：

```bash
adb push ai_doc/tools/v4l2_capture/v4l2_capture.c /tmp/
adb push ai_doc/tools/v4l2_capture/Makefile /tmp/
adb shell 'cd /tmp && make -f Makefile preview'
```

以桌面用户启动窗口：

```bash
adb shell 'DISPLAY=:0 XAUTHORITY=/home/topeet/.Xauthority \
  runuser -u topeet -- /tmp/v4l2_capture_x11 \
  -d /dev/video0 -W 1920 -H 1080 -p NV12 -b 4 \
  -n 100000000 --preview --preview-size 640x360 -l 300'
```

窗口标题每秒更新一次，显示实际采集尺寸、实时 FPS、frame、V4L2 sequence，以及 forward missing/duplicate/regression 计数。按 `Esc`、`Q` 或关闭窗口会走正常的 STREAMOFF、munmap 和 close 清理流程。默认窗口为 640×360；在当前板卡上能维持约 30 fps。纯 CPU 的 960×540 转换实测会降到约 22 fps 并引起采集缺号，因此不作为默认值。

如果希望从开发电脑通过 ADB 启动后让窗口持续留在板载屏幕，可以使用板端 root 的 Xauthority 和 `setsid`：

```bash
adb shell 'setsid -f env DISPLAY=:0 \
  XAUTHORITY=/var/run/lightdm/root/:0 \
  /tmp/v4l2_capture_x11 \
  -d /dev/video0 -W 1920 -H 1080 -p NV12 -b 6 \
  -n 100000000 --preview --preview-size 640x360 -l 300 \
  > /tmp/v4l2-preview.log 2>&1 < /dev/null'
```

这里的 `DISPLAY=:0` 是 RK3568 板端 Xorg，窗口显示在板卡连接的 LVDS 屏幕，不会显示到运行 ADB 的开发电脑。

当前 X11 路径支持 RKISP 返回的单物理 plane NV12。它按照 `S_FMT` 返回的 source width、height 和 bytesperline 读取 Y/UV，使用 CPU 完成 NV12→RGB 与最近邻缩放。该窗口适合直观调试，但 CPU 转换会改变性能数据；严肃的性能基线仍应使用不带 `--preview` 的版本，后续 DMA-BUF/DRM 阶段再实现硬件零拷贝显示。
