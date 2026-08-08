# 第四周：perf/ftrace、CMA、延迟与稳定性测试报告

测试日期：2026-08-08

## 1. 测试目标和边界

本周把第三周“功能可用”的 pipeline 升级为可重复测量的系统实验。测试固定 OV5695/RKISP `/dev/video0`、`1280x720 NV12@30fps`、LVDS `1280x800@60`、CRTC 90、NV12 overlay plane 112，并在直接 KMS 测试期间停止 LightDM/Xorg。

软件延迟只测量 `driver timestamp→DQBUF`、`DQBUF→Atomic commit return` 和 `DQBUF→page-flip event`。这些数据不能称为 sensor-to-display 光子延迟；由于没有同步 LED、光敏器件或高速相机，本周没有测量 LED→屏幕的真实端到端延迟。

## 2. Day 22：五轮受控 A/B benchmark

每个 backend 先预热 900 帧（约 30 秒），再重复 5 轮、每轮 1800 帧（约 60 秒）。两组各统计 9000 个显示帧；十轮均满足 frames=page-flip events、无 sequence gap/duplicate/regression、无 fd 增长。

| 指标 | CPU copy | DMA-BUF | 变化 |
|---|---:|---:|---:|
| FPS 均值 | 30.036902 | 30.042593 | 吞吐相同 |
| 进程 CPU 均值 | 4.203% | 1.372% | 降低 67.36% |
| 系统 busy 均值 | 5.166% | 3.702% | 降低 28.34% |
| 用户态 copy | 922.500 µs/帧 | 0 | 消除 CPU copy |
| RSS 采样均值 | 13452.8 KiB | 7409.5 KiB | 降低 44.92% |
| DQBUF→commit return 均值 | 1348.542 µs | 154.131 µs | 降低 88.57% |
| DQBUF→page-flip 均值 | 10041.884 µs | 8958.827 µs | 降低 10.79% |
| DQBUF→page-flip P95 | 17358.660 µs | 16291.627 µs | 降低 6.15% |
| DQBUF→page-flip P99 | 18068.052 µs | 16945.485 µs | 降低 6.21% |

五轮 run-to-run FPS 标准差分别只有 0.000241 和 0.003163。BSP 没有暴露 cpufreq policy，因此无法固定或记录 CPU governor/频率；温度实际范围为 56.7～61.7°C，这一限制已保留，不能宣称严格固定频率。

## 3. Day 23：perf 和 `/proc` 指标

板端原系统没有 perf。使用同一 BSP 的 `kernel/tools/perf` 和 AArch64 工具链构建精简版 `perf 5.10.160`，确认硬件 PMU 可用后，对两条路径各附加统计 60 秒。

| perf event | CPU copy | DMA-BUF | 绝对计数降低 |
|---|---:|---:|---:|
| cycles | 3,586,060,020 | 1,019,546,022 | 71.57% |
| instructions | 684,542,247 | 227,799,646 | 66.72% |
| cache references | 353,164,508 | 22,367,522 | 93.67% |
| cache misses | 2,995,019 | 1,863,838 | 37.77% |
| context switches | 5,332 | 3,619 | 32.13% |
| page faults | 0 | 0 | — |

DMA-BUF cache-miss 比例为 8.333%，高于 copy 的 0.848%，但这是因为 DMA-BUF 的 cache-reference 分母减少了 93.67%；绝对 cache miss 仍减少 37.77%。面试或简历中应比较绝对计数和工作量，不能只挑 miss ratio。

## 4. Day 24：ftrace

板端没有 trace-cmd，但 tracefs 已挂载。先从 `available_filter_functions` 确认真正存在的函数，再使用 function tracer 跟踪：

```text
vb2_buffer_done
drm_atomic_commit
rockchip_drm_atomic_helper_commit_tail_rpm
dma_fence_signal
```

300 帧跟踪共保存 1218 个 function entries，实际观察到 `vb2_buffer_done`、`drm_atomic_commit` 和 Rockchip atomic commit tail。`dma_fence_signal` 虽然存在于可过滤函数列表，但本轮没有命中，因此不能写成已经抓到该调用。

`vb2_buffer_done` 不只属于主视频节点：trace 显示它还由 RKISP stats、params 和 `rkisp_buf_done_task` 等路径调用，所以不能把 910 次 `vb2_buffer_done` 简单解释成 300 个画面 buffer。

## 5. Day 25：CMA 和 buffer 预算

系统启动参数实际保留 16 MiB CMA，空闲稳定在 13264 KiB。单个 NV12 buffer 的实际 `sizeimage=1382400` 字节：

| V4L2 buffer 数 | capture buffer 预算 | DMA-BUF 额外显示图像 buffer | copy 后端额外 dumb buffer |
|---:|---:|---:|---:|
| 3 | 3.955 MiB | 0（复用 capture） | 2.637 MiB |
| 4 | 5.273 MiB | 0（复用 capture） | 2.637 MiB |
| 6 | 7.910 MiB | 0（复用 capture） | 2.637 MiB |

