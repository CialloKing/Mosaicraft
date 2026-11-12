# Mosaicraft

Mosaicraft 用图库里的照片拼出一张目标图，可通过本地 Web UI 或命令行运行。生成前可以实时预览并逐格换图，生成时可用 NVIDIA CUDA 加速，并将超大图流式写入文件。

[下载最新版](https://github.com/CialloKing/Mosaicraft/releases/latest) · [快速开始](#快速开始) · [使用指南](#使用指南) · [CUDA 模板](#cuda-分辨率模板) · [源码构建](#安装与构建) · [Star 历史](#star-历史)

（本项目纯AI生成，绝无手写代码）

## 下载与运行

在 [GitHub Releases](https://github.com/CialloKing/Mosaicraft/releases/latest) 的 **Assets** 中，按平台和文件名后缀选择发布包。使用支持 CUDA 13.5 的 NVIDIA 驱动时可选 CUDA 包，其余情况选 CPU 包。

| 平台 | 系统基线 | CPU 包后缀 | CUDA 包后缀 |
| --- | --- | --- | --- |
| Windows x64 | Windows 10 22H2 | `_windows-x64_cpu-only.zip` | `_windows-x64_cuda.zip` |
| Linux x64 | Ubuntu 22.04 / glibc 2.35 | `_linux-x64_cpu-only.tar.gz` | `_linux-x64_cuda.tar.gz` |
| macOS ARM64 | macOS 13 | `_macos-arm64_cpu-only.tar.gz` | — |

包内 `BUILD_INFO.txt` 记录构建配置和 GPU 验收状态。v1.15.4 按维护者要求跳过全部测试，Windows/Linux CUDA 包均为 `GpuValidation: not-run`，未做 GPU 实机验收。 [v1.15.4 发布说明与校验值](https://github.com/CialloKing/Mosaicraft/releases/tag/v1.15.4)

## 快速开始

准备一张**目标图**和一个**素材目录**。建库后，Mosaicraft 会从图库中挑选素材，逐格拼出目标图。

### Web UI

解压后，Windows 双击 `MosaicraftWebUI.exe`，Linux/macOS 在解压目录运行 `./MosaicraftWebUI`。浏览器默认打开 [http://localhost:8080](http://localhost:8080)。

1. **建库。** 在「建库」页填写素材目录和图库输出目录，点击「运行」。完成后点击「使用此图库」，将数据库路径填入生成表单。
2. **设置输出。** 在「生成马赛克」页填写目标图和输出路径。用「输出规划」查看尺寸、内存和文件体积估算，展开「目标与图库覆盖」检查亮度候选不足的区域。初次试跑可将「输出 Tile 尺寸」设为 `18x32`。
3. **预览与选图（可选）。** 在「预览」页查看目标参考图、素材复用和匹配误差，或手动替换单格素材。可保存 A/B 快照比较参数。保持当前预览开启并完成匹配，正式生成会沿用选图结果。
4. **生成。** 点击「运行」，在「任务与结果」查看文件、阶段耗时和历史摘要。生成前勾选「质量分析」可查看误差直方图和热力图，勾选「资源监控」可查看进程 CPU、内存及可用的 CUDA 指标。

相对路径以页面顶部显示的服务工作目录为准。更多参数和示例见「使用帮助」。

### 命令行

在解压目录运行以下命令。`photos` 是素材目录，`target.jpg` 是目标图。示例生成 `result.png` 和便于查看的 `result_preview.jpg`。Windows PowerShell 也可运行 `./mosaicraft.exe`。

```powershell
./mosaicraft build -i photos -o library -r
./mosaicraft mosaic -i target.jpg -d library/mosaicraft.db -o result.png --output-tile 18x32 --preview
```

换一张目标图时可直接复用 `library/mosaicraft.db`。运行 `./mosaicraft --help` 可查看完整命令和选项。

## 使用指南

### 建库与追加

建库时，素材会按指定尺寸等比缩放、中心裁剪，随后提取特征、按解码像素去重，并按综合色调编号。建议把素材目录和图库目录分开，程序会在图库目录中处理副本。尺寸和格式已符合要求的素材会保留原文件字节。 [同尺寸免重编码](docs/SAME_SIZE_NORMALIZATION.md)

```powershell
# 建立 270×480 图库
./mosaicraft build -i photos -o library/270x480 --normalize-size 270x480 -r

# 追加素材时沿用同一图库目录和归一化尺寸
./mosaicraft build -i new_photos -o library/270x480 --normalize-size 270x480 --append -r
```

- **换尺寸**时，在新目录建库。`--normalize-size` 默认 `180x320`，每轴支持 1～8192 像素。
- **追加素材**后，数字文件名可能变化，数据库中的素材 ID 和使用统计会保留。
- **搬移图库**时，先关闭使用图库的程序，再复制整个图库目录，包括 `mosaicraft.db`、素材和 `features/`。 [可搬移图库](docs/PORTABLE_LIBRARIES.md)
- **恢复中断的建库**时，若图库留有 `.color-order.json`，恢复文件访问后，用原参数加 `--append` 继续。

### 匹配尺寸与输出尺寸

| 尺寸 | 参数 | 如何选择 |
| --- | --- | --- |
| 图库归一化尺寸 | `build --normalize-size WxH` | 按素材比例选择，生成时从图库读取。该尺寸也决定 CUDA 模板。 |
| 匹配 Tile | `--tile-w`、`--tile-h` | 默认 9×16 像素。想用更多素材块拼出轮廓时，缩小匹配 Tile。 |
| 输出 Tile | `--output-tile WxH` | 默认 180×320，横向图库使用默认值时取 320×180。想缩小成品尺寸时，按比例缩小输出 Tile。 |

成品宽高由各轴的格数乘以输出 Tile 的对应边长得到。例如，90×160 的目标图按 9×16 分成 10×10 格，输出 Tile 用 180×320 时，成品是 1800×3200。改用 18×32 时，成品是 180×320，仍然匹配 100 格。`--out-w`、`--out-h` 和 `--upscale` 在切块前调整目标图尺寸。

### 生成与导出

```powershell
# 流式写入大图，同时保存便于查看的 JPEG 预览
./mosaicraft mosaic -i target.jpg -d library/mosaicraft.db -o result.png --write-mode stream --preview

# 使用 20% 的目标图叠加强度，并保存选图质量报告
./mosaicraft mosaic -i target.jpg -d library/mosaicraft.db -o blended.png --overlay-strength 0.2 --analyze
```

单图格式按 **`--format` → 输出文件后缀 → 输入文件后缀 → PNG** 的顺序确定。

| 格式 | 选项与超限处理 |
| --- | --- |
| JPG、WebP | `--quality` 取 1～100，默认 100。单轴上限分别为 65,500 和 16,383 像素。超限时，自动推导的格式转为等尺寸 PNG，显式指定的格式尝试缩小输出 Tile。 |
| PNG | `--png-level 1～9`，默认 1。支持 `--write-mode stream` 降低大图写入时的内存占用。 |
| TIFF | 支持 BigTIFF 和流式写入，适合超大单图。 |

导出单图时，`--preview` 会在主图旁保存 `<主图名>_preview.jpg`。需要单独保存每个图块时用 `--tiled`，需要分层缩放浏览时用 `--deepzoom`。 [输出格式与超限处理](docs/OUTPUT_FORMAT_VERIFICATION.md) · [实时预览与 JPEG 预览](docs/PREVIEW_VERIFICATION.md)

### 常用生成参数

| 需求 | 参数 |
| --- | --- |
| 调节选图多样性 | `--candidates` 控制召回候选数，默认 150。`--topn-random` 控制精排后的随机选图范围，默认 10。 |
| 找不到候选素材 | 遇到 `no_candidates` 时，可增大 `--l-range`（默认 20），或补充与目标图亮度相近的素材。 |
| 减少素材重复 | `--neighbor-window` 控制邻域范围，`--neighbor-penalty` 控制相邻重复的惩罚，`--penalty` 控制素材复用惩罚。 |
| 保留目标图轮廓 | `--overlay-strength` 取 0～1，默认 0。设为 0.2 会混合 80% 素材和 20% 对应位置的目标图。 |
| 微调素材亮度 | 启用 `--color-adjust` 后，`--color-strength` 控制随机亮度扰动，默认 ±4%。 |
| 查看质量和耗时 | `--analyze` 生成报告和热力图，`--benchmark` 显示分阶段耗时。 |
| 强制使用 CPU | `--cpu` |

### 图库管理

| 命令 | 用途 |
| --- | --- |
| `./mosaicraft db-stats -d library/mosaicraft.db` | 查看图库规模、LAB 分布和亮度覆盖。 |
| `./mosaicraft db-health -d library/mosaicraft.db` | 检查图库完整性和缺失资源。 |
| `./mosaicraft db-usage -d library/mosaicraft.db` | 查看素材使用统计，可用 `--export <目录>` 导出使用过的素材。 |
| `./mosaicraft db-purge -d library/mosaicraft.db` | 确认后清理图库中图片文件已缺失的记录及关联特征。 |

## CUDA 分辨率模板

CUDA 可用时，建库、实时预览和生成会根据图库的归一化尺寸自动选择模板，用于特征提取和候选评分。其他合法尺寸可走与 CPU 兼容的通用 CUDA 路径。

| 方向 | v1.15.2 发布包 | v1.15.4 发布包 |
| --- | --- | --- |
| 正方形 | — | 256×256、512×512 |
| 竖屏 9:16 | 180×320、360×640 | 180×320、270×480、360×640、540×960 |
| 横屏 16:9 | 320×180、640×360 | 320×180、480×270、640×360、960×540 |

预检发现 CUDA 不可用时，任务使用 CPU 并记录原因。 [CUDA 架构与验收](docs/CUDA_ARCHITECTURES.md)

## 安装与构建

### Linux 与 macOS

源码构建使用 C++17 编译器、CMake 3.21+、Ninja，以及系统提供的 OpenCV 4、SQLite、TIFF、PNG 和 JPEG 开发库。Ubuntu 22.04 自带的软件源满足 CMake 版本要求。macOS 请先安装 Xcode Command Line Tools（`xcode-select --install`）和 Homebrew。

```sh
git clone https://github.com/CialloKing/Mosaicraft.git
cd Mosaicraft
./scripts/build-native.sh --install-deps
```

`--install-deps` 在 Debian/Ubuntu 上调用 apt，在 Fedora 上调用 dnf，在 macOS 上调用 Homebrew。开发库已安装时，直接运行 `./scripts/build-native.sh`。也可手动执行 `cmake --preset linux-cpu` 和 `cmake --build --preset linux-cpu`，macOS 使用 `macos-cpu`。自定义依赖位置可通过 `CMAKE_PREFIX_PATH` 提供。

- Linux CPU 产物在 `build/linux-cpu/Release/bin`，macOS 在 `build/macos-cpu/Release/bin`。进入目录后运行 `./MosaicraftWebUI` 或 `./mosaicraft --help`。
- Linux CUDA 构建先安装 CUDA Toolkit 13.5.0（nvcc 13.5.37），再运行 `CUDAToolkit_ROOT=/usr/local/cuda-13.5 ./scripts/build-native.sh --cuda`。将路径换成实际安装位置。产物在 `build/linux-cuda/Release/bin`。 [CUDA 工具链](docs/CUDA_13_5_MIGRATION.md)
- 原有 vcpkg 构建缓存可用新的构建目录避开，例如 `cmake --preset linux-cpu -B build/linux-system`，然后运行 `cmake --build build/linux-system`。

### Windows

使用 Visual Studio 2026 的 C++ 工具、CMake 4.2+、PowerShell 7 和 Git。已有对应提交的 vcpkg 时，设置 `VCPKG_ROOT` 后即可依次运行 `Prepare` 和 `Install`。

```powershell
git clone https://github.com/CialloKing/Mosaicraft.git
cd Mosaicraft
git clone https://github.com/microsoft/vcpkg.git build/tools/vcpkg
git -C build/tools/vcpkg checkout a0400024711b283056538ac19ced80b91a83c24c
$env:VCPKG_ROOT = (Resolve-Path build/tools/vcpkg).Path
./scripts/setup-dependencies.ps1 -Mode Prepare
./scripts/setup-dependencies.ps1 -Mode Install
```

使用 CUDA 时将下方的 `windows-cpu` 换成 `windows-cuda`，并设置 `$env:CUDAToolkit_ROOT = 'C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v13.5'`。 [架构与单卡构建](docs/CUDA_ARCHITECTURES.md)

```powershell
$preset = 'windows-cpu'
cmake --preset $preset
cmake --build --preset $preset --target mosaicraft MosaicraftWebUI --parallel
```

构建产物位于 `build/<preset>/Release/bin`。

发布包使用独立的固定依赖和验收流程，推送 `v*` 版本标签会触发打包。 [发布清单](docs/RELEASE_CHECKLIST.md)

## 开发文档

- 接口文档列出请求字段、异步任务和诊断信息。运行 Web UI 后可通过 `GET /api/info` 查看版本和能力。 [REST API](docs/API.md)
- 匹配流程使用 AvgLAB、8×8 Grid、TinyImage、边缘密度和 LBP，结合候选召回、评分与邻域去重。 [项目百科](docs/ENCYCLOPEDIA.md)
- 固定样本的预览耗时、流式 PNG 导出和 CPU 建库结果。 [v1.15.2 预览性能对照](docs/PREVIEW_VERIFICATION.md#v1152-预览性能对照)
- 原生包构建、GPU 验收和 SHA256 校验流程。 [CI 与打包](docs/CI_PACKAGING.md) · [发布清单](docs/RELEASE_CHECKLIST.md)

## Star 历史

<p align="center">
  <a href="https://github.com/CialloKing/Mosaicraft/blob/star-history/stars.csv">
    <img src="https://raw.githubusercontent.com/CialloKing/Mosaicraft/refs/heads/star-history/star-history.svg" alt="Mosaicraft Star 数量历史图" width="960">
  </a>
</p>

GitHub Actions 每天北京时间 03:17 记录 Star 数量。首批历史按采集时仍有效的 Star 时间重建，后续以每日实测为准。 [查看 CSV 原始数据](https://github.com/CialloKing/Mosaicraft/blob/star-history/stars.csv)

## 许可证

本项目采用 GNU 通用公共许可证第 3 版（`GPL-3.0-only`）。 [许可证全文](LICENSE)
