# CI 与原生发布包

日常 `CI` 在 Windows x64、Linux x64 和 macOS ARM64 上编译 CPU 版 CLI 与 Web UI。三个平台均直接使用 CMake 平台预设；Linux 和 macOS 使用系统依赖，Windows 使用固定的 vcpkg 依赖。推送 `v*` 版本标签会触发 `Native packages`，执行完整 CTest、Web UI/API smoke、打包和独立包验收。维护者使用通过验收的 Actions artifacts 创建 GitHub Release。

| 平台 | runner | CPU 包 | 云端 CUDA 包 | Vulkan 包 |
| --- | --- | --- | --- | --- |
| Windows x64 | windows-2025-vs2026 | ZIP | ZIP candidate | ZIP |
| Linux x64 | ubuntu-22.04 | tar.gz | tar.gz，正式附件（GPU 未验收） | tar.gz |
| macOS ARM64 | macos-15 | tar.gz | 无 | 无 |

支持基线为 Windows 10 22H2 x64、Ubuntu 22.04/glibc 2.35、macOS 13 ARM64。没有安装器、DMG、Universal 2 或 Developer ID 公证。macOS 包使用 ad-hoc 签名；这不等于 Apple 公证。

## 正式包后端范围

v1.15.11 保留七个原生包，另提供五个 Qt 桌面包。原生 CPU/CUDA 包关闭 Vulkan 和 Metal；原生 Vulkan 包启用 `MOSAICRAFT_VULKAN_AUTO`，`auto` 优先 Vulkan 并回退 CPU，不编译 CUDA/Metal。`release.ps1` 显式设置这些开关并核对实际 CMake 缓存。

```powershell
cmake -DMODE=Vulkan -P cmake/PrepareRelease.cmake
./scripts/release.ps1 -BuildDir build/package-vulkan -NoCuda -VulkanRelease -OutputDir build/archives
```

正式 Vulkan 包不带 `_candidate` 后缀，`BUILD_INFO.txt` 记录 `PortableAutoSelection: vulkan-to-cpu` 和 `PortableGpuValidation: not-run`。独立验收隐藏 Vulkan ICD，验证默认建库和生成自动回退 CPU；实机 GPU 测试另记。原 `-Vulkan`/`-Metal` 参数仍生成仅显式选择的候选包，不得用候选替代正式附件。

## 固定依赖与缓存

`vcpkg.json` 同时固定工具仓库和依赖基线为 `a0400024711b283056538ac19ced80b91a83c24c`。OpenCV 4.12.0 仅启用 jpeg/png/tiff/webp/intrinsics/thread，产品仅链接 core/imgproc/imgcodecs。TIFF 保留 JPEG、LZMA、ZIP 编解码。三个自定义 triplet 只构建 Release 动态库，Windows 使用动态 CRT。

缓存键包含平台、架构、实际编译器/SDK/CMake/vcpkg 指纹、vcpkg 提交和当前依赖配置哈希。产品版本、README、普通源码不影响依赖缓存；CPU/CUDA 共享同一键。PR 只读取缓存，main/tag 在依赖安装成功后立即保存，项目测试失败不会丢掉成功构建的依赖。GitHub 仍可能淘汰超过七天未访问的缓存。

首次本机使用：

```powershell
cmake -DMODE=Prepare -DVCPKG_ROOT=<pinned-vcpkg> -P cmake/PrepareRelease.cmake
cmake -DMODE=Install -DVCPKG_ROOT=<pinned-vcpkg> -P cmake/PrepareRelease.cmake
./scripts/release.ps1 -BuildDir build/package-cpu -NoCuda -OutputDir build/archives
```

准备入口自动选择本机平台的 Release triplet。Linux 打包还需要 patchelf。每种运行时使用独立构建目录。CMake 递归收集第三方运行库，Linux 使用相对 RPATH，macOS 修正加载路径后签名；Unix tar.gz 保留执行权限和符号链接。包内附有第三方许可证。

Linux/macOS 的日常源码构建使用 CMake 3.21+。发布脚本使用 PowerShell 7，并向 CMake 显式传入固定依赖工具链和运行库打包选项。Windows Preset 的 Visual Studio 2026 生成器需要 CMake 4.2+。 [源码构建](building.md)

## CMake 发布入口（迁移验证）

此入口先验证干净提交和实际构建配置，再运行必选 CTest、安装到临时目录、由 CPack 归档，最后用独立工具验证解包后的程序。只有通过验收的归档和 `.sha256` 才进入输出目录。没有跳过测试、冒烟或验包的选项。

先按[源码构建说明](building.md)运行 `Prepare` 和 `Install`。Linux 打包另需系统的 Ninja 和 patchelf。未准备依赖时，发布配置不会自动开始下载和编译 OpenCV。