冷启动和创建 2 GiB tmpfs 内存压力后，4、6 buffer 的 DMA-BUF 测试均约 30 FPS、分配成功。3 buffer 在两种条件下都只有约 20 FPS，并出现 59 个 forward gap；这不是 CMA 分配失败，而是当前“page-flip 完成后才归还上一显示 buffer”的正确生命周期会同时占用两个显示相关 buffer，剩余采集深度不足，导致 ISP buffer 饥饿。因此本平台建议至少使用 4 buffer，6 buffer 可以运行但会增加潜在排队深度和内存预算。

CMA tracepoint 已启用，但测试区间没有 `cma_alloc_*`/`cma_release` 事件，`CmaFree` 也未变化。这说明本轮没有证据证明这些 buffer 在测试时触发 CMA 分配，可能使用已有池、IOMMU/SG 或其他 allocator。不能在项目描述中写“修复 CMA 分配失败”；准确表述应是“建立 buffer 内存预算并验证 16 MiB CMA 配置下 3/4/6 buffer 的行为边界”。

## 6. Day 26：软件延迟

工具新增四组逐帧统计：

```text
driver timestamp → DQBUF
DQBUF → Atomic commit return
DQBUF → page-flip event
Atomic ioctl call duration
```

DMA-BUF 五轮的平均分位数为：

| 指标 | mean | P50 | P95 | P99 |
|---|---:|---:|---:|---:|
| driver→DQBUF | 380.165 µs | 290.164 µs | 570.652 µs | 607.355 µs |
| DQBUF→commit return | 154.131 µs | 137.842 µs | 254.217 µs | 288.633 µs |
| DQBUF→page-flip | 8958.827 µs | 9109.801 µs | 16291.627 µs | 16945.485 µs |
| Atomic ioctl | 137.499 µs | 121.508 µs | 237.591 µs | 271.541 µs |

page-flip 延迟主要受 60 Hz 显示扫描相位影响，因此分布接近 0～16.67 ms，而不是固定值。DMA-BUF 主要消除了约 0.92 ms memcpy 和相关 CPU/cache 工作，并不能消除等待下一个 vblank 的时间。

## 7. Day 27：稳定性和异常测试

- [x] 启动/退出 100 次：100 成功、0 失败。
- [x] 100 次日志均 `fd_growth=0`，CmaFree 前后都是 13264 KiB。
- [x] SIGINT：146 帧后退出，146 次 flip，fd 9→9，STREAMOFF 成功，退出码 0，LightDM 恢复。
- [x] 连续 DMA-BUF 预览 7222.509 秒（120.375 分钟），217000 帧和 217000 次 page-flip event，30.044822 FPS。
- [ ] 模式切换 100 次：当前工具与实际 RKISP 节点只验证一种 NV12 模式，不能伪造“支持模式切换”。
- [ ] HDMI 热插拔：当前显示为板载 LVDS，不适用。

两小时长稳最终结果：

| 指标 | 结果 |
|---|---:|
| 连续时间 | 7222.509007 秒（120.375 分钟） |
| frames / page-flip | 217000 / 217000 |
| FPS | 30.044822 |
| sequence forward / duplicate / regression | 20 / 20 / 0 |
| 进程 CPU（`/proc`） | 1.354% |
| steady fd | 9→9，增长 0 |
| CmaFree | 13264→13264 KiB |
| SoC 温度 | 57.8～63.9°C，平均 61.3°C |
| 程序与 service 退出 | 0 / 正常完成 |
| 清理 | STREAMOFF success，LightDM active |

20 次 forward 与 20 次 duplicate 完全抵消，`first_sequence=0`、`last_sequence=216999` 与 217000 帧一致；这是约 0.0092% 的两类 sequence 标记异常事件，不是少收到 40 个 buffer。两小时 dmesg 增量只有 RKISP 时钟、DPHY 正常 stream on/off，没有新增 CSI/ISP error、timeout、IOMMU fault 或 DRM error。

RSS 从 5240 KiB 增长到 17424 KiB，其中程序为 217000 帧预分配 `217000×32=6944000` 字节的延迟样本，并随运行逐页触达；PSS/RSS 曲线与该有界数组吻合，进程退出后整体释放。它是测试插桩的有界内存成本，不是无界泄漏。生产预览版本应改用在线直方图或固定大小环形窗口，避免测试时长决定统计数组大小。

## 8. 当前结论与简历边界

可以据实写：完成 CPU copy 与 DMA-BUF 五轮受控对比；DMA-BUF 将进程 CPU 从约 4.20% 降到 1.37%，perf cycles 降低约 71.6%；实现 page-flip 完成后再归还 capture buffer；验证 4 buffer 是当前链路保持 30 FPS 的最低安全深度；用 ftrace 确认 VB2 与 Rockchip Atomic 关键路径。

暂时不能写：真实光子端到端延迟、解决 CMA 分配失败、支持多模式热切换、所有 fence 路径均已 trace。
