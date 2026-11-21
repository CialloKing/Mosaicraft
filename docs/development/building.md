# 源码构建

运行发布包无需安装编译器或 SDK。构建与公开发布是两个独立流程；版本与候选状态见[版本状态](../releases.md)。

## Linux 与 macOS

源码构建使用 C++17 编译器、CMake 3.21+、Ninja，以及系统提供的 OpenCV 4、SQLite、TIFF、PNG 和 JPEG 开发库。Ubuntu 22.04 自带的软件源满足 CMake 版本要求。macOS 请先安装 Xcode Command Line Tools（`xcode-select --install`）和 Homebrew。

```sh
git clone https://github.com/CialloKing/Mosaicraft.git
cd Mosaicraft
./scripts/build-native.sh --install-deps
```

`--install-deps` 在 Debian/Ubuntu 上调用 apt，在 Fedora 上调用 dnf，在 macOS 上调用 Homebrew。开发库已安装时，直接运行 `./scripts/build-native.sh`。也可手动执行 `cmake --preset linux-cpu` 和 `cmake --build --preset linux-cpu`，macOS 使用 `macos-cpu`。自定义依赖位置可通过 `CMAKE_PREFIX_PATH` 提供。

- Linux CPU 产物在 `build/linux-cpu/Release/bin`，macOS 在 `build/macos-cpu/Release/bin`。进入目录后运行 `./MosaicraftWebUI` 或 `./mosaicraft --help`。
- Linux CUDA 构建先安装 CUDA Toolkit 13.5.0（nvcc 13.5.37），再运行 `CUDAToolkit_ROOT=/usr/local/cuda-13.5 ./scripts/build-native.sh --cuda`。将路径换成实际安装位置。产物在 `build/linux-cuda/Release/bin`。 [CUDA 工具链](../archive/gpu/cuda-13-5-migration.md)
- 原有 vcpkg 构建缓存可用新的构建目录避开，例如 `cmake --preset linux-cpu -B build/linux-system`，然后运行 `cmake --build build/linux-system`。

## Windows

使用 Visual Studio 2026 的 C++ 工具、CMake 4.2+ 和 Git。下列命令从仓库根目录执行，也可在不安装 PowerShell 或 Python 的环境中使用。已有锁定版本的 vcpkg 时，直接使用其路径；首次准备依赖是显式步骤。

```sh
git clone https://github.com/microsoft/vcpkg.git build/tools/vcpkg
git -C build/tools/vcpkg checkout a0400024711b283056538ac19ced80b91a83c24c
cmake -DMODE=Prepare -DVCPKG_ROOT=build/tools/vcpkg -P cmake/PrepareRelease.cmake
cmake -DMODE=Install -DVCPKG_ROOT=build/tools/vcpkg -P cmake/PrepareRelease.cmake
cmake -E env VCPKG_ROOT=build/tools/vcpkg cmake --preset windows-cpu
cmake --build --preset windows-cpu
```

使用 CUDA 时将 `windows-cpu` 换成 `windows-cuda`，配置时通过 `-DCUDAToolkit_ROOT="C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v13.5"` 指定实际 SDK。 [架构与单卡构建](cuda.md)

随源码分发的第三方库和 vcpkg 安装的依赖都在 `third_party/`；Windows 构建预设使用 `third_party/vcpkg_installed/`。清理 `build/` 后仍可复用已安装的依赖。

构建产物位于 `build/<preset>/Release/bin`。

发布包使用独立的固定依赖和验收流程，推送 `v*` 版本标签会触发打包。 [发布清单](releasing.md)

## Qt Widgets 桌面端

桌面端位于 `desktop/`，不使用浏览器服务。根目录构建默认不查找 Qt，启用 `MOSAICRAFT_BUILD_DESKTOP` 才构建桌面端。Qt Designer 可直接打开 [主窗口](../../desktop/ui/MainWindow.ui)、[通用设置窗口](../../desktop/ui/SettingsDialog.ui) 和 [建库设置窗口](../../desktop/ui/LibrarySettingsDialog.ui)。在主窗口表单中选中 `workspaces`，用 `currentIndex` 切换编辑（0）、导出（1）和素材库（2）。

