# 源码构建

运行发布包无需安装编译器或 SDK。构建与公开发布是两个独立流程；版本与候选状态见[版本状态](../releases.md)。

## 平台预设

| 平台 | x64 | ARM64 | 生成器 |
| --- | --- | --- | --- |
| Windows | `windows-x64` | `windows-arm64` | Visual Studio 2026 |
| Linux | `linux-x64` | `linux-arm64` | Ninja |
| macOS | `macos-x64` | `macos-arm64` | Ninja，最低 macOS 13.0 |

六个预设默认构建 CPU-only Release，配置和构建用同一个预设名，产物位于 `build/<preset>/Release/bin`。切换 GPU 后端或保留多个变体时，在配置命令中用 `-B` 指定独立目录，再用 `cmake --build <目录>` 构建；Visual Studio 项目同时指定 `--config Release`。旧的 `CMakeUserPresets.json` 若引用已移除的预设，请先备份，再更新 `inherits` 和 `configurePreset`。

## Linux 与 macOS

源码构建使用 C++17 编译器、CMake 3.21+、Ninja，以及系统提供的 OpenCV 4、SQLite、TIFF、PNG 和 JPEG 开发库。Ubuntu 22.04 自带的软件源满足 CMake 版本要求。macOS 请先安装 Xcode Command Line Tools（`xcode-select --install`）和 Homebrew。

```sh
git clone https://github.com/CialloKing/Mosaicraft.git
cd Mosaicraft
./scripts/build-native.sh --install-deps
```

`--install-deps` 在 Debian/Ubuntu 上调用 apt，在 Fedora 上调用 dnf，在 macOS 上调用 Homebrew。开发库已安装时，直接运行 `./scripts/build-native.sh`，脚本会按本机平台和架构选择预设。也可手动执行 `cmake --preset linux-x64` 和 `cmake --build --preset linux-x64`，或换成对应的 ARM64、macOS 预设。自定义依赖位置可通过 `CMAKE_PREFIX_PATH` 提供。

- Linux 预设要求工具链的目标架构匹配预设名。交叉编译时用 `-DCMAKE_TOOLCHAIN_FILE=<文件>` 指定目标编译器和系统库。macOS 预设直接设置目标架构，依赖库也须提供相同架构。
- Linux CUDA 构建先安装 CUDA Toolkit 13.5.0（nvcc 13.5.37），再运行 `CUDAToolkit_ROOT=/usr/local/cuda-13.5 ./scripts/build-native.sh --cuda`。将路径换成实际安装位置。产物在 `build/<preset>-cuda/Release/bin`。 [CUDA 工具链](../archive/gpu/cuda-13-5-migration.md)

## Windows

使用 Visual Studio 2026 的 C++ 工具、CMake 4.2+ 和 Git。下列命令从仓库根目录执行，也可在不安装 PowerShell 或 Python 的环境中使用。已有锁定版本的 vcpkg 时，直接使用其路径；首次准备依赖是显式步骤。

```sh
git clone https://github.com/microsoft/vcpkg.git build/tools/vcpkg
git -C build/tools/vcpkg checkout a0400024711b283056538ac19ced80b91a83c24c
cmake -DMODE=Prepare -DTARGET_ARCH=x64 -DVCPKG_ROOT=build/tools/vcpkg -P cmake/PrepareRelease.cmake
cmake -DMODE=Install -DTARGET_ARCH=x64 -DVCPKG_ROOT=build/tools/vcpkg -P cmake/PrepareRelease.cmake
cmake -E env VCPKG_ROOT=build/tools/vcpkg cmake --preset windows-x64
cmake --build --preset windows-x64
```

使用 `windows-arm64` 时，安装 Visual Studio 的 ARM64 C++ 工具，并将两条依赖准备命令中的目标架构改为 `-DTARGET_ARCH=arm64`。在 x64 主机交叉构建时，vcpkg 会用 x64 主机工具和 ARM64 目标库。

Windows x64 依赖装到 `third_party/vcpkg_installed/`，ARM64 依赖装到独立的 `third_party/vcpkg_installed/arm64/`。清理 `build/` 后仍可复用已安装的依赖。

发布包使用独立的固定依赖和验收流程，推送 `v*` 版本标签会触发打包。 [发布清单](releasing.md)

## GPU 后端与编译配置

配置时可在基础预设上添加 GPU 开关。CPU 后端始终可用。 [GPU 后端与验收](portable-gpu.md)

