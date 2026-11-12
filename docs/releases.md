# 版本状态

本页区分当前源码与公开安装包，详细历史记录见[历史档案](archive/README.md)。软件与数据版本的唯一来源是根目录 contracts.json；安装后的实际能力以该包 BUILD_INFO.txt、contracts.json 和 --help-all 为准。

## v1.16.0

状态：当前源码开发版本；v1.16.0-rc.1 已取消公开候选预发布并保留为草稿，不作为可下载稳定版推荐。此状态记录截至 2026-11-13，后续发布必须更新本页。

- 四后端接口包含 CPU、CUDA、Vulkan 和 Metal；新后端仍显式选择，auto 保留 CUDA → CPU。
- API 能力版本为 1.5，增加设备发现、设备选择及 compatible/fast 评分模式；已有 CPU/CUDA 行为和图库格式保留。
- Web UI 显示任务与预览的实际计算后端，命令生成可选 PowerShell 5.1、PowerShell 7.3+、CMD、Bash、Zsh 与 POSIX sh。
- 快速模式的质量退化与相对 FP64 最终选图差异均受 1% 门槛约束，范围与结果见[四后端验收](development/portable-gpu.md)。
- Intel、Linux GPU 和 Apple GPU 实机结果保留 not-run；编译与本机两块 GPU 的记录分别说明。

图库、特征、ANN、FeaturePack、颜色顺序及 journal 契约均为 1。现有有效版本 1 图库无须为新后端重建；v1.15.0 之前不兼容图库仍需另目录重建，见[搬移图库](guides/portable-libraries.md)。

## v1.15.6

截至 2026-11-13，为公开稳定版。下载和校验值见 [GitHub Release](https://github.com/CialloKing/Mosaicraft/releases/tag/v1.15.6)。稳定包不因当前分支文档更新而获得后续开发能力。

版本更新与附件验收流程见[发布清单](development/releasing.md)。
