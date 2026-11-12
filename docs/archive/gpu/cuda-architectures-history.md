# CUDA 多架构历史验证

> 历史归档：保留当时的版本、命令与样本，不能视为当前操作或性能结论。返回[历史档案](../README.md)。

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

旧版查询只数设备，新预检会加载上下文和全部内核，部分初始化从后续计算提前到预检；这两列初始化工作量不同。首次进程和各轮原始耗时、浮点最大差异、文件清单与 SHA256 见 [结构化记录](../../performance/cuda-architectures.json)。驱动 JIT 缓存未清除；短任务与单次构建的波动不足以宣称加速，主要收益是兼容范围和提前发现错误。

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

七架构重编四个 CUDA 模块、链接发布/测试目标及显式基准共 57.80 秒（依赖和 CPU 对象已存在）；测试前均已完成构建。模板生成中位数：旧版 67.36 ms、原生 67.52 ms、JIT 66.18 ms；通用尺寸分别为 113.65、112.99、112.84 ms。短任务采样不用于声明普遍加速。完整配置、来源文件 SHA256、首次启动、每轮耗时及文件大小见 [七架构结构化记录](../../performance/cuda-seven-architectures.json)。

七架构 CLI 为 3,372,032 字节；相同运行文件清单、ZIP deflate 等级 6 的归档为 7,478,016 字节，相比单 sm_89 基线 6,128,996 字节增加约 22.0%。该比较不含文档、许可证和 BUILD_INFO，并非正式附件大小。
