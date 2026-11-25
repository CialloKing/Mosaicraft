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

源码构建使用 C++17 编译器、CMake 3.21+、Ninja。依赖使用与 CI 同一套锁定的 vcpkg 配置，其中 OpenCV 版本为 **5.1.0**。macOS 请先安装 Xcode Command Line Tools（`xcode-select --install`）和 Homebrew。

首次构建时，安装工具链；已安装可跳过：

```sh
# Debian / Ubuntu
sudo apt-get update
sudo apt-get install -y --no-install-recommends build-essential cmake ninja-build git curl zip unzip tar pkg-config

# Fedora
sudo dnf install -y gcc-c++ cmake ninja-build git curl zip unzip tar pkgconf-pkg-config

# macOS
brew install cmake ninja
```

从仓库根目录准备依赖并构建。把预设改为本机系统和架构；此流程不要求安装系统 OpenCV。

```sh
git clone https://github.com/microsoft/vcpkg.git build/tools/vcpkg
git -C build/tools/vcpkg checkout a0400024711b283056538ac19ced80b91a83c24c
cmake -DMODE=Prepare -DVCPKG_ROOT=build/tools/vcpkg -P cmake/PrepareRelease.cmake
cmake -DMODE=Install -DVCPKG_ROOT=build/tools/vcpkg -P cmake/PrepareRelease.cmake

preset=linux-x64
triplet=x64-linux-release # 与准备步骤打印的 triplet 一致；macOS 使用 *-osx-release
cmake --preset "$preset" \
  "-DCMAKE_TOOLCHAIN_FILE=$PWD/build/tools/vcpkg/scripts/buildsystems/vcpkg.cmake" \
  "-DVCPKG_INSTALLED_DIR=$PWD/third_party/vcpkg_installed" \
  "-DVCPKG_TARGET_TRIPLET=$triplet" "-DVCPKG_HOST_TRIPLET=$triplet" -DVCPKG_MANIFEST_INSTALL=OFF
cmake --build --preset "$preset" --target mosaicraft MosaicraftWebUI --parallel 2
```

也可自行提供 OpenCV **5.1.0**（`core`、`imgproc`、`imgcodecs`）和 SQLite、TIFF、PNG、JPEG、Zlib 开发库。不要使用系统的 OpenCV 4.x；必要时用 `-DOpenCV_DIR=<包含 OpenCVConfig.cmake 的目录>` 指定 5.1.0 安装。升级旧工作区后建议使用新构建目录，避免沿用曾指向 4.x 的 CMake 缓存。

- Linux 预设要求工具链的目标架构匹配预设名。交叉编译时用 `-DCMAKE_TOOLCHAIN_FILE=<文件>` 指定目标编译器和系统库。macOS 预设直接设置目标架构，依赖库也须提供相同架构。
- Linux CUDA 构建先安装 CUDA Toolkit 13.5.0（nvcc 13.5.37），配置时在基础预设上启用 CUDA 并指定 SDK，使用独立目录： [CUDA 工具链](../archive/gpu/cuda-13-5-migration.md)

  在上方的配置命令中追加 `-B build/linux-x64-cuda -DMOSAICRAFT_CUDA=ON -DCUDAToolkit_ROOT=/usr/local/cuda-13.5`，再运行 `cmake --build build/linux-x64-cuda --target mosaicraft MosaicraftWebUI --parallel 2`。

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

## 智能裁剪（可选）

