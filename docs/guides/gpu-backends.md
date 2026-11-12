# GPU 后端与精度模式

当前源码提供 CPU、CUDA、Vulkan、Metal。已安装包只包含它构建时启用的后端；先查看包内 `BUILD_INFO.txt` 和设备发现结果。

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
| Vulkan | 编译启用后，按实际驱动枚举 | 显式候选，未进入 auto |
| Metal | macOS，编译启用后 | 显式候选，实机未验收 |

auto 当前仍为 CUDA → CPU。显式 GPU 请求不可用或准备资源不足时失败，不会切换到另一个 GPU 后端；上传、执行或同步失败会传播。一个任务的 GPU 提取与 GPU 评分使用同一设备。

## 精度模式

| 模式 | CPU / CUDA | Vulkan / Metal |
| --- | --- | --- |
| compatible（默认） | 保留现有特征和评分行为 | GPU 整数统计，CPU 兼容收尾与 FP64 评分 |
| fast（显式选择） | 首期沿用已有评分实现 | GPU FP32 五距离，CPU FP64 权重和惩罚合成 |

fast 不改变图库或 FeaturePack 格式，无须重建现有版本 1 图库。固定验收集同时要求每场景、每种子的五特征误差 mean/p90/p99 退化不超过 1%，最终素材 ID 相对 FP64 差异不超过 1%。少于 100 格的场景必须选图完全一致。这些是固定样本验收门槛，不是对任意输入的数学保证。

## 当前验收结论

RTX 4060 Laptop 的 CUDA/Vulkan 与 Radeon 780M 的 Vulkan 已有本机记录。当前真实图库样本中 Vulkan 未达到自动选择性能门槛，因此继续显式启用。Intel、Linux GPU 和 Apple GPU 实机结果为 not-run；编译覆盖不等于硬件验收。

任务和预览界面显示实际后端、设备与评分路径，兼容模式的 CPU 评分属于正常执行。详见[实现、验收与性能数据](../development/portable-gpu.md)。

## CUDA 尺寸模板

当前模板覆盖 256×256、512×512；竖屏 180×320、270×480、360×640、540×960；横屏 320×180、480×270、640×360、960×540。其他合法尺寸使用通用 CUDA 路径。不要为使用 GPU 任意改变已有图库规格。

工具链、架构和编译选项见[CUDA 配置](../development/cuda.md)，构建步骤见[源码构建](../development/building.md)。
