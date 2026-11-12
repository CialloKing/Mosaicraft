# CUDA 多架构支持与验证

## 默认包与配置

默认 CUDA 包包含 sm_75（RTX 20/GTX 16）、sm_80（A100/A30）、sm_86（RTX 30）、sm_89（RTX 40）、sm_90（H100/H200）、sm_100（B200/GB200）、sm_120（RTX 50）七种原生机器码，同时携带 compute_75/80/86/89/90/100/120 七种 PTX。原生机器码优先；PTX 提供更新架构的前向兼容路径。硬件目标依据 [NVIDIA 列表](https://developer.nvidia.com/cuda/gpus)，代码选择遵循 [NVIDIA 兼容规则](https://docs.nvidia.com/cuda/blackwell-compatibility-guide/index.html)。

当前仅 RTX 4060 Laptop / Ada 完成实机验收；其余架构已编译覆盖，尚未实机验证。强制 PTX JIT 验证不代表模拟了其他 GPU。当前源码使用 CUDA 13.5.0 / nvcc 13.5.37；旧驱动不因 GPU 架构受支持而自动兼容，PTX JIT 还要求驱动能处理相应 PTX 版本。[13.5 迁移记录](../archive/gpu/cuda-13-5-migration.md)与[13.3 历史采样](../archive/gpu/cuda-architectures-history.md)分开保存，已发布的 v1.14.8 保持原工具链和附件。

```powershell
# 新构建目录默认通用配置
$env:CUDAToolkit_ROOT = 'C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v13.5'
cmake --preset windows-cuda
# 开发者可只构建 RTX 30；保留原参数形式
cmake --preset windows-cuda -B build/rtx30 -DCUDA_ARCH=sm_86
# 覆盖旧目录中的 sm_89 缓存（PowerShell 需要引号保护分号）
cmake --preset windows-cuda "-DCUDA_ARCH=sm_75;sm_80;sm_86;sm_89;sm_90;sm_100;sm_120"
```

CUDA_ARCH 去重并按计算能力排序；每个原生目标同时生成对应 PTX，驱动在 JIT 时选择匹配的虚拟架构。四个手动 nvcc 命令共享生成参数，架构/编译器配置文件是对象依赖；变更目标会重新编译全部四个模块。非法目标在配置时失败。兼容评分仍关闭 FMA 并保留精确除法/平方根、非 FTZ 参数，模板算法与其原编译选项不变。

## 预检及验收信息

建库和生成在 GPU 工作前检查所选设备、实际内核及线程数和共享内存限制。auto 在预检或资源准备不可用时可记录原因并使用 CPU；显式 --backend cuda 不可用时失败。上传、执行或同步错误继续传播，任务内复用预检结果。--backend cpu、CPU-only 构建与仅归一化不执行 CUDA 内核预检。设备选择与错误语义以[GPU 指南](../guides/gpu-backends.md)为准。

BUILD_INFO 的 CudaArchitectures、CudaPtx 由构建配置和 cuobjdump 产物核对产生；正式通用包拒绝旧目录遗留的单架构配置。GpuValidationDevice、GpuValidationComputeCapability、GpuValidationDriver 来自 Required 预检，未经 GPU 实机验收的包均为 not-run；Linux 正式附件也保留此状态。这里 Driver 是 cudaDriverGetVersion 返回的 CUDA 驱动 API 版本整数，而非 nvidia-smi 的显卡驱动包版本。passed 仅对应记录的设备。旧包缺少新增字段或使用六架构/单 PTX 元数据时，普通附件验证仍可执行；新包必须通过七架构与对应七种 PTX 的产物检查。

13.5 新增 CudaToolkit、CudaRuntimeMode、GpuValidationRuntime，分别记录编译工具链、运行库加载方式和通过实机预检的运行时版本。运行时版本只在设备枚举成功后读取，无驱动的失败分支不增加任何 CUDA 查询或错误清理调用。

历史测量与旧环境说明已移至[CUDA 多架构历史验证](../archive/gpu/cuda-architectures-history.md)。
