# CUDA 多架构支持与验证

## 默认包与配置

默认 CUDA 包包含 sm_75（RTX 20/GTX 16）、sm_80（A100/A30）、sm_86（RTX 30）、sm_89（RTX 40）、sm_90（H100/H200）、sm_100（B200/GB200）、sm_120（RTX 50）七种原生机器码，同时携带 compute_75/80/86/89/90/100/120 七种 PTX。原生机器码优先；PTX 提供更新架构的前向兼容路径。硬件目标依据 [NVIDIA 列表](https://developer.nvidia.com/cuda/gpus)，代码选择遵循 [NVIDIA 兼容规则](https://docs.nvidia.com/cuda/blackwell-compatibility-guide/index.html)。

当前仅 RTX 4060 Laptop / Ada 完成实机验收；其余架构已编译覆盖，尚未实机验证。强制 PTX JIT 验证不代表模拟了其他 GPU。当前源码使用 CUDA 13.5.0 / nvcc 13.5.37；旧驱动不因 GPU 架构受支持而自动兼容，PTX JIT 还要求驱动能处理相应 PTX 版本。[13.5 迁移记录](CUDA_13_5_MIGRATION.md)独立于下文的 13.3 历史采样，已发布的 v1.14.8 保持原工具链和附件。

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

建库和生成在 GPU 工作前查询当前默认设备，通过 cudaFuncGetAttributes 加载四个模块的实际内核，包括四种模板实例，并核对线程数和共享内存限制。无设备、驱动不兼容、缺少内核或预备资源不足时，日志说明原因并在计算前使用 CPU。未知错误及上传/执行/同步失败继续传播。任务内复用预检结果；不引入新 CLI/API 参数、多卡选择或特征版本变化。`--cpu`、CPU-only 构建、仅归一化不会执行 GPU 内核预检。

BUILD_INFO 的 CudaArchitectures、CudaPtx 由构建配置和 cuobjdump 产物核对产生；正式通用包拒绝旧目录遗留的单架构配置。GpuValidationDevice、GpuValidationComputeCapability、GpuValidationDriver 来自 Required 预检，未经 GPU 实机验收的包均为 not-run；Linux 正式附件也保留此状态。这里 Driver 是 cudaDriverGetVersion 返回的 CUDA 驱动 API 版本整数，而非 nvidia-smi 的显卡驱动包版本。passed 仅对应记录的设备。旧包缺少新增字段或使用六架构/单 PTX 元数据时，普通附件验证仍可执行；新包必须通过七架构与对应七种 PTX 的产物检查。

13.5 新增 CudaToolkit、CudaRuntimeMode、GpuValidationRuntime，分别记录编译工具链、运行库加载方式和通过实机预检的运行时版本。运行时版本只在设备枚举成功后读取，无驱动的失败分支不增加任何 CUDA 查询或错误清理调用。

## 六架构初版验证

2026-11-09，Windows 10 22H2 x64、RTX 4060 Laptop、驱动包 616.92、CUDA 13.3.33、MSVC 19.51、OpenCV 4.12.0。CUDA 和 CPU-only Release 构建均通过。原生与 CUDA_FORCE_PTX_JIT=1 模式完成完整 CTest；CUDA_VISIBLE_DEVICES=-1 下普通 CTest 通过且 --require-cuda 返回非零。CPU/CUDA Web UI/API smoke 覆盖提交任务、图片格式和中文/空格路径。

四个模块逐个检查六种 ELF 机器码及单份 PTX，最终 CLI/Web UI 各包含 24 个 cubin 和 4 个 PTX。配置切换 sm_89 -> 通用列表时检查四个对象的更新时间，确认全部重编。关键回归覆盖配置校验、元数据一致性、预检错误分类；既有运行中故障注入继续验证不能静默回退或提交部分结果。

## 六架构初版历史成本与行为对照

以下采样实现为 d9049ed；后续修复 Windows 无驱动时的预检顺序（先枚举设备，再查询驱动详情；失败分支只格式化返回码，不再调用 CUDA 错误查询/清理 API），不修改计算或编译目标。

依赖已安装、CPU 对象保持有效，只改变架构配置，重新编译四个 CUDA 模块并链接四个发布/测试目标。单架构耗时 26.16 秒，通用配置 56.81 秒；这是一次本机采样，不包含依赖冷构建，也不代表 CI 时间。

| 项目 | v1.14.7 单 sm_89 | 通用构建 |
|---|---:|---:|
| CLI 原始字节 | 1,284,608 | 2,739,712 |
| Web UI 原始字节 | 1,714,176 | 3,169,280 |
| 程序及运行库 ZIP 字节 | 6,128,996 | 6,786,149 |

ZIP 比较使用相同文件清单和 Python deflate 等级 6，不含文档、许可证和 BUILD_INFO；这是可比的运行文件归档，非正式发布附件。压缩后增加约 642 KiB（10.7%）。同代码的单 sm_89 CLI 为 1,293,824 字节，约 9 KiB 差异来自预检等新增逻辑。

显式基准脚本 `scripts/benchmark_cuda_architectures.cpp` 链接保留的 v1.14.7 核心/CUDA 静态库和当前静态库各一份。使用独立的 24 张合成素材图库，模板 180×320 和通用 36×64 各一次预热、三次交替采样；每轮从相同数据库快照开始，种子 123，400 块选图并叠加基础图。亮度随机扰动沿用默认关闭，避免其独立随机源影响像素比较。全部采样选图 ID 和输出像素一致；Tiny/LBP/Edge 一致，模板浮点误差保持既有 0.001 容差，评分最大差异小于 0.00001。通用后端另由现有 CPU 精确对照回归验收。

| 三次中位数，毫秒 | 单 sm_89 | 通用构建 |
|---|---:|---:|
| 模板预检/设备查询 | 34.09 | 122.39 |
| 模板生成 | 66.87 | 65.83 |
| 模板总过程 | 206.96 | 197.70 |
| 通用预检/设备查询 | 29.45 | 128.16 |
| 通用生成 | 123.90 | 129.54 |
| 通用总过程 | 250.84 | 268.46 |

旧版查询只数设备，新预检会加载上下文和全部内核，部分初始化从后续计算提前到预检；这两列初始化工作量不同。首次进程和各轮原始耗时、浮点最大差异、文件清单与 SHA256 见 [结构化记录](performance/cuda-architectures.json)。驱动 JIT 缓存未清除；短任务与单次构建的波动不足以宣称加速，主要收益是兼容范围和提前发现错误。

## 七架构与 PTX JIT

当前默认值增加 sm_100，并为七个目标分别保留 PTX。每个 CUDA 模块包含七个 cubin 和七个 PTX，最终 CLI/Web UI 各为 28 个 cubin、28 个 PTX。单架构覆盖仍会生成一份原生代码和对应 PTX。

原生与 JIT 必须分别验证，避免运行成功只是因为另一种代码路径可用：

```powershell
$env:MOSAICRAFT_REQUIRE_CUDA = '1'
$env:CUDA_DISABLE_PTX_JIT = '1'
ctest --test-dir build/cuda -C Release --output-on-failure
Remove-Item Env:CUDA_DISABLE_PTX_JIT
$env:CUDA_FORCE_PTX_JIT = '1'
ctest --test-dir build/cuda -C Release --output-on-failure
Remove-Item Env:CUDA_FORCE_PTX_JIT
Remove-Item Env:MOSAICRAFT_REQUIRE_CUDA
```

变量含义见 [NVIDIA 环境变量文档](https://docs.nvidia.com/cuda/cuda-programming-guide/05-appendices/environment-variables.html)。强制 JIT 会忽略 cubin；禁用 JIT 只能使用兼容 cubin。实际使用时保持默认自动选择，首次 JIT 可能有额外耗时，后续由驱动缓存。

使用通用目标 sm_100/compute_100，不引入 100a/100f 等限制前向兼容的特殊目标。PTX 不会让旧驱动获得读取新 PTX ISA 的能力，也不提供旧显卡缺少的硬件功能；兼容性仍取决于设备、驱动及实际内核加载。只有本机 Ada 已实机验收，其他架构保持“已编译覆盖、尚未实机验证”。

七架构本机 Release、禁用 JIT 与强制 JIT 两组完整 CTest，以及 CPU-only CTest 均通过。私有 24 张样本、两种图库尺寸各预热一次后三次采样，旧单 sm_89、七架构原生、七架构 JIT 共 24 轮的 400 块选图 ID 和输出像素全部一致，模板误差保持既有容差。

七架构重编四个 CUDA 模块、链接发布/测试目标及显式基准共 57.80 秒（依赖和 CPU 对象已存在）；测试前均已完成构建。模板生成中位数：旧版 67.36 ms、原生 67.52 ms、JIT 66.18 ms；通用尺寸分别为 113.65、112.99、112.84 ms。短任务采样不用于声明普遍加速。完整配置、来源文件 SHA256、首次启动、每轮耗时及文件大小见 [七架构结构化记录](performance/cuda-seven-architectures.json)。

七架构 CLI 为 3,372,032 字节；相同运行文件清单、ZIP deflate 等级 6 的归档为 7,478,016 字节，相比单 sm_89 基线 6,128,996 字节增加约 22.0%。该比较不含文档、许可证和 BUILD_INFO，并非正式附件大小。
