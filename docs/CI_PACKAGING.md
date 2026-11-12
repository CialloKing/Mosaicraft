# CI 与原生发布包

日常 `CI` 在 Windows x64、Linux x64 和 macOS ARM64 上编译 CPU 版 CLI 与 Web UI。Linux 和 macOS 通过 `scripts/build-native.sh` 使用系统依赖，Windows 使用固定的 vcpkg 依赖。推送 `v*` 版本标签会触发 `Native packages`，执行完整 CTest、Web UI/API smoke、打包和独立包验收。维护者使用通过验收的 Actions artifacts 创建 GitHub Release。

| 平台 | runner | CPU 包 | 云端 CUDA 包 |
| --- | --- | --- | --- |
| Windows x64 | windows-2025-vs2026 | ZIP | ZIP candidate |
| Linux x64 | ubuntu-22.04 | tar.gz | tar.gz，正式附件（GPU 未验收） |
| macOS ARM64 | macos-15 | tar.gz | 无 |

支持基线为 Windows 10 22H2 x64、Ubuntu 22.04/glibc 2.35、macOS 13 ARM64。没有安装器、DMG、Universal 2 或 Developer ID 公证。macOS 包使用 ad-hoc 签名；这不等于 Apple 公证。

## 固定依赖与缓存

`vcpkg.json` 同时固定工具仓库和依赖基线为 `a0400024711b283056538ac19ced80b91a83c24c`。OpenCV 4.12.0 仅启用 jpeg/png/tiff/webp/intrinsics/thread，产品仅链接 core/imgproc/imgcodecs。TIFF 保留 JPEG、LZMA、ZIP 编解码。三个自定义 triplet 只构建 Release 动态库，Windows 使用动态 CRT。

缓存键包含平台、架构、实际编译器/SDK/CMake/vcpkg 指纹、vcpkg 提交和当前依赖配置哈希。产品版本、README、普通源码不影响依赖缓存；CPU/CUDA 共享同一键。PR 只读取缓存，main/tag 在依赖安装成功后立即保存，项目测试失败不会丢掉成功构建的依赖。GitHub 仍可能淘汰超过七天未访问的缓存。

首次本机使用：

```powershell
./scripts/setup-dependencies.ps1 -Mode Prepare -VcpkgRoot <pinned-vcpkg> -Triplet x64-windows-release
./scripts/setup-dependencies.ps1 -Mode Install -VcpkgRoot <pinned-vcpkg> -Triplet x64-windows-release
./scripts/release.ps1 -BuildDir build/package-cpu -NoCuda -OutputDir build/archives
```

Linux/macOS 分别选择 x64-linux-release、arm64-osx-release。Linux 打包还需要 patchelf。每种运行时使用独立构建目录。CMake 递归收集第三方运行库，Linux 使用相对 RPATH，macOS 修正加载路径后签名；Unix tar.gz 保留执行权限和符号链接。包内附有第三方许可证。

