# GPU 后端与精度模式

当前源码提供 CPU、CUDA、Vulkan、Metal。v1.15.9 在 Windows/Linux x64 提供独立的 Vulkan 包；CPU/CUDA 包不包含 Vulkan，正式包均不包含 Metal。已安装包只支持构建时启用的后端，查看包内 `BUILD_INFO.txt` 和 `mosaicraft devices --json` 确认。

## 选择后端与设备

```sh
./mosaicraft devices --json
./mosaicraft library build photos -o library --size 180x320 -r --backend auto
./mosaicraft create target.jpg -l library -o result.png --backend vulkan --device vulkan:0 --score-mode fast
```

设备标识必须从当前机器的 devices 输出中选择，示例 vulkan:0 不保证是你的目标 GPU。建库、追加和生成均支持 --backend 和 --device，生成另支持 --score-mode。

| 后端 | 可用范围 | 当前 auto 策略 |
| --- | --- | --- |
| CPU | 所有原生平台 | GPU 不可用时的默认路径 |
| CUDA | NVIDIA，需对应 CUDA 包与兼容驱动 | 保留现有优先策略 |
| Vulkan | 编译启用后，按实际驱动枚举 | Vulkan 包优先，源码候选默认显式 |
| Metal | macOS，编译启用后 | 显式候选，实机未验收 |

`auto` 随发布包而定：Vulkan 包走 Vulkan → CPU（不尝试 CUDA 或软件 Vulkan），CUDA 包走 CUDA → CPU，CPU 包只用 CPU。自动回退只发生在设备不可用或计算资源准备失败时；显式 GPU 请求在相同情况下报错，上传、执行或同步失败也会传播。一个任务的 GPU 提取与 GPU 评分使用同一设备。Vulkan 包动态加载系统 Vulkan 驱动，需要 Vulkan 1.1 和足够的计算能力，不要求安装 SDK。

## 精度模式

| 模式 | CPU / CUDA | Vulkan / Metal |
| --- | --- | --- |
| fast（默认） | CPU、CUDA 以 FP32 计算五特征加权分数 | GPU 返回每候选一个 FP32 加权分数 |
| compatible（显式选择） | 保留各后端现有特征和评分行为 | GPU 整数统计，CPU 兼容收尾与 FP64 评分 |

fast 不改变图库或 FeaturePack 格式，无须重建现有版本 1 图库。fast 按格减去共同的使用次数再合成历史分，数值跨度大时使用高精度合成。不同精度可能选出不同素材；邻域和最终选图仍按顺序执行。历史请求缺少精度字段时恢复为 compatible。2026-11-13 的验收只适用于旧评分实现，不能作为新路径的准入凭据。

## 当前验收结论

RTX 4060 Laptop 的 CUDA/Vulkan 与 Radeon 780M 的 Vulkan 有本机记录。Vulkan 包优先选择 Vulkan 是按包区分的默认策略，不代表 GPU 在每种任务上都更快。Intel、Linux GPU 和 Apple GPU 实机结果仍为 `not-run`；编译和 CPU 回退通过不等于硬件验收。

任务和预览界面显示实际后端、设备与评分路径，兼容模式的 CPU 评分属于正常执行。详见[实现、验收与性能数据](../development/portable-gpu.md)。

CPU、CUDA、RTX 4060 Laptop 和 Radeon 780M 的性能比较见 [2026-11-18 小任务多轮复测](../development/backend-retest-20261118.md)与[重负载复测](../development/backend-scale-20261118.md)，分别覆盖 900 / 3,600 格，以及 14,400 / 57,600 格和更高候选评分负载。报告保留运行波动及输出差异，不能用单一场景代替所有任务。

## CUDA 尺寸模板

当前模板覆盖 256×256、512×512；竖屏 180×320、270×480、360×640、540×960；横屏 320×180、480×270、640×360、960×540。其他合法尺寸使用通用 CUDA 路径。不要为使用 GPU 任意改变已有图库规格。

工具链、架构和编译选项见[CUDA 配置](../development/cuda.md)，构建步骤见[源码构建](../development/building.md)。
