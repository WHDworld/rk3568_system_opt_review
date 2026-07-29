# 内存与性能分析

本目录用于沉淀：

- CMA、buddy allocator、页面迁移和内存规整；
- DMA API、scatter-gather、IOMMU 和 IOVA；
- videobuf2 与 DMA-BUF 的内存模型；
- perf、ftrace、trace-cmd 的方法；
- benchmark 设计、原始数据索引和性能结论。

任何性能数字必须注明：

- 源码 commit；
- kernel/DTB；
- 测试命令；
- CPU governor 和温度；
- 分辨率、格式、fps、buffer 数；
- 样本数和统计口径；
- 对应的 `results/raw/` 文件。

