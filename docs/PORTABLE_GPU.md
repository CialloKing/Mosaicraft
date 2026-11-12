# 四后端实现与验收

日期：2026-11-13；目标版本：1.16.0；API：1.5。数据库、FeaturePack 和特征定义仍为版本 1，现有 v1.15 图库可直接使用。

## 执行方式

| 后端 | compatible（默认） | fast（显式） | 自动选择 |
|---|---|---|---|
| CPU | 原提取、FP64 评分 | 保留原评分 | 兜底 |
| CUDA | 原模板/通用实现 | 保留原评分 | 保留历史策略 |
| Vulkan | GPU 整数统计 + CPU 收尾/FP64 评分 | GPU 整数统计 + FP32 五距离 + CPU FP64 合成 | 暂关闭 |
| Metal | 与 Vulkan 相同数据契约 | MSL 五距离 + CPU FP64 合成 | 暂关闭，候选 |

`mosaicraft devices --json` 或 `/api/info.devices` 返回实际设备、能力与不可用原因。建库、追加和生成使用 `--backend`、`--device`；生成和预览使用 `--score-mode`。后端与设备冲突会被拒绝。GPU 提取与评分使用同一选定设备，兼容模式的 CPU 评分是正常计划。

可用设备不等于完成包验收。设备编号属于当前机器的枚举结果，不是跨机器标识。Metal、Intel 和 Linux GPU 的实机结果必须单独补齐；编译通过不等于硬件通过。新后端当前均显式选择，尚无自动选择任务范围。

## 计算与资源边界

- `ComputeBackend` 隔离设备和会话，保留既有 CUDA 工作区；`PortableCompute` 共享预处理、收尾与数据布局，原生句柄不进入业务层。
- CPU 完成 OpenCV 颜色转换、Tiny、Canny；GPU 每块最多处理 4096 像素，使用有界 uint32 统计 LAB/Grid/LBP；CPU uint64 合并，再按现有舍入、LBP 饱和与归一化规则收尾。
- 评分布局固定为 518 个 uint32；Tiny 每四字节打包，不要求 8 位存储或 FP64/64 位原子操作。所有 ANN 候选均计算，不增加 GPU Top-K。
- FP32 距离有独立 CPU 参考；权重、历史使用惩罚、邻域处理、排序和随机选择复用原流程。缺失必需特征继续按现有 FeaturePack 规则拒绝；无效候选使用原最大分数语义。
- 工作区按任务复用，图库常驻单独计量；图库主机打包以最多 4096 行分段上传，避免额外分配整库副本。设备范围、工作组能力和实际分配均检查。
- 核显使用映射/共享缓冲。`gpuSharedMemoryBytes` 与 `gpuResidentBytes` 指向同一份分配，不将它们再次相加。主机工作区、GPU 缓冲和进程 RSS 分别报告；驱动不可取得的指标保持不可用。
- Vulkan 统计和距离输出优先使用主机可缓存内存；读回使用非缓存 BAR 内存会让 CPU 下载远慢于内核。设备选择只探测所选后端，完整枚举留给设备发现，避免无关驱动初始化拖慢 CUDA 启动。
- `gpuComputeWaitMs` 包括命令提交和同步等待。当前没有独立设备时间戳，不能将该项解释为纯内核时间。
- 准备失败时显式 GPU 请求返回退出码 4；上传、执行和同步错误传播到任务，失败批次不提交事务，不静默重算。输出沿用原有原子发布流程。

## 固定验收集

沿用 doctest、回归、Web UI smoke、`mosaicraft_match_benchmark` 和 `mosaicraft_quality_benchmark`。新增参数化用例涵盖小于 8 像素、非整除尺寸、横竖长边、非连续 ROI、尾批、资源不足、无效候选、平分与缺失特征。

真实图库使用 45,613 张素材的私有数据库副本和只读素材链接；原图库哈希在验收前后检查。三类目标为 8、400、900 格，各使用种子 42/20261113 和 Top-N 1/10，在 RTX 4060 Laptop 与 Radeon 780M 分别验证 compatible/fast。每次固定图库使用计数、输入、候选生成规则和选图参数。

快速模式同时要求：

1. 每个场景、种子的五特征误差 mean、p90、p99 分别满足 `fast <= reference * 1.01 + 1e-6`。
2. 最终素材 ID 不同的格数 `<= floor(total / 100)`，包括未匹配状态。8 格场景必须完全一致；另检查未匹配数量。

