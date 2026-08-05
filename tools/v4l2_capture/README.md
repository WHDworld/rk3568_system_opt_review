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
