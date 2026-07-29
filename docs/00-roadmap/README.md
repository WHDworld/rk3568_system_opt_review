# 路线与里程碑

该目录保存跨项目的路线和阶段状态，不保存每天的流水账。

## 当前里程碑

| 里程碑 | 验收条件 | 状态 |
|---|---|---|
| M0：构建与回滚 | 能编译、部署、确认版本并恢复 | 待复核 |
| M1：OV5695 BSP 基线 | probe、chip ID、media topology | 待执行 |
| M2：V4L2 采集 | NV12 连续采集、统计丢帧 | 待执行 |
| M3：DRM 独立显示 | test pattern + atomic modeset | 待执行 |
| M4：DMA-BUF 直显 | PRIME import、正确 buffer 生命周期 | 待执行 |
| M5：性能与内存 | A/B benchmark、perf/ftrace、CMA | 待执行 |
| M6：长稳和交付 | 2 小时测试、文档、演示、简历 | 待执行 |

详细的 Camera 四周计划位于：

- [RK3568 OV5695 Camera 项目计划](../03-camera-pipeline/RK3568_OV5695_MIPI_V4L2_DRM项目设计与四周学习计划.md)

