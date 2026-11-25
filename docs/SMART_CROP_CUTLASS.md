# Windows CUDA 智能裁剪轻量化实验

本文记录开发阶段的实验：仅保留三个锁定模型所需的 CUDA 算子，以同源码 cuDNN 路径为对照。下文的候选 ZIP 与数据是开发记录；v1.15.18 的正式附件和校验值见[版本说明](releases.md#v11518)。

## 探针

在 x64 Visual Studio Developer PowerShell 中运行：

```powershell
cmake "-DCUDA_ROOT=C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v13.5" `
  "-DWORK=$PWD/build/smart-crop-cutlass" -P scripts/probe_smart_crop_cutlass.cmake
```

只编译 CUTLASS 的头文件模板。探针在 MSVC 14.51.36231、nvcc 13.5.37、驱动 621.14、RTX 4060 Laptop（sm_89）上通过 SIMT FP32 GEMM、3×3 卷积和 3×3 空洞卷积精确结果检查。`/Zc:preprocessor` 是本工具链的 CCCL 要求；使用了原工具链，未修改 CUTLASS 源码。

## 对比方式

用两个独立安装树中的 CLI，避免加载对侧的 OpenCV DLL。对 200 张动漫和 32 张独立真人图分别运行：

```powershell
python scripts/bench_smart_crop_cuda.py <基线CLI> <输入目录> <新报告目录> `
  --study cutlass --candidate-exe <候选CLI> --mode smart-anime --rounds 3
```

真人用 `--mode smart-real`。交错运行，报告逐文件 SHA-256、智能裁剪坐标、CPU 回退、墙钟时间、工作集及显存采样。脚本也检查 OpenCV 日志中的内部 CPU 回退，不把宿主侧的形状计算算作推理回退。这个对比不替代仅驱动环境中的模块加载验收。

## 矩阵运算跟踪

三个锁定 ONNX 没有显式 Gemm 或 MatMul。用同源码基线在 RTX 4060 Laptop 上分别运行真人和动漫小样本，并用 Nsight Systems 记录 CUDA、cuBLAS 和 cuDNN API。动漫有 2 次 `cublasLtMatmul`（对应 GEMV 内核），两次都在同线程的 `cudnnConvolutionForward` 内；真人没有 Matmul 执行，但两个模式都从 cuDNN 调用了 cuBLASLt 启发式 API。OpenCV 自身仍各初始化了两个 cuBLAS 句柄。

因此不编译没有实际调用的 CUDA GEMM、MatMul 或 Attention 节点。中间版仅去掉 DNN 的直接 cuBLAS 调用和链接，保留 cuDNN 作为对照，不能作为无 cuBLAS 依赖的包分发。

中间版的 `opencv_dnn5.dll` 导入表只剩 cuDNN；8 张真人和 24 张动漫输出与基线逐字节相同。动漫运行仍间接加载 `cublasLt64_13.dll` 和 cuDNN 系列 DLL，因此未对中间版打包。

## 构建隔离

在 Windows x64 使用仓库固定的 vcpkg 和 `x64-windows-release` triplet。为基线安装 `smart-crop-cuda`，为候选安装 `smart-crop-cutlass`，分别指定安装目录。两者都用 `MOSAICRAFT_CUDA_SDK` 指向 CUDA 13.5.37；候选不需要 `MOSAICRAFT_CUDNN_SDK`。先设 `MOSAICRAFT_OPENCV_CUDA_ARCH=8.9` 做单卡实验，Mosaicraft 侧对应 `-DCUDA_ARCH=sm_89`。只在单卡达标后移除单架构设置，OpenCV 使用七种默认架构和末端 PTX，应用沿用各架构的 PTX 策略。

在仓库根目录安装候选依赖（基线用另一目录和特性）：

```powershell
& "$vcpkg/vcpkg.exe" install --x-feature=smart-crop-cutlass --triplet=x64-windows-release `
  --host-triplet=x64-windows-release "--overlay-triplets=$PWD/cmake/triplets" `
  "--x-install-root=$candidateDeps"
```

应用使用各自的 `-DVCPKG_INSTALLED_DIR`，两边都设置 `MOSAICRAFT_SMART_CROP=ON`、`MOSAICRAFT_SMART_CROP_CUDA=ON`、`MOSAICRAFT_BUNDLE_RUNTIME=ON` 和已校验的 `MOSAICRAFT_SMART_CROP_MODELS_DIR`。仅候选额外设置 `MOSAICRAFT_SMART_CROP_CUTLASS=ON`；基线继续使用现有 CUDA redist。分别构建 `mosaicraft`、`MosaicraftWebUI`，再用 `cmake --install` 安装到两个新的本地目录，不经过正式发布流程。

候选使用 FP32 SIMT implicit GEMM；无填充、步长为 1 的 1×1 卷积直接对 NCHW 张量做 GEMM，无需转置。普通 3×3 和可适配的空洞卷积使用 CUTLASS，不可适配的形状以及 YuNet 深度卷积使用直接 CUDA 核。模型、输入尺寸、图执行器和已有后处理不变；尺寸改变时复用已有设备权重缓存。

## 显存诊断与单卡门槛

首版对整张特征图做 NCHW ↔ NHWC 转换，输出和耗时达标，但采样显存分别增加 1278–1460 MiB（动漫）和 866–894 MiB（真人）。使用 Nsight Systems 记录同一输入上的 CUDA 分配和释放，按设备地址累计仍存活的分配：动漫从 4729.78 MiB 升至 6196.16 MiB，候选有一笔 1547.11 MiB 的布局工作区；真人从 1864.26 MiB 升至 3424.24 MiB。它们是分配事件峰值，不等同于 `nvidia-smi` 的整卡采样。

优化后每个图的卷积布局工作区上限为 64 MiB。对较大的特征图按输出行分条，只转换本条需要的输入行及卷积重叠区；在同一 CUDA 流上复用缓存并写回原 NCHW 输出。分条保持原始步长、空洞率和边界填充，不拆分单个输出的累加。复测分配事件峰值为 4701.88 MiB（动漫）和 1928.93 MiB（真人）；动漫的设备分配事件也从 27,951 次降到了 647 次。

在 RTX 4060 Laptop 上，`sm_89` 限额版通过本机单卡门槛：

| 样本（三轮交错） | 基线 / CUTLASS 中位墙钟 | 候选峰值工作集变化 | 候选采样显存变化（各轮） |
|---|---:|---:|---:|
| 200 张动漫 | 23.558 s / 21.050 s（少 10.6%） | −491 至 −490 MiB | −32 / −32 / −32 MiB |
| 32 张独立真人照片 | 5.793 s / 5.740 s（少 0.9%） | −448 至 −446 MiB | +210 / +68 / −110 MiB |

两组输入哈希均不变，输出文件名、ROI、SHA-256 全部相同；没有新增失败、OOM 或 CPU 回退。输入是既有 HighLikes 200 张样本和有来源记录的 32 张独立 Wikimedia Commons 真人照片。显存按每轮采样峰值扣除运行前空闲值，再按同轮基线和候选比较。

## 本地包检查

```powershell
python scripts/audit_smart_crop_cutlass.py <基线安装目录> <候选安装目录> <新报告目录>
pwsh -File scripts/audit-smart-crop-cutlass-modules.ps1 -Exe <候选CLI> `
  -InputDirectory <测试图目录> -OutputDirectory <新报告目录> -Crop smart-anime
```

第一个命令从 x64 Visual Studio Developer PowerShell 调用，扫描普通及延迟导入，并用相同 Deflate 设置测量两个 ZIP。第二个命令使用受限 PATH，记录成功执行严格 CUDA 的进程模块；仅在实际未安装 CUDA 工具包和旧运行库的 Windows 环境运行该命令，才计作纯驱动验收。

## 最终七架构构建实测（2026-11-25）

候选从全新的 vcpkg 构建和安装目录构建 OpenCV 和应用，未使用中间版二进制。用 `cuobjdump` 确认两边的 `opencv_dnn5.dll` 均含 `sm_75 / 80 / 86 / 89 / 90 / 100 / 120` 原生代码，OpenCV 的末端 PTX 为 `compute_120`。**只在 RTX 4060 Laptop（`sm_89`）实测**；其他六种架构仅完成构建和二进制检查。

| 样本（三轮交错） | 基线 / CUTLASS 中位墙钟 | 候选峰值工作集变化 | 候选采样显存变化（各轮） |
|---|---:|---:|---:|
| 200 张动漫 | 21.544 s / 20.584 s（少 4.5%） | −490 至 −489 MiB | −31 / −19 / −30 MiB |
| 32 张独立真人照片 | 6.112 s / 5.980 s（少 2.2%） | −448 至 −445 MiB | +35 / +78 / +30 MiB |

最终候选两组都满足墙钟、工作集、显存门槛；各轮输入哈希不变，文件名、ROI 和输出 SHA-256 一致。所有 232 张输入在每轮都完成 CUDA 推理，无新增失败、OOM 或 CPU 回退。未运行全量。

回归通过：同尺寸 PNG 保留原字节，坏图跳过，连续切换尺寸和两组样本内的最大图均保持输出一致；新建图库在真人、动漫模式下各验证一组真实 GPU 输入。原有严格 CUDA 故障排空和 `auto` 恢复用故障注入回归，`mosaicraft_tests` 和 `mosaicraft_regression_tests` 共 2/2 通过。

| 同参数 Deflate | 基线 | CUTLASS |
|---|---:|---:|
| ZIP | 761.54 MiB | **48.83 MiB** |
| 解包 | 1211.62 MiB | 193.82 MiB |
| `opencv_dnn5.dll`（解包 / ZIP 内） | 102.77 / 14.65 MiB | 103.44 / 14.75 MiB |

候选 ZIP 减少 **93.6%**。两包逐文件尺寸见审计报告；候选含 CUTLASS 许可及构建信息，所有 EXE/DLL 的普通及延迟导入均未命中 cuBLAS、cuBLASLt 或 cuDNN。候选 ZIP 的 CRC 检查通过，打包的 CLI 和 DNN DLL 哈希与跑分二进制一致。

受限 PATH 的本机进程采样中，动漫和真人均加载系统 NVIDIA 驱动，未发现目标 DLL 或 Toolkit 模块。**纯驱动 Windows 环境未实测**：测试机仍安装了 Toolkit，不能将受限 PATH 当作纯驱动验收。本节记录的是当时的本地候选，不是 v1.15.18 正式下载文件。

本地证据位于 `build/smart-crop-cutlass/multi`：`ab-anime/report.json`、`ab-real/report.json`、`architectures.txt`、`ctest.log`、`regression/report.json`、`modules-{anime,real}/modules.txt` 及 `zip-audit-final/report.json`。
