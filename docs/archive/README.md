# 历史档案

这些记录保留当时的版本、环境、命令和结论。旧 API、CLI 参数与性能数字不代表当前行为；使用产品请返回[文档中心](../README.md)。机器可读数据见[原始数据索引](../performance/README.md)。

## 版本与旧版说明

- [Mosaicraft 1.15.0 不兼容更新](releases/breaking-1.15.0.md)
- [Mosaicraft 项目百科全书 / Project Encyclopedia](releases/encyclopedia.md)
- [Mosaicraft Release Checklist](releases/release-checklist-legacy.md)

## 功能验收

- [基础图透明叠加验证](verification/base-image-overlay-verification.md)
- [边界与一致性修复验证](verification/bugfix-verification.md)
- [CLI、HTML 工程与回导验收记录](verification/cli-projects-verification.md)
- [平均颜色 HSV v2 验证](verification/color-order-verification.md)
- [构建入口与引擎整理验收](verification/engine-refactor-verification.md)
- [指定目标图 HTML 工程生成测试](verification/html-nahaki-generation-check.md)
- [无版本工程生成检查](verification/html-project-generation-check.md)
- [HTML 便携工程真实图库验收](verification/html-project-real-data-verification.md)
- [HTML 工程单份素材与原文件名验收](verification/html-project-single-asset-verification.md)
- [HTML 最大缩放与可视窗口加载检查](verification/html-viewport-check.md)
- [窗口中心加载与素材卸载实测](verification/html-window-center-check.md)
- [输出格式与超限适配验收](verification/output-format-verification.md)
- [可选预览图验证](verification/preview-verification.md)
- [运行诊断与 Web UI 验收](verification/runtime-diagnostics-verification.md)
- [同尺寸素材免重编码验证](verification/same-size-normalization.md)

## 历史性能报告

- [CI 与打包历史采样](performance/packaging-20261108.md)
- [第一轮缓存与建库优化验收](performance/performance-phase1.md)
- [第二轮有界匹配验收](performance/performance-phase2.md)
- [v1.14.0 性能与稳定性验收报告](performance/performance-v1.14.0.md)
- [v1.14.1 选图提速与内存收敛验收报告](performance/performance-v1.14.1.md)
- [v1.14.5 性能与兼容说明](performance/performance-v1.14.5.md)

## 选图与质量研究

- [图库搬移与研究环境历史约定](studies/portable-paths.md)
- [Mosaicraft 可复现画质对照实验](studies/quality-study.md)
- [Mosaicraft 选图策略实验](studies/selection-study.md)

## CUDA 实现与迁移记录

- [CUDA 多架构历史验证](gpu/cuda-architectures-history.md)
- [CUDA 13.5 迁移验证](gpu/cuda-13-5-migration.md)
- [CUDA 传输流水线验证](gpu/cuda-transfer-pipeline.md)
- [动态尺寸 CUDA 特征提取验证](gpu/dynamic-cuda-features.md)
- [独立 CUDA 评分验证](gpu/independent-cuda-scoring.md)
