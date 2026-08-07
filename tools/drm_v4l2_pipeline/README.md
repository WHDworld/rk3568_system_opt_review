# DRM/KMS + V4L2 DMA-BUF 实验工具

该工具按互斥后端隔离问题：

- `pattern`：只打开 DRM、选择 connector/CRTC/plane、创建 XRGB dumb buffer、Atomic modeset 色条。
- `copy`：V4L2 MMAP DQBUF 后逐行复制 NV12 到双 DRM dumb buffer，再 Atomic page flip。
- `dmabuf`：初始化时为每个 V4L2 buffer 执行一次 EXPBUF、PRIME import 和 AddFB2；热路径不调用 memcpy。

板端编译：

```bash
adb push ai_doc/tools/drm_v4l2_pipeline /tmp/
adb shell 'cd /tmp/drm_v4l2_pipeline && make'
```

独立 KMS 需要 DRM master。测试前停止 LightDM，结束后恢复：

```bash
adb shell 'systemctl stop lightdm'
adb shell '/tmp/drm_v4l2_pipeline/drm_v4l2_pipeline --backend pattern --seconds 10'
adb shell 'systemctl start lightdm'
```

摄像头后端示例：

```bash
/tmp/drm_v4l2_pipeline/drm_v4l2_pipeline --backend copy \
  -W 1280 -H 720 -b 4 -n 1000

/tmp/drm_v4l2_pipeline/drm_v4l2_pipeline --backend dmabuf \
  -W 1280 -H 720 -b 4 -n 1000
```

`-D` 可指定 DRM card，`-d` 可指定 V4L2 node。程序以 `S_FMT` 返回的实际 width、height、stride 和 sizeimage 建立 framebuffer，不假定驱动一定接受请求值。

DMA-BUF 后端的 buffer 生命周期是：

```text
FREE -> CAPTURE_QUEUED -> CAPTURE_DONE -> DISPLAY_PENDING
     -> DISPLAYED -> page-flip releases previous -> FREE -> QBUF
```

程序在 QBUF 前检查状态，禁止把 DISPLAY_PENDING/DISPLAYED buffer 提前交回 ISP。

30 分钟板端验收使用 `systemd-run` 托管，避免 USB/ADB 瞬断连带杀死预览进程：

```bash
adb push ai_doc/tools/drm_v4l2_pipeline/run-longtest-board.sh /tmp/
adb shell 'chmod +x /tmp/run-longtest-board.sh'
adb shell 'systemd-run --unit=drm-v4l2-longtest --collect /tmp/run-longtest-board.sh'
adb shell 'systemctl status drm-v4l2-longtest --no-pager -l'
```

脚本会在测试前停止 LightDM，结束后重新启动；结果分别写入 `/tmp/dmabuf-longrun-detached.txt` 与 `/tmp/dmabuf-longrun-detached.exit`。