Windows 使用与 MSVC x64 匹配的 Qt 6.5+，包含 Widgets、Sql、Network 与 Linguist Tools。先按前一节准备依赖，再从仓库根目录运行以下 PowerShell 命令。CPU 使用 `windows-cpu`，CUDA 使用 `windows-cuda` 并在配置时按前一节指定实际 SDK。

```powershell
$env:VCPKG_ROOT = (Resolve-Path build/tools/vcpkg).Path
$preset = 'windows-cpu'
$qtRoot = 'C:/Qt/6.12.0/msvc2022_64'

cmake --preset $preset -B build/windows-desktop -DMOSAICRAFT_BUILD_DESKTOP=ON "-DCMAKE_PREFIX_PATH=$qtRoot"
cmake --build build/windows-desktop --config Release --target MosaicraftDesktop --parallel

$env:PATH = "$qtRoot/bin;$env:PATH"
./build/windows-desktop/Release/bin/MosaicraftDesktop.exe
```

素材导入和便携工程渲染使用 `mosaicraft`。普通工程的成品预览、候选查询和换图后导出使用 `mosaicraft_desktop_worker`。也可在「设置 → 通用设置」选择 `build/<preset>/Release/bin/mosaicraft.exe`，匹配程序会从同目录加载。Qt 的 SQLite 插件用于图库浏览，WebP 和 TIFF 预览使用相应的 Qt 图像格式插件。

「保存」将本地文件引用、参数和手动换图记录写入 `.mscproj` 工程文件。导出 HTML 可将最终排列和素材打包为便携工程目录。已有便携工程可从目录或 `project.json` 打开，ZIP 需先解压。

Windows 设置上述 Qt PATH 后，运行 `ctest --test-dir build/windows-desktop -C Release -R "^desktop_smoke$" --output-on-failure`。根构建会自动为测试提供 CLI 路径，覆盖真实建库、预览、换图、保存、重开和导出。仍可使用 `cmake -S desktop` 单独构建 GUI；独立构建时，可设置 `MOSAICRAFT_DESKTOP_SMOKE_CLI` 为配有 worker 的 CLI 完整路径来启用同样的端到端检查。

## Vulkan 与 Metal

Vulkan 与 Metal 都是独立开关，默认 OFF，五个标准发布包保持关闭，CPU 构建不需要 GPU SDK。源码启用 Vulkan 时选用下方 Vulkan preset，或在配置时显式传入 `-DMOSAICRAFT_VULKAN=ON`。开发者 preset 的启用不会影响正式包；发布脚本再次显式设置开关并检查缓存，避免复用目录带入额外后端。设备与性能准入见[四后端实现与验收](portable-gpu.md)。

- Windows：运行 `cmake -DMODE=Vulkan -P cmake/PrepareRelease.cmake`，然后用其输出设置 `-DMOSAICRAFT_VULKAN_INCLUDE` 和 `-DMOSAICRAFT_GLSLANG`，再配置 `windows-vulkan` 或 `windows-cuda-vulkan`。
- Linux：安装 Vulkan headers 和 glslang，使用 linux-vulkan preset。
- macOS：在完整 Xcode 环境使用 macos-metal preset，Command Line Tools 本身不足以提供完整 Metal 离线工具链。

着色器在构建时离线编译并嵌入程序，运行发布包无需安装着色器开发工具。纯 CPU、CUDA、Vulkan 和 Metal 使用独立构建目录，避免沿用其他后端的缓存。

## 单独构建回归工具

普通构建不编译测试程序，也不探测解释器。配置后运行对应的 `-tests` 构建预设，再沿用同名测试预设（不带 `-tests`）：

```sh
cmake --build --preset windows-cpu-tests
ctest --preset windows-cpu --output-on-failure
```

更换平台或后端时两条命令使用同一套预设。使用 `-B` 覆盖构建目录时，改用 `cmake --build <目录> --config Release --target mosaicraft_test_targets` 和 `ctest --test-dir <目录> -C Release --output-on-failure`。独立的前端命令检查由 CI 执行，不进入 CTest 发布门禁。