`MOSAICRAFT_SMART_CROP` 默认关闭。CPU 推理需要带 `dnn` 和 `objdetect` 的 OpenCV 5.1.0 和锁定 ONNX 模型。Windows/Linux 双后端发行版还需要 CUTLASS CUDA 依赖、Vulkan ncnn SDK 及转换模型。[本地构建和验收](smart-crop.md) · [双后端打包步骤](packaging.md#cmake-发布入口)

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

桌面端位于 `desktop/`，编辑页和导出页的实现分别位于 `desktop/EditPage/` 和 `desktop/ExportPage/`。根目录构建默认不查找 Qt，启用 `MOSAICRAFT_BUILD_DESKTOP` 才构建桌面端。Qt Designer 可直接打开主窗口和设置窗口，素材库配置位于设置窗口的 `librarySettingsPage`。在主窗口表单中选中 `workspaces`，用 `currentIndex` 切换编辑（0）和导出（1）。源文件位于 `desktop/ui/`。

Windows 使用与目标架构匹配的 MSVC Qt 6.5+，包含 Widgets、Sql、Network 与 Linguist Tools。先准备依赖，再从仓库根目录运行以下 PowerShell 命令。需要 CUDA 时按前一节添加后端参数并指定 SDK。

```powershell
$env:VCPKG_ROOT = (Resolve-Path build/tools/vcpkg).Path
$preset = 'windows-x64'
$qtRoot = 'C:/Qt/6.12.0/msvc2022_64'

cmake --preset $preset -B build/windows-desktop -DMOSAICRAFT_BUILD_DESKTOP=ON "-DCMAKE_PREFIX_PATH=$qtRoot"
cmake --build build/windows-desktop --config Release --target MosaicraftDesktop --parallel

./build/windows-desktop/Release/bin/MosaicraftDesktop.exe
```

Windows 桌面构建会把 Qt DLL、`qt.conf` 和 `plugins/` 放在可执行文件旁，可直接启动程序。需要 WebP 和 TIFF 预览时，请在构建用的 Qt SDK 中安装 Qt Image Formats 组件。

桌面构建将素材库维护和编辑会话编入 `mosaicraft` 的内部 `desktop-worker` 模式。桌面端也用同一个程序执行 `render` 等普通 CLI 命令。若在「设置 → 通用设置」指定程序路径，请选择桌面构建产出的 `build/<preset>/Release/bin/mosaicraft.exe`。

在「设置 → 素材库设置」选择原始素材目录和素材库目录。素材库目录留空时，程序将生成的素材库存放在本机应用数据目录。每次启动会检查素材图片和数据库中的特征数据，并核对原图内容与建库设置。首次创建、出现损坏或检测到变化时会构建新的版本，校验通过后再启用。只选择已有图库目录时，启动会检查该图库的完整性，自动重建需要设置原始素材目录。

「保存」将本地文件引用、参数和手动换图记录写入 `.mscproj` 工程文件。导出 HTML 可将最终排列和素材打包为便携工程目录。从目录、`index.html` 或 `project.json` 打开便携工程时，桌面版会载入包内目标图和参数，并按保存的素材排列恢复成品预览。ZIP 需先解压。

Windows 构建完成后，运行 `ctest --test-dir build/windows-desktop -C Release -R "^desktop_smoke$" --output-on-failure`。根构建会自动为测试提供 CLI 路径，覆盖真实建库、预览、换图、保存、重开和导出。也可使用 `cmake -S desktop` 单独构建 GUI，独立构建时可设置 `MOSAICRAFT_DESKTOP_SMOKE_CLI` 为配有 worker 的 CLI 完整路径来启用同样的端到端检查。

## 构建和运行测试

配置时加 `-DTESTS=ON`，让普通构建一并编译测试所需的目标。构建完成后用 CTest 运行测试。

```sh
cmake --preset windows-x64 -DTESTS=ON
cmake --build --preset windows-x64
ctest --test-dir build/windows-x64 -C Release --output-on-failure
```

`TESTS` 默认 `OFF`，已有缓存可用 `-DTESTS=OFF` 恢复。对于已配置的构建目录，也可用 `cmake --build --preset windows-x64 --target mosaicraft_test_targets` 单独编译测试所需的目标。自定义了构建目录或编译配置时，把实际目录和配置传给 CTest。独立的前端命令检查由 CI 执行。
