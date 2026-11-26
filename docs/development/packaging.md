# CI 与原生发布包

日常 `CI` 使用 CMake 平台预设和固定的 vcpkg 依赖，在 Windows x64、Linux x64 和 macOS ARM64 构建 CPU 版 CLI 与 Web UI，并在 Windows 检查 Qt 客户端。推送 `v*` 版本标签触发 `Native packages`，执行构建、测试、归档与验包。维护者汇集通过验收的六个正式包后发布。

| 平台 | 智能裁剪原生包 | Qt 客户端包 |
| --- | --- | --- |
| Windows x64 | CUDA + Vulkan ZIP | CUDA + Vulkan Qt ZIP |
| Linux x64 | CUDA + Vulkan tar.gz | CUDA + Vulkan Qt tar.gz |
| macOS ARM64 | CPU tar.gz | CPU Qt tar.gz |

智能裁剪包提供 CLI 和 Web UI。Windows/Linux 的包以 CUDA 优先的自动策略生成马赛克，Vulkan 可显式选择。智能裁剪推理可选 CPU、CUDA 或 Vulkan，Vulkan 用于独立归一化。macOS 智能裁剪使用 CPU。公开附件与候选的区别、六件套校验和标注正例设置见[发布清单](releasing.md#六个发布附件)。

原生支持基线为 Windows 10 22H2 x64、Ubuntu 22.04/glibc 2.35、macOS 13 ARM64。macOS 包使用 ad-hoc 签名。

## 固定依赖与缓存

`vcpkg.json` 同时固定工具仓库和依赖基线为 `a0400024711b283056538ac19ced80b91a83c24c`。`cmake/ports/opencv5` 直接从 GitHub 官方仓库获取 OpenCV 5.1.0 源码，CUDA DNN 构建另取同版本的 `opencv_contrib` 源码以提供 `cudev`。两个归档均锁定上游提交和 SHA-512。SQLite 与编解码库沿用 vcpkg 基线。默认构建仅请求 `core`、`imgproc`、`imgcodecs`（OpenCV 5.1 同时构建它们的传递依赖），智能裁剪单独加入 `dnn`、`objdetect` 及其必要依赖。TIFF 保留 JPEG、LZMA、ZIP 编解码。三个自定义 triplet 只构建 Release 动态库，Windows 使用动态 CRT。[OpenCV 5.1.0](https://github.com/opencv/opencv/releases/tag/5.1.0) · [opencv_contrib 5.1.0](https://github.com/opencv/opencv_contrib/releases/tag/5.1.0)

缓存键包含平台、架构、实际编译器/SDK/CMake/vcpkg 指纹、vcpkg 提交和当前依赖配置哈希。产品版本、README、普通源码不影响依赖缓存；使用普通 OpenCV 的 CPU/CUDA 构建可共享依赖缓存，CPU 智能裁剪和 CUTLASS 智能裁剪各有独立的依赖与缓存键。PR 只读取缓存，main/tag 在依赖安装成功后立即保存，项目测试失败不会丢掉成功构建的依赖。GitHub 仍可能淘汰超过七天未访问的缓存。

首次本机使用：

```powershell
cmake -DMODE=Prepare "-DVCPKG_ROOT=<pinned-vcpkg>" -P cmake/PrepareRelease.cmake
cmake -DMODE=Install "-DVCPKG_ROOT=<pinned-vcpkg>" -P cmake/PrepareRelease.cmake
cmake -DRUNTIME=cpu-only "-DVCPKG_ROOT=<pinned-vcpkg>" -P cmake/Release.cmake
```

准备入口自动选择本机平台的 Release triplet。Linux 打包还需要 patchelf。每种运行时使用独立构建目录。CMake 递归收集第三方运行库，Linux 使用相对 RPATH，macOS 修正加载路径后签名；Unix tar.gz 保留执行权限和符号链接。包内附有第三方许可证。

Linux/macOS 的日常源码构建使用 CMake 3.21+。发布使用 CMake/CTest/CPack，并显式使用固定依赖工具链。Windows Preset 的 Visual Studio 2026 生成器需要 CMake 4.2+。 [源码构建](building.md)

## CMake 发布入口

`Release.cmake` 校验干净的源提交和实际构建配置，运行必选 CTest，由 CPack 归档，再用独立工具验收解包后的程序。只有通过验收的归档和 `.sha256` 才会进入输出目录。没有跳过测试、冒烟或验包的选项。

现行智能裁剪包先准备模型和依赖。macOS 在 `Prepare`、`Install` 时设置 `SMART_CROP=ON`，再以 `-DRUNTIME=cpu-only -DSMART_CROP=ON` 运行 `Release.cmake`。Windows/Linux 先运行 `PrepareRelease.cmake` 的 `CUDA`、`Vulkan`、`SmartCropVulkan`、`Models` 模式，再在 `Prepare`、`Install` 时设置 `SMART_CROP=ON` 与 `SMART_CROP_CUTLASS=ON`。Vulkan 模型在 Windows 用 `-DMODE=SmartCropVulkan -DPREPARE_VULKAN_MODELS=ON` 生成，Linux 使用同一份六文件模型目录。导出准备步骤打印的 ncnn 配置和许可路径，分别赋给 `MOSAICRAFT_NCNN_DIR`、`MOSAICRAFT_NCNN_LICENSE`。打包使用以下参数：

```sh
cmake -DRUNTIME=cuda-vulkan -DSMART_CROP=ON -DSMART_CROP_CUDA=ON \
  -DSMART_CROP_CUTLASS=ON -DSMART_CROP_VULKAN=ON -DCUDA_VALIDATION=Required \
  -P cmake/Release.cmake
```

打包默认使用生成的测试图验收推理。需要检查标注样本时，可选传入 `-DSMART_CROP_FIXTURE_ROOT=<fixtures>`。

以下入口留作历史包复查和单后端开发打包：

```sh
cmake -DVCPKG_ROOT=build/tools/vcpkg -DRUNTIME=cpu-only -P cmake/Release.cmake
cmake -DVCPKG_ROOT=build/tools/vcpkg -DRUNTIME=vulkan -DMOSAICRAFT_VULKAN_INCLUDE=<headers-include> -DMOSAICRAFT_GLSLANG=<glslang> -P cmake/Release.cmake
cmake -DVCPKG_ROOT=build/tools/vcpkg -DRUNTIME=metal -DCHANNEL=candidate -P cmake/Release.cmake
```

可传入 `BUILD_DIR`、`OUTPUT_DIR`、`VCPKG_INSTALLED_DIR`。默认输出 `build/archives/`，`CHANNEL` 默认为 `stable`。`PrepareRelease.cmake` 的 `CUDA`、`Vulkan` 模式会打印 SDK 路径；本机请显式传入这些路径或设置相应环境变量，GitHub Actions 会自动传递给后续步骤。

`Audit CMake release packages` 可分 CPU、portable、CUDA 运行；Linux 冷运行审计子进程和依赖缓存。Windows 正式 CUDA 附件仍须在实机以 `Required` 模式打包。

## CUDA 验收状态

云端与本机编译器固定 CUDA Toolkit 13.5.0/nvcc 13.5.37，使用 NVIDIA 官方重分发组件和锁定 SHA256；云端不安装显卡驱动。默认包含 sm_75、sm_80、sm_86、sm_89、sm_90、sm_100、sm_120 原生内核及对应七种 PTX，原算术编译参数保持不变。`cmake/cuda-13.5.json` 同时锁定 cudart、cuobjdump 13.5.37；打包前逐模块和最终程序核对架构，缺少任何目标即失败。

```sh
# Windows 云端候选（SDK 路径替换成实际安装目录）
cmake -DRUNTIME=cuda -DCHANNEL=candidate -DCUDA_VALIDATION=BuildOnly -DCUDAToolkit_ROOT=<sdk> -P cmake/Release.cmake

# Linux CUDA 正式附件：完整测试，GPU 状态仍为 not-run
cmake -DRUNTIME=cuda -DCUDA_VALIDATION=BuildOnly -DALLOW_UNVALIDATED_LINUX_CUDA=ON -DCUDAToolkit_ROOT=<sdk> -P cmake/Release.cmake

# Windows 实机正式 CUDA 包
cmake -DRUNTIME=cuda -DCUDA_VALIDATION=Required -DCUDAToolkit_ROOT=<sdk> -P cmake/Release.cmake
```

`BUILD_INFO.txt` 的 `GpuValidation` 为 `passed`、`not-run` 或 `not-applicable`。`CudaArchitectures`、`CudaPtx` 记录实际编译目标；`GpuValidationDevice`、`GpuValidationComputeCapability`、`GpuValidationDriver` 记录 Required 模式实际通过预检的设备，未经 GPU 实机验收的包均为 `not-run`。`passed` 仅表示所记录设备通过验收；其他架构不因此获得实机验证结论。旧包缺少新增字段时仍可普通验收。Windows BuildOnly 仅允许候选包；Required 在无 GPU 时失败，且禁止跳过 CTest、smoke 或解压验证。正式 Windows CUDA 包必须来自同一提交并通过本机 GPU 完整回归；Linux CUDA 从 v1.14.8 起进入正式 Release，保持 `not-run`，并在页面明确说明尚未做 Linux 实机 GPU 验收。显式 `-DALLOW_UNVALIDATED_LINUX_CUDA=ON` 仅允许原生 Linux x64 tar.gz，不允许跳过 CTest、smoke 或解压验证，也不放宽 Windows CUDA 的 Required 门禁。

新包另记录 `CudaToolkit`（编译器组件版本 13.5.37）、`CudaRuntimeMode`（Windows `hybrid` / Linux `shared`）和 `GpuValidationRuntime`（实机运行时 API 版本，未验收为 `not-run`）。Driver 字段同样是 CUDA API 版本整数，均不是 nvidia-smi 的驱动包版本。Windows hybrid 从显示驱动加载运行时，包内不分发显示驱动或盲目复制 SDK DLL；Linux 仍收集实际共享运行库并设置相对 RPATH。历史包缺少这组字段仍可普通验收。迁移验证与成本见 [CUDA 13.5 迁移报告](../archive/gpu/cuda-13-5-migration.md)。

## Qt 桌面发布包

`Native packages` 为每个平台构建一个 Qt 桌面包。桌面包自带 CLI 和 worker。

| 平台 | 附件后缀 | 解压后启动 |
| --- | --- | --- |
| Windows x64 | `_windows-x64_qt_cuda-vulkan.zip` | `bin/MosaicraftDesktop.exe` |
| Linux x64 | `_linux-x64_qt_cuda-vulkan.tar.gz` | `./bin/MosaicraftDesktop` |
| macOS ARM64 | `_macos-arm64_qt.tar.gz` | `MosaicraftDesktop.app` |

桌面包均用 `cmake/qt-sdk.json` 锁定的 Qt SDK，随包部署 Qt 共享库、平台插件、SQLite、WebP、TIFF、支持内部 `desktop-worker` 模式的 CLI 及后端依赖。保持解压后的目录结构，无需另装 Qt 或 CUDA Toolkit。Linux 以 Ubuntu 24.04 上的构建为基线，仍需要操作系统的显示服务和驱动；macOS `.app` 为临时签名（ad-hoc），未公证。

双后端包的 CLI 编入 CUDA 和 Vulkan，覆盖普通命令和桌面工作模式。`auto` 使用 CUDA → CPU，Vulkan 可通过桌面设置显式选择。使用 GPU 时需安装相应显卡驱动。

所有桌面包都核对归档布局、Qt 库、插件及许可证，并生成 `.sha256`。双后端包另检查 CUDA 的七种原生架构及 PTX，解包后运行 GUI 和无 GPU 时的 CPU 回退测试。Windows 云端产物以 `_candidate` 结尾，正式包需实机通过 CUDA 和 Vulkan 预览；Linux 云端包的 GPU 实机状态记录为 `not-run`。可复查下载的桌面附件：

```powershell
cmake "-DARCHIVE=<archive>" "-DEXPECTED_SHA256=<sha256>" -P cmake/VerifyDesktopPackage.cmake
```

本机构建先按[源码构建](building.md)准备对应平台的锁定 vcpkg 依赖，设置 `VCPKG_ROOT`，Linux 另装工作流中列出的链接/显示依赖，再运行：

```powershell
cmake -DMODE=Qt -P cmake/PrepareRelease.cmake
cmake "-DQT_ROOT=build/tools/qt-sdk/6.12.0" -P cmake/ReleaseDesktop.cmake
```

Windows/Linux 双后端构建另需准备锁定的 CUDA SDK、Vulkan 头文件和离线着色器编译器（见 `Native packages`），然后传入 `-DRUNTIME=cuda-vulkan`；默认要求实机验证。没有 GPU 验收环境时显式加 `-DGPU_VALIDATION=BuildOnly`；Linux 冒烟测试还需 `xvfb` 与 `xauth`。

产物位于 `build/archives-qt/`。在 Windows/Linux 本机可给验包入口加 `-DRUN_SMOKE=ON` 执行归档中的 GUI；双后端包还检查禁用 GPU 后的回退，加 `-DREQUIRE_GPU=ON` 则同时要求实机通过两个后端。对外分发时保留包内许可证和源码来源说明，并随发布提供与锁定 Qt 版本对应的源码获取方式。

## 独立包验收

独立验收逐个平台下载两种附件。原生包验证 CLI、Web UI/API、智能裁剪模型与推理冒烟检查、图片生成、中文与空格路径、依赖加载位置和 Unix 执行权限。Qt 包另检查归档布局及 macOS 签名。设备验收状态记录在 `BUILD_INFO.txt`。

用 [独立原生验包器](releasing.md#使用原生验包器)验收本地归档或已发布的附件。历史包缺少新 GPU 证据字段时仍可普通验收；要求指定验收状态时，则必须匹配该状态。

正式发布后，手动运行 `Verify published release` 并传入版本标签，可下载并验收真实 Release 上的六个正式附件。该流程只读取 Release，不上传或修改附件。

历史测量与旧环境说明已移至[CI 与打包历史采样](../archive/performance/packaging-20261108.md)。

## 文档归档布局

新包复制完整 docs/ 树，文档中心、当前指南与历史报告保留同一相对位置，不再维护易遗漏的文件名清单。根目录只保留 README 等启动和许可文件；API 和历史百科从 docs/ 导航。版本摘要检查使用 docs/releases.md。独立验收入口同时识别旧包布局，不能要求历史包凭空增加新目录。

Vulkan/Metal 候选编译与验收状态见[四后端实现](portable-gpu.md)。
