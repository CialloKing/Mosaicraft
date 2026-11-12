# 文档中心

按使用目的查阅；历史报告和当前说明分开维护。本文及当前指南以本源码分支为准，安装包的实际版本与能力请以包内 BUILD_INFO.txt 和 contracts.json 为准。

## 开始使用

- [快速开始](guides/quick-start.md)：下载、启动、首次建库与生成。
- [Web UI](guides/web-ui.md)：预览、实时后端状态、命令行 Shell 和任务结果。
- [命令行指南](guides/cli.md)：图库管理、尺寸、输出与常用参数。
- [GPU 后端](guides/gpu-backends.md)：CPU/CUDA/Vulkan/Metal、设备、精度与回退。
- [搬移图库](guides/portable-libraries.md)：备份、相对路径和旧库重建。
- [便携工程与命令迁移](guides/portable-projects.md)：离线 HTML、回导、检查与打包。

## 接口与开发

- [HTTP API](reference/api.md)：API 1.5、任务、预览和诊断。
- [源码构建](development/building.md)：Windows、Linux、macOS 与 GPU 构建开关。
- [架构与数据契约](development/architecture.md)：处理流程、数据边界及版本来源。
- [CUDA 配置](development/cuda.md)：架构、模板与实机覆盖范围。
- [四后端实现与验收](development/portable-gpu.md)：实现边界、双 1% 门槛与实际结果。
- [CI 与打包](development/packaging.md)：原生 CI、依赖和独立包策略。
- [发布清单](development/releasing.md)：版本、验收、包内容与发布步骤。
- [文档维护](development/documentation.md)：分类、更新和链接检查规则。
- [版本状态](releases.md)：当前源码与公开发布的区别。

## 历史与原始数据

- [历史档案](archive/README.md)：旧版变更、验收、性能、研究与 GPU 迁移记录。
- [性能原始数据](performance/README.md)：保留原始 JSON/SVG 路径，不能替代当前性能结论。

旧书签入口：[API.md](API.md) · [ENCYCLOPEDIA.md](ENCYCLOPEDIA.md)。内容均已迁入对应目录。