| 后端 | 配置参数 | 构建依赖 |
| --- | --- | --- |
| CUDA | `-DMOSAICRAFT_CUDA=ON` | 指定 `CUDAToolkit_ROOT` 环境变量或传入 `-DCUDAToolkit_ROOT=<目录>`。 [CUDA 工具链与架构](cuda.md) |
| Vulkan | `-DMOSAICRAFT_VULKAN=ON` | Windows 可运行 `cmake -DMODE=Vulkan -P cmake/PrepareRelease.cmake`，将输出路径传给 `-DMOSAICRAFT_VULKAN_INCLUDE` 和 `-DMOSAICRAFT_GLSLANG`。Linux 可安装 Vulkan headers 和 glslang。 |
| Metal | `-DMOSAICRAFT_METAL=ON` | macOS 需要完整 Xcode 提供离线 Metal 编译器。 |

下面的命令将 Windows x64 的 Vulkan 构建放在独立目录。

```powershell
cmake --preset windows-x64 -B build/windows-x64-vulkan -DMOSAICRAFT_VULKAN=ON
cmake --build build/windows-x64-vulkan --config Release
```

在 Visual Studio 构建中用 `--config RelWithDebInfo` 选择带调试信息的优化产物。Linux 和 macOS 的 Ninja 预设在配置时用 `-DCMAKE_BUILD_TYPE=RelWithDebInfo`，下面以 Linux x64 为例。

```sh
cmake --preset linux-x64 -B build/linux-x64-relwithdebinfo -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/linux-x64-relwithdebinfo
```

## Qt Widgets 桌面端

桌面端位于 `desktop/`，不使用浏览器服务。根目录构建默认不查找 Qt，启用 `MOSAICRAFT_BUILD_DESKTOP` 才构建桌面端。Qt Designer 可直接打开 [主窗口](../../desktop/ui/MainWindow.ui)、[通用设置窗口](../../desktop/ui/SettingsDialog.ui) 和 [建库设置窗口](../../desktop/ui/LibrarySettingsDialog.ui)。在主窗口表单中选中 `workspaces`，用 `currentIndex` 切换编辑（0）、导出（1）和素材库（2）。

Windows 使用与目标架构匹配的 MSVC Qt 6.5+，包含 Widgets、Sql、Network 与 Linguist Tools。先准备依赖，再从仓库根目录运行以下 PowerShell 命令。需要 CUDA 时按前一节添加后端参数并指定 SDK。

```powershell
$env:VCPKG_ROOT = (Resolve-Path build/tools/vcpkg).Path
$preset = 'windows-x64'
$qtRoot = 'C:/Qt/6.12.0/msvc2022_64'

cmake --preset $preset -B build/windows-desktop -DMOSAICRAFT_BUILD_DESKTOP=ON "-DCMAKE_PREFIX_PATH=$qtRoot"
cmake --build build/windows-desktop --config Release --target MosaicraftDesktop --parallel

$env:PATH = "$qtRoot/bin;$env:PATH"
./build/windows-desktop/Release/bin/MosaicraftDesktop.exe
```

素材导入和便携工程渲染使用 `mosaicraft`。普通工程的成品预览、候选查询和换图后导出使用 `mosaicraft_desktop_worker`。也可在「设置 → 通用设置」选择 `build/<preset>/Release/bin/mosaicraft.exe`，匹配程序会从同目录加载。Qt 的 SQLite 插件用于图库浏览，WebP 和 TIFF 预览使用相应的 Qt 图像格式插件。

「保存」将本地文件引用、参数和手动换图记录写入 `.mscproj` 工程文件。导出 HTML 可将最终排列和素材打包为便携工程目录。已有便携工程可从目录或 `project.json` 打开，ZIP 需先解压。

Windows 设置上述 Qt PATH 后，运行 `ctest --test-dir build/windows-desktop -C Release -R "^desktop_smoke$" --output-on-failure`。根构建会自动为测试提供 CLI 路径，覆盖真实建库、预览、换图、保存、重开和导出。仍可使用 `cmake -S desktop` 单独构建 GUI；独立构建时，可设置 `MOSAICRAFT_DESKTOP_SMOKE_CLI` 为配有 worker 的 CLI 完整路径来启用同样的端到端检查。

## 构建和运行测试

配置时加 `-DTESTS=ON`，让普通构建一并编译测试所需的目标。构建完成后用 CTest 运行测试。

```sh
cmake --preset windows-x64 -DTESTS=ON
cmake --build --preset windows-x64
ctest --test-dir build/windows-x64 -C Release --output-on-failure
```

`TESTS` 默认 `OFF`，已有缓存可用 `-DTESTS=OFF` 恢复。对于已配置的构建目录，也可用 `cmake --build --preset windows-x64 --target mosaicraft_test_targets` 单独编译测试所需的目标。自定义了构建目录或编译配置时，把实际目录和配置传给 CTest。独立的前端命令检查由 CI 执行。