当前真实图库的 48 项 compatible/fast 对照全部通过；其中 24 项 fast 对照总计 10,464 次逐格比较，素材 ID 差异为 0，mean/p90/p99 与 FP64 参考相同。兼容模式逐格一致。非模板 31×17 和模板 180×320 的合成图库场景也通过两块 GPU 对照。结果仅适用于固定验收集。

机器可读记录见 [验收摘要](performance/portable-gpu-20261113.json)。该记录同时报告素材复用、邻域重复和差异位置。

## 性能与准入

冷启动单独记录；热路径预热一次，使用五组交替对照的中位数。性能报告包括完整任务及匹配阶段；计时含整库上传和管线准备，不能只选择内核时间宣称加速。

45,613 张真实图库、900 格照片场景的最终结果如下，单位为毫秒。Vulkan 分别与各自交替采样的 CPU 对照比较；CUDA 与原版 `64501626d` 的相同源码构建比较。

| 对照 | 匹配阶段中位数 | 端到端中位数 | 结果 |
|---|---:|---:|---|
| 原版 CUDA | 530.16 | 2244.13 | 基线 |
| 当前 CUDA | 530.63 | 2309.91 | +0.09% / +2.93%，通过 5% 门槛 |
| CPU（NVIDIA 对照） | 1113.65 | 2317.64 | 基线 |
| RTX 4060 Laptop Vulkan fast | 1599.04 | 2997.05 | 未达到自动选择准入 |
| CPU（AMD 对照） | 1099.28 | 2291.12 | 基线 |
| Radeon 780M Vulkan fast | 1497.16 | 2890.08 | 未达到自动选择准入 |

读回优化显著降低了统计下载开销，但当前固定任务的初始化、主机预处理及整库上传仍超过 GPU 计算收益，Vulkan 保留显式候选。8 格历史采样在报告中单独标记为优化前结果；不能据此外推更大任务。

耗时对照统一关闭资源监控，避免原版基准工具未启动采样造成不对称开销；内存通过独立的质量阶段采样。本机 Vulkan 匹配过程的进程 RSS 采样峰值分别为 369.2 MiB / 330.2 MiB，设备实际分配峰值约 154.4 MiB。核显共享缓冲不与进程 RSS 再相加；未取得的驱动利用率及显存监控保持不可用。

报告保留修复前的 CUDA 启动回归及不可比的监控样本，不使用它们作最终准入结论。最终所有质量门槛和 CUDA 性能门槛通过，新后端自动选择仍关闭。Intel、Linux GPU 和 Apple GPU 的实机结果保持 `not-run`。

## 构建与候选包

CPU 构建不引入 GPU SDK。Vulkan 使用 1.1 基线，动态加载系统驱动，GLSL 离线编译并嵌入 SPIR-V。Windows 的 `scripts/setup-vulkan.ps1` 固定 Vulkan-Headers 提交及 glslang 16.6.0 下载哈希。Metal 仅在 macOS 使用完整 Xcode 的 metal/metallib 工具离线编译并嵌入，无快速数学和浮点收缩。

新增 presets：`windows-vulkan`、`windows-cuda-vulkan`、`linux-vulkan`、`macos-metal`。新增 `Portable GPU candidates` CI，复用发布、CTest、smoke 和归档校验。`release.ps1 -NoCuda -Vulkan`、`release.ps1 -Vulkan` 或 macOS 的 `release.ps1 -NoCuda -Metal` 均生成 `_candidate` 包，记录 `PortableGpuValidation: not-run` 和 `PortableAutoSelection: disabled`。包状态不能替代本机限定范围的验收记录。

使用固定清单复现：

```text
python scripts/verify-compute.py manifest.json --benchmark <mosaicraft_match_benchmark> --quality <mosaicraft_quality_benchmark> --output build/private-acceptance --performance
```

清单包括源数据库 `source`、目标 `cases`（id/input/width/height）、设备 `devices`、`seeds` 和 `top_n`。工具创建私有副本，输出选择序列、逐项质量、差异位置、冷/热计时和通过状态；不满足任一门槛时返回失败。使用 `--performance-case photo` 指定性能场景，使用 `--baseline-benchmark <原版基准程序>` 执行 CUDA 的五组交替端到端回归门槛。
