# CUDA 13.5 迁移验证

本轮统一本机和 Windows/Linux CI 的构建工具链与 Preset，不增加显卡目标，不修改特征 v6、API 合约 2.0 或图库格式。已发布的 v1.14.8、标签和附件保持不变；本文记录其后的源码迁移，不代表发布新版本。

## 工具链与入口

CUDA Toolkit 13.5.0 的 nvcc、cudart、cuobjdump 固定为 13.5.37；CCCL 的官方组件版本为 13.3.5.0.6，不是旧 SDK 混入。六个最小组件的 Windows/Linux 地址和 SHA256 来自 [NVIDIA 官方清单](https://developer.download.nvidia.com/compute/cuda/redist/redistrib_13.5.0.json)，保存在 `cmake/cuda-13.5.json`。原 13.3 锁文件和性能记录保留历史用途，安装脚本只使用新锁。

五组平台 Preset 共用 Release-only 依赖目录，通过 VCPKG_ROOT 和 CUDAToolkit_ROOT 显式选择工具链；CPU-only 不查找 CUDA。主入口要求 CMake 4.2+、PowerShell 7，Windows 使用 VS 2026，Linux/macOS 使用 Ninja。Windows default/release/relwithdebinfo 入口仍可用；旧 CMakeSettings.json 已移除。发布脚本复用 Preset，允许覆盖构建目录和 vcpkg toolchain。

Linux hosted runner 的 CMake 3.31 无法读取新入口，CI 在安装依赖后复用 vcpkg 固定的 CMake。依赖缓存指纹和共享策略不因 CUDA 版本变化而改变。首次远端迁移试跑还遇到 GitHub vcpkg 下载连续 HTTP 504；它与编译或测试失败分开记录。

配置阶段检查实际 nvcc 版本、所用头文件版本，以及缓存中的编译器、头文件、cudart 路径是否位于同一 SDK。旧 13.3 编译器和指向其他 SDK 的缓存明确失败，不能自动混用。新增缓存边界测试不依赖 CUDA 安装，因此 CPU CI 也覆盖此规则。

Windows 保留 CUDA::cudart 的 hybrid 加载方式，从显示驱动取得实际运行时，不无条件复制 SDK DLL，也不随包分发驱动；Linux 保留 shared 和相对 RPATH。[NVIDIA 运行库加载说明](https://docs.nvidia.com/cuda/cuda-compiler-driver-nvcc/index.html#cudart-none-shared-static-hybrid-cudart)。BUILD_INFO 新增 CudaToolkit、CudaRuntimeMode、GpuValidationRuntime；历史包缺少整组字段时仍可验证，部分字段或与 GPU 验收状态冲突则失败。Driver 和 Runtime 是 CUDA API 版本整数，不是驱动包版本。

## 本机正确性

2026-11-09，Windows 10 22H2 x64、RTX 4060 Laptop（sm_89）、驱动包 621.14、MSVC 19.51.36257、OpenCV 4.12.0。13.5 验收时 Driver API 和 Runtime API 均为 13050。

| 检查 | 结果 |
| --- | --- |
| CPU-only Release、完整 CTest、Web UI/API smoke | 通过 |
| CUDA Release、GPU Required、禁用 PTX JIT 的完整 CTest 与 smoke | 通过 |
| 强制 PTX JIT 的 GPU Required、完整 CTest 与 smoke | 通过 |
| cuobjdump 逐模块及最终 CLI 检查 | 通过，每模块 7 ELF/7 PTX，最终 28 ELF/28 PTX |
| 13.3 编译器拒绝、SDK 缓存边界、元数据及历史兼容测试 | 通过 |
| 当前驱动下的 13.3 基线完整 GPU 测试 | 59 核心 + 30 回归用例通过 |

13.5 的两轮 CTest 各通过 5 项入口测试，包含 59 核心与 30 回归用例。通用特征和评分继续逐字段精确对照 CPU；已有运行中故障注入仍要求传播错误，不能中途静默切换后端。无驱动时先枚举设备，失败分支不调用版本、错误名称或清理 API；新增运行时版本查询仅发生在成功枚举设备后。

13.3 对照保留七架构编译产物，使用同一当前驱动和相同 OpenCV/运行库文件。固定 24 张合成素材的私有图库快照，模板 180×320 和通用 36×64 两种规格；每次恢复数据库，种子 123，Top-N 为 3、叠加强度 0.1，亮度随机扰动关闭。13.3/13.5 各原生/JIT、两种规格、一次预热和三次测量，共 32 次，全部 400 块选图 ID 与解码输出像素一致。

显式架构基准输出的 LAB/Grid/评分数组来自模板核，即使生成使用通用图库也如此；其最大差异分别为 0.00003052、0.00004578、0.00000006223，满足既有 0.001/0.00001 标准，Tiny/LBP/Edge 完全一致。通用特征和评分的精确性由两个 SDK 的完整 GPU 回归另行覆盖，不能将模板容差用于通用后端。

## 成本采样

下面是短任务样本，不作普遍提速结论。独立 JIT 缓存按 SDK 和执行模式分开；首次模板 JIT 从空缓存开始。进程预检首次耗时：13.3 原生 140.67 ms、13.5 原生 137.72 ms；13.3 JIT 1172.88 ms、13.5 JIT 888.26 ms。这包含上下文和内核加载，不是单个内核的编译时间。

| 预热后三次中位数 | 13.3 原生 | 13.5 原生 | 13.3 JIT | 13.5 JIT |
| --- | ---: | ---: | ---: | ---: |
| 模板生成，ms | 67.30 | 64.37 | 65.31 | 63.52 |
| 模板进程墙钟，ms | 291.20 | 373.27 | 374.66 | 287.72 |
| 模板峰值工作集，MiB | 186.02 | 185.78 | 185.75 | 185.96 |
| 通用生成，ms | 134.41 | 135.30 | 134.03 | 134.74 |
| 通用进程墙钟，ms | 431.65 | 358.93 | 383.75 | 430.04 |
| 通用峰值工作集，MiB | 148.38 | 147.99 | 148.17 | 148.57 |

峰值内存读取 Windows GetProcessMemoryInfo 的 PeakWorkingSetSize；不包含显存。短进程墙钟受启动及退出开销影响，不能只选较快的一列描述迁移收益。

已有依赖和 CPU 对象下，重编四个 CUDA 模块并链接 CLI/Web UI/测试/显式架构基准共 94.68 秒。原 13.3 的历史 57.80 秒采样保留在七架构报告中，时间点和环境不同，不视为本轮严格构建性能 A/B。

| 字节数 | 13.3 正式包运行文件 | 13.5 新运行文件 |
| --- | ---: | ---: |
| CLI | 3,372,032 | 3,271,680 |
| Web UI | 3,802,112 | 3,701,248 |
| 相同运行文件清单 ZIP，deflate 6 | 7,911,216 | 7,912,629 |

归档比较仅含两程序及部署的 DLL；两组 DLL 哈希和文件清单完全相同，不含文档、许可证或 BUILD_INFO，不是完整发布附件。原始文件略小未转化为压缩包缩小。首次运行、全部样本、峰值内存、文件哈希及输入快照摘要见 [结构化记录](performance/cuda-13.5-migration.json)。

## 远端和正式发布边界

Native packages 在五类产物上执行完整测试、打包及独立解压验收：三个 CPU 平台、Windows CUDA 候选、Linux CUDA 正式附件策略。独立 job 不安装构建依赖或 CUDA Toolkit，验证无驱动 CPU 回退；CUDA 构建 job 另要求 Required 探测失败。迁移交付的工作流结果与本机 Required 包验收一并报告，不用隐藏本机 GPU 代替真实无驱动环境。

Windows CUDA 仍须本机 Required 及真实 GPU 回归，Linux CUDA 保留 not-run。只有本机 Ada 实测通过，其他六个目标是编译覆盖，尚未实机验证。13.5 PTX 仍要求相应驱动支持，七架构和 PTX 不保证任意旧驱动可用。本轮不创建标签或 Release，也不改动既有 v1.14.8。
