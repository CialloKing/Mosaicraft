# 版本状态

本页区分当前源码与公开安装包，详细历史记录见[历史档案](archive/README.md)。软件与数据版本的唯一来源是根目录 contracts.json；安装后的实际能力以该包 BUILD_INFO.txt、contracts.json 和 --help-all 为准。

## v1.15.7

本版保持五个标准包：Windows x64 CPU/CUDA、Linux x64 CPU/CUDA、macOS ARM64 CPU。Vulkan 和 Metal 通过条件编译关闭，不另发新后端包；Vulkan 用户须[自行从源码启用编译](development/building.md#vulkan-与-metal)。下载与校验值见 [GitHub Release](https://github.com/CialloKing/Mosaicraft/releases/tag/v1.15.7)。

- 发布构建显式关闭可选后端，并检查缓存，避免开发者构建目录将 Vulkan 带入正式包。
- Web UI 实时显示任务和预览的实际后端、设备与评分路径。
- 命令生成支持 PowerShell 5.1、PowerShell 7.3+、CMD、Bash、Zsh 和 POSIX sh，并按平台选择默认 Shell。
- 文档按用户指南、接口参考、开发与历史记录整理，发布包保留完整导航。
- API 能力版本保持 1.5；CPU/CUDA 行为与现有有效图库格式保持不变。

源码仍保留 Vulkan/Metal 的特征提取、快速评分与双 1% 门槛，详见[四后端实现与原验收记录](development/portable-gpu.md)。auto 保留 CUDA → CPU。Intel、Linux GPU 和 Apple GPU 实机结果保留 not-run；编译覆盖不等于硬件验收。

图库、特征、ANN、FeaturePack、颜色顺序及 journal 契约均为 1。现有有效版本 1 图库无须重建；v1.15.0 之前不兼容图库仍需另目录重建，见[搬移图库](guides/portable-libraries.md)。

## 已撤下的 v1.16.0-rc.1

2026-11-13 已取消公开候选预发布并保留草稿，不作为当前发布或下载推荐。历史候选的实测结果继续保留，不能据此宣称 v1.15.7 标准包包含 Vulkan/Metal。

## v1.15.6

上一公开稳定版。下载和校验值见 [GitHub Release](https://github.com/CialloKing/Mosaicraft/releases/tag/v1.15.6)。稳定包不因当前分支文档更新而获得后续开发能力。

版本更新与附件验收流程见[发布清单](development/releasing.md)。
