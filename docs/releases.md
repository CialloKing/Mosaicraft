# 版本状态

本页区分当前源码与公开安装包，详细历史记录见[历史档案](archive/README.md)。软件与数据版本的唯一来源是根目录 contracts.json；安装后的实际能力以该包 BUILD_INFO.txt、contracts.json 和 --help-all 为准。

## v1.15.11

提供三个 CPU 版 Qt 桌面包（Windows x64、Linux x64、macOS ARM64），以及两个 CUDA + Vulkan 双后端 Qt 桌面包（Windows x64、Linux x64）。原来的七个 Web UI / CLI 包保持不变。[下载与校验值](https://github.com/CialloKing/Mosaicraft/releases/tag/v1.15.11) · [Qt 桌面包说明](development/packaging.md#qt-桌面发布包)

- 桌面应用支持预览、逐格换图、保存及重开 `.mscproj` 工程后导出。桌面与 Web 共用换图校验和候选评分规则；已有桌面工程不需要迁移。
- 源码构建可启用 `MOSAICRAFT_BUILD_DESKTOP`，默认关闭。Qt 桌面包随附 Qt 运行库、平台插件与许可说明，不依赖系统 Qt。
- `--target-size` 支持像素、单轴 `auto` 和百分比；移除 `--target-upscale`。HTTP API 1.6 改用 `targetSize`，不再接受 `upscale`。已导出的旧两段缩放工程仍可离线浏览，但新版检查和回导要求重新生成。详见[命令行指南](guides/cli.md)及 [API 迁移说明](reference/api.md)。

## v1.15.10

本版提供七个原生包：Windows x64 与 Linux x64 各提供 CPU、CUDA、Vulkan，macOS ARM64 提供 CPU。归档及 SHA-256 见 [GitHub Release](https://github.com/CialloKing/Mosaicraft/releases/tag/v1.15.10)。

- 建库和追加对同尺寸的 JPG/JPEG、PNG、WebP 逐字节直拷，保留原格式和后缀拼写。BMP 和 TIFF 输入统一转为 PNG，历史图库仍可使用。原地输入需要转换或缩放时，使用独立输出目录。
- CLI、API 和 Web UI 支持设置建库编码。JPG 质量默认 100，WebP 默认无损，PNG 压缩默认 9，范围为 0–9。编码设置只作用于重新编码的素材。见[命令行指南](guides/cli.md)。
- `--reuse-penalty` 默认 0。关闭时匹配跳过读取图库历史使用计数，生成仍更新使用统计。
- Web UI 显示预览的图库读取进度，改进建库到生成的操作流程、路径选择和手动换图撤销，并在 Deep Zoom 查看器加载失败时给出提示。
- x64 的 CPU `fast` 评分在 CPU 和操作系统支持时自动优先选择 AVX-512，不支持则继续尝试 AVX2 或标量；ARM64 的 NEON 策略不变。这不改变 `auto` 的设备选择顺序。
- `--threads 0` 对至少 14,400 格的普通 CPU `fast` 单图，在批次有足够并行工作且访问表内存预算允许时，为 ANN 和基础评分使用更多逻辑线程。手动线程设置、小图、预览和 GPU 路径沿用原线程策略。 详见[分派与线程配对验证](performance/cpu-auto-7940h-20261119.md)。

## v1.15.9

本版共七个原生包：Windows x64 和 Linux x64 各提供 CPU、CUDA、Vulkan，macOS ARM64 继续提供 CPU。归档及 SHA-256 见 [GitHub Release](https://github.com/CialloKing/Mosaicraft/releases/tag/v1.15.9)。

- 新增 Vulkan 正式包：建库、匹配和 Web UI 预览默认优先使用可用的 Vulkan GPU；无合格设备或预备工作区失败时自动使用 CPU。显式 `--backend vulkan` 仍是强制要求，执行中的 GPU 错误不会静默重试。CUDA 包的 CUDA → CPU 策略不变，Metal 仍不随正式包发布。
- Windows CPU 快速预览按可用内存选择缓存档，最高 1664 MiB，按需分配并保留低内存回退。改 Top-N 可复用评分表，改权重可复用目标特征和 ANN 候选。详见[预览缓存](guides/web-ui.md#预览缓存)。
- Windows CPU 快速评分运行时优先选择支持的 AVX-512 路径；增加大图自动匹配并行度、同层 Deep Zoom 写出并行度与运行诊断。图库 PNG 归一化采用最高无损压缩等级；同尺寸 PNG 仅在副本更小时替换。
- Vulkan 包在无可用驱动的独立环境验证 CPU 回退；`BUILD_INFO.txt` 的包级 GPU 状态 `not-run` 不等同于硬件验收。图库、便携工程及 API 1.5 兼容性不变。

## v1.15.8

本版主要缩短普通单图生成时间。五个标准包仍为 Windows x64 CPU/CUDA、Linux x64 CPU/CUDA 和 macOS ARM64 CPU；不含 Vulkan/Metal。安装包和 SHA-256 校验值见 [GitHub Release](https://github.com/CialloKing/Mosaicraft/releases/tag/v1.15.8)。

- 新建 CLI、Web UI 与 API 请求默认使用 `fast` 评分；需要沿用原计算语义可显式选 `compatible`。缺少精度字段的旧 Web UI 历史仍按 `compatible` 恢复。`auto` 后端顺序不变；图库与 API 契约未变，现有有效 v1.15 图库无需重建。
- CPU 小图任务减少重复读库，大图任务减少匹配开销。大图跳过数学上不可能相似的邻居；在至少 57,600 格、180×320 原生特征的普通单图上精确合并 Grid/均值求和。已实现的 AVX-512 不作为默认选择。详见[实际 CLI 配对与适用范围](performance/create-49bde5f5b.md)。
- 符合条件的 Ada CUDA 大图减少目标上传并在设备上计算 Canny；未测出墙钟收益的 CPU 邻域过滤不用于 CUDA。同种子 CUDA 大图仍可能出现既有的选图波动。[CUDA 与建库验收](performance/create-182a41ebb.md)区分实测范围与未验证组合。
- 同一 Web UI 进程可重验并复用只读图库数据和索引，不复用过期使用次数。建库和追加在需要重新编码 PNG 时减少压缩开销，保持解码像素，代价是文件可能略大。固定排列输出及热会话的数据另见[生成、输出与热会话验收](performance/create-29ac0f477.md)。Deep Zoom 不计入本轮优化。

Windows CUDA 正式附件须通过本机 GPU 验收；Linux CUDA 附件验证构建、CPU 回退与独立包，Linux GPU 实机状态为 `not-run`。其余 GPU 架构的编译覆盖不等于实机验收。

## v1.15.7

本版保持五个标准包：Windows x64 CPU/CUDA、Linux x64 CPU/CUDA、macOS ARM64 CPU。Vulkan 和 Metal 通过条件编译关闭，不另发新后端包；Vulkan 用户须[自行从源码启用编译](development/building.md#gpu-后端与编译配置)。下载与校验值见 [GitHub Release](https://github.com/CialloKing/Mosaicraft/releases/tag/v1.15.7)。

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