```sh
cmake -DVCPKG_ROOT=build/tools/vcpkg -DRUNTIME=cpu-only -DOUTPUT_DIR=build/migration-cpu -P cmake/Release.cmake
cmake -DVCPKG_ROOT=build/tools/vcpkg -DRUNTIME=vulkan -DMOSAICRAFT_VULKAN_INCLUDE=<headers-include> -DMOSAICRAFT_GLSLANG=<glslang> -P cmake/Release.cmake
cmake -DVCPKG_ROOT=build/tools/vcpkg -DRUNTIME=metal -DCHANNEL=candidate -P cmake/Release.cmake
```

`RUNTIME` 必填，`CHANNEL` 默认为 `stable`。可传入 `BUILD_DIR`、`OUTPUT_DIR`、`VCPKG_INSTALLED_DIR`；可以提供现有 SDK，也可用 `PrepareRelease.cmake` 的 `CUDA`、`Vulkan` 模式准备。CUDA 发布用 `-DCUDAToolkit_ROOT=<sdk>` 指定 SDK；Windows 正式包默认且必须 `Required`。Linux 未实机且要输出正式命名时须同时传入 `-DCUDA_VALIDATION=BuildOnly -DALLOW_UNVALIDATED_LINUX_CUDA=ON`，标记为 `not-run`。Windows 组合包须显式选 `-DRUNTIME=cuda-vulkan -DCHANNEL=candidate`，如无设备验证再加 `-DCUDA_VALIDATION=BuildOnly`。

打开 **Compare CMake and legacy release packages**，按 `cpu` → `portable` → `cuda` 依次运行。对照以同一提交的两个归档为输入，新的独立 job 不安装 OpenCV 或 GPU SDK，对照完毕再用旧验收器核验新包。`cold` 模式跳过二进制缓存；Linux 会审计依赖准备、发布及独立验收的实际子进程。可传入 `published-tag` 验证现有正式附件。此工作流仅上传对照用的 Actions artifacts，不发布或替换附件。未完成跨平台对照前保持现有 `Native packages`、`Portable GPU candidates` 和 `Verify published release` 不变；Windows 正式 CUDA 还必须单独通过实机 `Required`。

## CUDA 验收状态

云端与本机编译器固定 CUDA Toolkit 13.5.0/nvcc 13.5.37，使用 NVIDIA 官方重分发组件和锁定 SHA256；云端不安装显卡驱动。默认包含 sm_75、sm_80、sm_86、sm_89、sm_90、sm_100、sm_120 原生内核及对应七种 PTX，原算术编译参数保持不变。`cmake/cuda-13.5.json` 同时锁定 cudart、cuobjdump 13.5.37；打包前逐模块和最终程序核对架构，缺少任何目标即失败。

```powershell
# Windows 云端候选：完整 CPU 回退测试，但不声称 GPU 通过验收
cmake -DMODE=CUDA -P cmake/PrepareRelease.cmake
./scripts/release.ps1 -BuildDir build/package-cuda -CudaValidation BuildOnly -OutputDir build/archives

# Linux CUDA 正式附件：完整测试，GPU 状态仍为 not-run
./scripts/release.ps1 -BuildDir build/package-linux-cuda -CudaValidation BuildOnly -AllowUnvalidatedCudaRelease -OutputDir build/archives

# 本机 Windows 正式 CUDA 包：默认 Required，必须有可用 GPU
$env:CUDAToolkit_ROOT = 'C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v13.5'
./scripts/release.ps1 -BuildDir build/package-cuda-local -CudaValidation Required -OutputDir build/archives
```

`BUILD_INFO.txt` 的 `GpuValidation` 为 `passed`、`not-run` 或 `not-applicable`。`CudaArchitectures`、`CudaPtx` 记录实际编译目标；`GpuValidationDevice`、`GpuValidationComputeCapability`、`GpuValidationDriver` 记录 Required 模式实际通过预检的设备，未经 GPU 实机验收的包均为 `not-run`。`passed` 仅表示所记录设备通过验收；其他架构不因此获得实机验证结论。旧包缺少新增字段时仍可普通验收。BuildOnly 自动添加 `_candidate` 文件名后缀；Required 在无 GPU 时失败，且禁止跳过 CTest、smoke 或解压验证。正式 Windows CUDA 包必须来自同一提交并通过本机 GPU 完整回归；Linux CUDA 从 v1.14.8 起进入正式 Release，保持 `not-run`，并在页面明确说明尚未做 Linux 实机 GPU 验收。显式 `-AllowUnvalidatedCudaRelease` 仅允许原生 Linux x64 tar.gz，不允许跳过 CTest、smoke 或解压验证，也不放宽 Windows CUDA 的 Required 门禁。

新包另记录 `CudaToolkit`（编译器组件版本 13.5.37）、`CudaRuntimeMode`（Windows `hybrid` / Linux `shared`）和 `GpuValidationRuntime`（实机运行时 API 版本，未验收为 `not-run`）。Driver 字段同样是 CUDA API 版本整数，均不是 nvidia-smi 的驱动包版本。Windows hybrid 从显示驱动加载运行时，包内不分发显示驱动或盲目复制 SDK DLL；Linux 仍收集实际共享运行库并设置相对 RPATH。历史包缺少这组字段仍可普通验收。迁移验证与成本见 [CUDA 13.5 迁移报告](../archive/gpu/cuda-13-5-migration.md)。

