# CI 与原生发布包

日常 `CI` 在 Windows x64、Linux x64 和 macOS ARM64 上编译 CPU 版 CLI 与 Web UI。Linux 和 macOS 通过 `scripts/build-native.sh` 使用系统依赖，Windows 使用固定的 vcpkg 依赖。推送 `v*` 版本标签会触发 `Native packages`，执行完整 CTest、Web UI/API smoke、打包和独立包验收。维护者使用通过验收的 Actions artifacts 创建 GitHub Release。

| 平台 | runner | CPU 包 | 云端 CUDA 包 |
| --- | --- | --- | --- |
| Windows x64 | windows-2025-vs2026 | ZIP | ZIP candidate |
| Linux x64 | ubuntu-22.04 | tar.gz | tar.gz，正式附件（GPU 未验收） |
| macOS ARM64 | macos-15 | tar.gz | 无 |

支持基线为 Windows 10 22H2 x64、Ubuntu 22.04/glibc 2.35、macOS 13 ARM64。没有安装器、DMG、Universal 2 或 Developer ID 公证。macOS 包使用 ad-hoc 签名；这不等于 Apple 公证。

## 正式包后端范围

v1.15.7 继续交付 Windows CPU/CUDA、Linux CPU/CUDA、macOS ARM64 CPU 五个包，不添加 Vulkan 或 Metal 附件。标准 presets 默认关闭 MOSAICRAFT_VULKAN 和 MOSAICRAFT_METAL，release.ps1 未收到显式可选后端开关时强制传入 OFF，再检查实际 CMake 缓存。BUILD_INFO.txt 记录 VulkanBackend / MetalBackend 为 disabled。

需要 Vulkan 的用户须[从源码编译](building.md#vulkan-与-metal)。Portable GPU candidates 工作流仅用于可选后端构建验证，不将候选二进制纳入此正式 Release。

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

Linux/macOS 的日常源码构建使用 CMake 3.21+。发布脚本使用 PowerShell 7，并向 CMake 显式传入固定依赖工具链和运行库打包选项。Windows Preset 的 Visual Studio 2026 生成器需要 CMake 4.2+。 [源码构建](building.md)

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

新包另记录 `CudaToolkit`（编译器组件版本 13.5.37）、`CudaRuntimeMode`（Windows `hybrid` / Linux `shared`）和 `GpuValidationRuntime`（实机运行时 API 版本，未验收为 `not-run`）。Driver 字段同样是 CUDA API 版本整数，均不是 nvidia-smi 的驱动包版本。Windows hybrid 从显示驱动加载运行时，包内不分发显示驱动或盲目复制 SDK DLL；Linux 仍收集实际共享运行库并设置相对 RPATH。历史包缺少这组字段仍可普通验收。迁移验证与成本见 [CUDA 13.5 迁移报告](../archive/gpu/cuda-13-5-migration.md)。

## 独立包验收

CPU 三个平台和 CUDA 两个平台均在独立的新 job 下载附件，不安装项目依赖或 CUDA Toolkit。验证 CLI、API、图片生成、中文与空格路径、依赖加载位置，以及 Unix 执行权限和 macOS 签名。CUDA 候选在无 GPU 环境验证回退运行，仍标记 `not-run`。

```powershell
./scripts/verify-release-asset.ps1 -ArchivePath <archive> -RequireStandalone
./scripts/verify-release-asset.ps1 -Tag v<version> -AssetName <exact-archive-name>
```

`-ZipPath` 保留为 `-ArchivePath` 别名；旧包缺少 GPU 状态时仍可普通验收。多个附件必须指定 `-AssetName`。`release.ps1 -ArchiveFormat auto|zip|tar.gz` 默认 Windows ZIP、Unix tar.gz；包旁生成 `.sha256`，包内记录版本、提交、平台、架构和运行时。

正式发布后，手动运行 `Verify published release` 并传入版本标签，可在三个原生平台下载、验收真实 Release 上的五个正式附件（v1.14.7 的 Linux CUDA 项跳过）。该流程只读取 Release，不上传或修改附件。

历史测量与旧环境说明已移至[CI 与打包历史采样](../archive/performance/packaging-20261108.md)。

## 文档归档布局

新包复制完整 docs/ 树，文档中心、当前指南与历史报告保留同一相对位置，不再维护易遗漏的文件名清单。根目录只保留 README 等启动和许可文件；API 和历史百科从 docs/ 导航。版本摘要检查使用 docs/releases.md。独立验收脚本同时识别旧包布局，不能要求历史包凭空增加新目录。

Vulkan/Metal 候选编译与验收状态见[四后端实现](portable-gpu.md)。