Linux/macOS 的日常源码构建使用 CMake 3.21+。发布脚本使用 PowerShell 7，并向 CMake 显式传入固定依赖工具链和运行库打包选项。Windows Preset 的 Visual Studio 2026 生成器需要 CMake 4.2+。 [README 构建入口](../README.md#安装与构建)

## CUDA 验收状态

云端与本机编译器固定 CUDA Toolkit 13.5.0/nvcc 13.5.37，使用 NVIDIA 官方重分发组件和锁定 SHA256；云端不安装显卡驱动。默认包含 sm_75、sm_80、sm_86、sm_89、sm_90、sm_100、sm_120 原生内核及对应七种 PTX，原算术编译参数保持不变。`cmake/cuda-13.5.json` 同时锁定 cudart、cuobjdump 13.5.37；打包前逐模块和最终程序核对架构，缺少任何目标即失败。

```powershell
# Windows 云端候选：完整 CPU 回退测试，但不声称 GPU 通过验收
./scripts/setup-cuda.ps1
./scripts/release.ps1 -BuildDir build/package-cuda -CudaValidation BuildOnly -OutputDir build/archives

# Linux CUDA 正式附件：完整测试，GPU 状态仍为 not-run
./scripts/release.ps1 -BuildDir build/package-linux-cuda -CudaValidation BuildOnly -AllowUnvalidatedCudaRelease -OutputDir build/archives

# 本机 Windows 正式 CUDA 包：默认 Required，必须有可用 GPU
$env:CUDAToolkit_ROOT = 'C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v13.5'
./scripts/release.ps1 -BuildDir build/package-cuda-local -CudaValidation Required -OutputDir build/archives
```

`BUILD_INFO.txt` 的 `GpuValidation` 为 `passed`、`not-run` 或 `not-applicable`。`CudaArchitectures`、`CudaPtx` 记录实际编译目标；`GpuValidationDevice`、`GpuValidationComputeCapability`、`GpuValidationDriver` 记录 Required 模式实际通过预检的设备，未经 GPU 实机验收的包均为 `not-run`。`passed` 仅表示所记录设备通过验收；其他架构不因此获得实机验证结论。旧包缺少新增字段时仍可普通验收。BuildOnly 自动添加 `_candidate` 文件名后缀；Required 在无 GPU 时失败，且禁止跳过 CTest、smoke 或解压验证。正式 Windows CUDA 包必须来自同一提交并通过本机 GPU 完整回归；Linux CUDA 从 v1.14.8 起进入正式 Release，保持 `not-run`，并在页面明确说明尚未做 Linux 实机 GPU 验收。显式 `-AllowUnvalidatedCudaRelease` 仅允许原生 Linux x64 tar.gz，不允许跳过 CTest、smoke 或解压验证，也不放宽 Windows CUDA 的 Required 门禁。

新包另记录 `CudaToolkit`（编译器组件版本 13.5.37）、`CudaRuntimeMode`（Windows `hybrid` / Linux `shared`）和 `GpuValidationRuntime`（实机运行时 API 版本，未验收为 `not-run`）。Driver 字段同样是 CUDA API 版本整数，均不是 nvidia-smi 的驱动包版本。Windows hybrid 从显示驱动加载运行时，包内不分发显示驱动或盲目复制 SDK DLL；Linux 仍收集实际共享运行库并设置相对 RPATH。历史包缺少这组字段仍可普通验收。迁移验证与成本见 [CUDA 13.5 迁移报告](CUDA_13_5_MIGRATION.md)。

## 独立包验收

CPU 三个平台和 CUDA 两个平台均在独立的新 job 下载附件，不安装项目依赖或 CUDA Toolkit。验证 CLI、API、图片生成、中文与空格路径、依赖加载位置，以及 Unix 执行权限和 macOS 签名。CUDA 候选在无 GPU 环境验证回退运行，仍标记 `not-run`。

```powershell
./scripts/verify-release-asset.ps1 -ArchivePath <archive> -RequireStandalone
./scripts/verify-release-asset.ps1 -Tag v<version> -AssetName <exact-archive-name>
```

`-ZipPath` 保留为 `-ArchivePath` 别名；旧包缺少 GPU 状态时仍可普通验收。多个附件必须指定 `-AssetName`。`release.ps1 -ArchiveFormat auto|zip|tar.gz` 默认 Windows ZIP、Unix tar.gz；包旁生成 `.sha256`，包内记录版本、提交、平台、架构和运行时。

正式发布后，手动运行 `Verify published release` 并传入版本标签，可在三个原生平台下载、验收真实 Release 上的五个正式附件（v1.14.7 的 Linux CUDA 项跳过）。该流程只读取 Release，不上传或修改附件。

## 性能采样

依赖日志及 `install-summary.json` 记录安装时间和二进制恢复数量，打包目录的 `release-timings.json` 记录配置、编译、测试、smoke、打包和解压验证时间。GitHub step summary 展示相同阶段数据。

使用 `CI` 的 `cache-namespace` 输入，针对同一提交选择一个全新命名空间后连续手动运行两次，可得到 Windows 冷、热缓存对照。冷日志应没有 Protobuf、DNN 等依赖及 Debug 构建，热日志应恢复依赖。下方 Linux 数据来自日常 CI 仍使用 vcpkg 的时期。

2026-11-08 对提交 `874503e` 使用独立缓存命名空间，各测一次冷、热缓存；[两次运行](https://github.com/CialloKing/Mosaicraft/actions/runs/42479756921)均通过。依赖列包含固定工具检出、引导、缓存及安装，不包含产品编译。

| 平台 | 冷缓存依赖步骤 | 热缓存依赖步骤 | 冷缓存 job | 热缓存 job | 热恢复数量 |
| --- | --- | --- | --- | --- | --- |
| Windows x64 | 596 秒 | 39 秒 | 12 分 11 秒 | 2 分 52 秒 | 11/11 |
| Linux x64 | 324 秒 | 17 秒 | 6 分 28 秒 | 1 分 15 秒 | 11/11 |

两个热缓存 job 均达到目标，未重新编译 OpenCV；冷日志确认没有 Protobuf、Abseil、FlatBuffers 或 Debug 依赖构建。热缓存中 vcpkg 安装本身分别为 8.42 秒和 3.27 秒。产品编译、CTest、smoke 的分项时间和实际工具链见[原始数据](performance/ci-packaging.json)。这是单次对照，不是长期耗时保证。

独立的热缓存 CPU 打包样本（提交 `babaae3`）中，Windows ZIP 打包 1.26 秒、解压验收 7.28 秒；Linux tar.gz 打包 3.50 秒、解压验收 7.58 秒。它与上述日常 CI 样本分开记录。

本机 CUDA Required 验收通过 88 个用例、110540 个断言和包内 Web UI/API smoke；隐藏 GPU 时 Required 明确失败。与已发布 v1.14.6 对照的私有固定输入涵盖 180×320、72×128 两种尺寸及 CPU/CUDA：按像素哈希对齐的特征、主图与预览像素、选图使用计数和评分摘要均一致。生成对照使用同一图库快照，Top-N 固定为 1，关闭亮度调整，叠加强度 0.25。

本轮保持 CLI、特征 v6、API 合约 2.0 和图库格式不变；不发布或修改 v1.14.6。

多架构配置、CPU 回退和实测记录见 [CUDA 多架构支持](CUDA_ARCHITECTURES.md)。