## Qt 桌面发布包

`Native packages` 除了七个 Web UI / CLI 包，还构建下列 Qt 桌面包。桌面包自带 CLI 和 worker，不依赖原生包。

| 平台 | CPU 附件后缀 | CUDA + Vulkan 附件后缀 | 解压后启动 |
| --- | --- | --- | --- |
| Windows x64 | `_windows-x64_qt.zip` | `_windows-x64_qt_cuda-vulkan.zip` | `bin/MosaicraftDesktop.exe` |
| Linux x64 | `_linux-x64_qt.tar.gz` | `_linux-x64_qt_cuda-vulkan.tar.gz` | `./bin/MosaicraftDesktop` |
| macOS ARM64 | `_macos-arm64_qt.tar.gz` | — | `MosaicraftDesktop.app` |

桌面包均用 [锁定的 Qt SDK](../../cmake/qt-sdk.json)，随包部署 Qt 共享库、平台插件、SQLite、WebP、TIFF、CLI、桌面匹配程序及后端依赖。保持解压后的目录结构，无需另装 Qt 或 CUDA Toolkit。Linux 以 Ubuntu 24.04 上的构建为基线，仍需要操作系统的显示服务和驱动；macOS `.app` 为临时签名（ad-hoc），未公证。

双后端包的 CLI 和 worker 均编入 CUDA 和 Vulkan；`auto` 使用 CUDA → CPU，需通过桌面设置显式选择 Vulkan。不分发显卡驱动，使用 GPU 时需安装相应驱动。

所有桌面包都核对归档布局、Qt 库、插件及许可证，并生成 `.sha256`。双后端包另检查 CUDA 的七种原生架构及 PTX，解包后运行 GUI 和无 GPU 时的 CPU 回退测试。Windows 云端产物以 `_candidate` 结尾，正式包需实机通过 CUDA 和 Vulkan 预览；Linux 云端包的 GPU 实机状态记录为 `not-run`。可复查下载的桌面附件：

```powershell
./scripts/verify-desktop-asset.ps1 -ArchivePath <archive> -ExpectedSha256 <sha256>
```

本机构建先按[源码构建](building.md)准备对应平台的锁定 vcpkg 依赖，设置 `VCPKG_ROOT`，Linux 另装工作流中列出的链接/显示依赖，再运行：

```powershell
cmake -DMODE=Qt -P cmake/PrepareRelease.cmake
$qtRoot = 'build/tools/qt-sdk/6.12.0'
./scripts/release-desktop.ps1 -QtRoot $qtRoot
```

Windows/Linux 双后端构建另需准备锁定的 CUDA SDK、Vulkan 头文件和离线着色器编译器（见 `Native packages`），然后给脚本加 `-CudaVulkan`；默认要求实机验证。没有 GPU 验收环境时显式加 `-GpuValidation BuildOnly`；Linux 冒烟测试还需 `xvfb` 与 `xauth`。

产物位于 `build/archives-qt/`。对外分发时保留包内许可证和源码来源说明，并随发布提供与锁定 Qt 版本对应的源码获取方式。

## 独立包验收

三个 CPU、两个 CUDA 和两个 Vulkan 包均在独立的新 job 下载附件，不安装项目依赖或 CUDA Toolkit。验证 CLI、API、图片生成、中文与空格路径、依赖加载位置，以及 Unix 执行权限和 macOS 签名。CUDA 候选在无 GPU 环境验证回退运行，仍标记 `not-run`。

```powershell
./scripts/verify-release-asset.ps1 -ArchivePath <archive> -RequireStandalone
./scripts/verify-release-asset.ps1 -Tag v<version> -AssetName <exact-archive-name>
```

`-ZipPath` 保留为 `-ArchivePath` 别名；旧包缺少 GPU 状态时仍可普通验收。多个附件必须指定 `-AssetName`。`release.ps1 -ArchiveFormat auto|zip|tar.gz` 默认 Windows ZIP、Unix tar.gz；包旁生成 `.sha256`，包内记录版本、提交、平台、架构和运行时。

正式发布后，手动运行 `Verify published release` 并传入版本标签，可在三个原生平台下载、验收真实 Release 上的七个正式附件（旧版本跳过尚未提供的附件）。该流程只读取 Release，不上传或修改附件。

历史测量与旧环境说明已移至[CI 与打包历史采样](../archive/performance/packaging-20261108.md)。

## 文档归档布局

新包复制完整 docs/ 树，文档中心、当前指南与历史报告保留同一相对位置，不再维护易遗漏的文件名清单。根目录只保留 README 等启动和许可文件；API 和历史百科从 docs/ 导航。版本摘要检查使用 docs/releases.md。独立验收脚本同时识别旧包布局，不能要求历史包凭空增加新目录。

Vulkan/Metal 候选编译与验收状态见[四后端实现](portable-gpu.md)。
