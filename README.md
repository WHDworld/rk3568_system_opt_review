# RK3568 System Optimization Review

本仓库用于保存 RK3568 Linux BSP、驱动、Camera Pipeline、内存与性能优化方面的私人技术文档，
并持续记录每天的实验过程、原始证据和阶段复盘。

> 可见性要求：本仓库必须保持 **Private**。  
> 主源码仓库：`WHDworld/rk3568_system_opt`。  
> 本仓库只保存文档、日志、测试结果和小型复现片段，不复制完整 BSP 源码、固件镜像或第三方课程。

## 当前主线

当前重点项目：

> 基于 RK3568 的 OV5695 MIPI Camera BSP 调试与 V4L2–DRM DMA-BUF 零拷贝显示系统

项目总计划见：

- [OV5695 MIPI V4L2/DRM 四周计划](docs/03-camera-pipeline/RK3568_OV5695_MIPI_V4L2_DRM项目设计与四周学习计划.md)

## 仓库导航

| 路径 | 内容 | 更新频率 |
|---|---|---|
| [`docs/00-roadmap`](docs/00-roadmap/) | 总体路线、里程碑、周计划 | 每周 |
| [`docs/01-platform-bsp`](docs/01-platform-bsp/) | 编译、烧写、启动链和 BSP 基线 | 按实验 |
| [`docs/02-driver-labs`](docs/02-driver-labs/) | GPIO、I²C、设备树等驱动实验 | 按实验 |
| [`docs/03-camera-pipeline`](docs/03-camera-pipeline/) | OV5695、V4L2、RKISP、DMA-BUF、DRM | 高频 |
| [`docs/04-memory-performance`](docs/04-memory-performance/) | CMA、IOMMU、perf、ftrace、性能分析 | 按实验 |
| [`docs/05-debug-cases`](docs/05-debug-cases/) | 已定位问题的完整证据链 | 按问题 |
| [`docs/06-interview-review`](docs/06-interview-review/) | 面试知识点与项目复盘 | 每周 |
| [`journal`](journal/) | 每日工作日志；记录事实，不写成教程 | 每天 |
| [`templates`](templates/) | 日志、实验、故障、周报模板 | 少量维护 |
| [`artifacts`](artifacts/) | 原始串口日志、启动分析等不可再生证据 | 按需 |
| [`results/raw`](results/raw/) | benchmark 原始数据 | 每次测试 |
| [`results/summary`](results/summary/) | 清洗后的 CSV、表格和结论 | 每轮测试 |
| [`assets`](assets/) | 文档引用的图片和小型附件 | 按需 |

详细规则见 [仓库使用规范](CONTRIBUTING.md)。

## 每日最小闭环

每天至少完成一次下面的闭环：

```text
目标
→ 当前环境与源码 commit
→ 可复现命令
→ 现象和原始日志
→ 可证伪的假设
→ 单变量修改
→ 对照验证
→ 结论
→ Git commit
```

创建当日日志时，从
[`templates/daily-log.md`](templates/daily-log.md) 复制到：

```text
journal/YYYY/MM/YYYY-MM-DD.md
```

实验记录和日记分工：

- `journal/` 回答“今天做了什么、卡在哪里、明天做什么”。
- `docs/` 回答“这个机制是什么、正确方法是什么”。
- `artifacts/` 和 `results/raw/` 保存未经改写的原始证据。
- `results/summary/` 保存可用于报告和简历的统计结果。

## 文档状态标记

建议在重要文档开头使用：

```text
状态：草稿 / 实验中 / 已验证 / 已归档
验证平台：板卡、内核、DTB
最后验证：YYYY-MM-DD
关联源码：源码仓库 commit
```

未经上板验证的内容必须标记为“草稿”或“推测”；AI 生成的说明不能自动视为实验结论。

## 隐私与安全

提交前执行：

```bash
git status --short
git diff --cached
rg -n -i \
  'password|passwd|token|secret|api[_-]?key|authorization|BEGIN .*PRIVATE KEY|github_pat_|ghp_' \
  .
```

不得提交：

- GitHub Token、SSH 私钥、Wi-Fi 密码、账号密码；
- 公司或客户未授权资料；
- 包含个人身份信息的截图；
- `/etc/shadow`、认证 cookie、云服务凭据；
- 大型固件、完整 SDK、第三方课程视频和受版权保护的资料副本。

板卡 IP、MAC、序列号、用户名和目录路径虽然不一定是密钥，也应按需要脱敏。

## 提交信息建议

```text
docs(bsp): document kernel build and flash workflow
docs(camera): add ov5695 media topology baseline
journal: record 2026-07-30 repository setup
test(camera): add 1080p capture benchmark results
debug(usb): document otg and adb root cause
perf(cma): compare buffer allocation under cma sizes
```

