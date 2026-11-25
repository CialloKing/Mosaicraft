# GPU 后端与精度模式

v1.15.19 的 Windows/Linux 双后端包可选 CPU、CUDA、Vulkan，macOS ARM64 包使用 CPU。Metal 可从源码启用。实际设备及驱动状态可用 `mosaicraft devices --json` 检查。[下载与运行](quick-start.md#下载与运行)

## 选择后端与设备

```sh
./mosaicraft devices --json
./mosaicraft library build photos -o library --size 180x320 -r --backend auto
./mosaicraft create target.jpg -l library -o result.png --backend vulkan --device vulkan:0 --score-mode fast
```

设备标识必须从当前机器的 devices 输出中选择，示例 vulkan:0 不保证是你的目标 GPU。建库、追加和生成均支持 --backend 和 --device，生成另支持 --score-mode。

| 后端 | 可用范围 | 当前 auto 策略 |
| --- | --- | --- |
| CPU | 所有原生平台 | CPU 包默认使用，双后端包的回退路径 |
| CUDA | Windows/Linux 双后端包，搭配兼容的 NVIDIA 驱动 | 双后端包优先使用 |
| Vulkan | Windows/Linux 双后端包，搭配兼容 Vulkan 驱动 | 在双后端包中显式选择 |
| Metal | macOS 源码构建 | 显式选择 |

`auto` 仅在设备不可用或计算资源准备失败时回退 CPU。显式 GPU 请求遇到同样条件会报错，上传、执行或同步错误也会结束任务。一个任务的 GPU 提取和 GPU 评分共用设备。Vulkan 计算动态加载系统驱动，需要 Vulkan 1.1 和足够的计算能力。

## 智能裁剪推理

`--backend` 和 `--device` 控制建库特征提取及生成匹配。`--crop-device` 独立控制智能裁剪推理。用 CUDA 裁剪并用 Vulkan 提取新图库特征的示例：

```sh
./mosaicraft library build photos -o library --size 180x320 --backend vulkan --crop smart-real --crop-device cuda
./mosaicraft library normalize photos -o normalized --size 180x320 --crop smart-anime --crop-device vulkan
```

第二条命令使用双后端智能裁剪包的 Vulkan 推理。独立归一化可另选实验性 `--crop-device cuda+vulkan`。[智能裁剪与双 GPU 的适用条件](../SMART_CROP_VULKAN.md)

## 精度模式

| 模式 | CPU / CUDA | Vulkan / Metal |
| --- | --- | --- |
| fast（默认） | CPU、CUDA 以 FP32 计算五特征加权分数 | GPU 返回每候选一个 FP32 加权分数 |
| compatible（显式选择） | 保留各后端现有特征和评分行为 | GPU 整数统计，CPU 兼容收尾与 FP64 评分 |

切换 fast 与 compatible 不改变图库格式。使用 1.15.12 前创建的图库时，需要在新目录重建。fast 按格减去共同的使用次数再合成历史分，数值跨度大时使用高精度合成。不同精度可能选出不同素材；邻域和最终选图仍按顺序执行。历史请求缺少精度字段时恢复为 compatible。2026-11-13 的验收只适用于旧评分实现，不能作为新路径的准入凭据。

## 当前验收结论

RTX 4060 Laptop 的 CUDA/Vulkan 与 Radeon 780M 的 Vulkan 有本机记录。包内 `BUILD_INFO.txt` 记录该包的验收设备和状态。Intel、Linux GPU 和 Apple GPU 的实机覆盖见相应验收记录。

任务和预览界面显示实际后端、设备与评分路径，兼容模式的 CPU 评分属于正常执行。详见[实现、验收与性能数据](../development/portable-gpu.md)。

CPU、CUDA、RTX 4060 Laptop 和 Radeon 780M 的性能比较见 [2026-11-18 小任务多轮复测](../development/backend-retest-20261118.md)与[重负载复测](../development/backend-scale-20261118.md)，分别覆盖 900 / 3,600 格，以及 14,400 / 57,600 格和更高候选评分负载。报告保留运行波动及输出差异，不能用单一场景代替所有任务。

## CUDA 尺寸模板

CUDA 按图库归一化尺寸（`library build --size`，宽×高）选择模板，与目标图或成品分辨率无关。

| 比例 | 模板尺寸 |
| --- | --- |
| 正方形 | 256×256、512×512、1024×1024 |
| 9:16 / 16:9 | 144×256、180×320、270×480、360×640、432×768、540×960，以及各自的横版 |
| 3:4 / 4:3 | 384×512、512×384 |

其他合法尺寸使用通用 CUDA 路径。新增模板不会重算现有图库的特征；新旧提取路径的数值可能不同。不要为使用 GPU 任意改变已有图库规格。

`1024×1024` 模板按列分组，每张图使用 256 个线程。模板 CUDA 建库和追加按像素字节数限制每批张数，`1024×1024` 时每批最多 85 张。

工具链、架构和编译选项见[CUDA 配置](../development/cuda.md)，构建步骤见[源码构建](../development/building.md)。
