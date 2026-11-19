# 版本状态

本页区分当前源码与公开安装包，详细历史记录见[历史档案](archive/README.md)。软件与数据版本的唯一来源是根目录 contracts.json；安装后的实际能力以该包 BUILD_INFO.txt、contracts.json 和 --help-all 为准。

## v1.15.8

本版主要缩短普通单图生成时间。五个标准包仍为 Windows x64 CPU/CUDA、Linux x64 CPU/CUDA 和 macOS ARM64 CPU；不含 Vulkan/Metal。安装包和 SHA-256 校验值见 [GitHub Release](https://github.com/CialloKing/Mosaicraft/releases/tag/v1.15.8)。

- 新建 CLI、Web UI 与 API 请求默认使用 `fast` 评分；需要沿用原计算语义可显式选 `compatible`。缺少精度字段的旧 Web UI 历史仍按 `compatible` 恢复。`auto` 后端顺序不变；图库与 API 契约未变，现有有效 v1.15 图库无需重建。
- CPU 小图任务减少重复读库，大图任务减少匹配开销。大图跳过数学上不可能相似的邻居；在至少 57,600 格、180×320 原生特征的普通单图上精确合并 Grid/均值求和。已实现的 AVX-512 不作为默认选择。详见[实际 CLI 配对与适用范围](performance/create-49bde5f5b.md)。
- 符合条件的 Ada CUDA 大图减少目标上传并在设备上计算 Canny；未测出墙钟收益的 CPU 邻域过滤不用于 CUDA。同种子 CUDA 大图仍可能出现既有的选图波动。[CUDA 与建库验收](performance/create-182a41ebb.md)区分实测范围与未验证组合。
- 同一 Web UI 进程可重验并复用只读图库数据和索引，不复用过期使用次数。建库和追加在需要重新编码 PNG 时减少压缩开销，保持解码像素，代价是文件可能略大。固定排列输出及热会话的数据另见[生成、输出与热会话验收](performance/create-29ac0f477.md)。Deep Zoom 不计入本轮优化。

Windows CUDA 正式附件须通过本机 GPU 验收；Linux CUDA 附件验证构建、CPU 回退与独立包，Linux GPU 实机状态为 `not-run`。其余 GPU 架构的编译覆盖不等于实机验收。

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

历史稳定版。下载和校验值见 [GitHub Release](https://github.com/CialloKing/Mosaicraft/releases/tag/v1.15.6)。稳定包不因当前分支文档更新而获得后续开发能力。

版本更新与附件验收流程见[发布清单](development/releasing.md)。
