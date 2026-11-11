# Mosaicraft

Mosaicraft 是一款本地照片马赛克生成器。它用图库中的照片拼出目标图，支持 CUDA 加速、浏览器实时预览和超大图流式输出。

[下载发布包](https://github.com/CialloKing/Mosaicraft/releases/latest) · [快速开始](#快速开始) · [CUDA 模板](#cuda-分辨率模板) · [源码构建](#安装与构建) · [Star 历史](#star-历史)

（本项目纯AI生成，绝无手写代码）

## 下载与运行

按文件名后缀选择与系统匹配的发布包。

| 平台 | CPU 包 | CUDA 包 | 系统基线 |
| --- | --- | --- | --- |
| Windows x64 | `_windows-x64_cpu-only.zip` | `_windows-x64_cuda.zip` | Windows 10 22H2 |
| Linux x64 | `_linux-x64_cpu-only.tar.gz` | `_linux-x64_cuda.tar.gz` | Ubuntu 22.04 / glibc 2.35 |
| macOS ARM64 | `_macos-arm64_cpu-only.tar.gz` | — | macOS 13 |

CUDA 发布包需要支持 CUDA 13.5 的 NVIDIA 驱动。包内 `BUILD_INFO.txt` 记录构建和 GPU 验收状态。Windows CUDA 发布包已通过实机验收，Linux CUDA 发布包的验收覆盖编译和 CPU 回退。 [发布说明](https://github.com/CialloKing/Mosaicraft/releases/tag/v1.15.2) · [CUDA 架构与验收](docs/CUDA_ARCHITECTURES.md)

## 快速开始

### Web UI

在解压目录双击 `MosaicraftWebUI.exe`，或在 Linux/macOS 运行 `./MosaicraftWebUI`。默认打开浏览器访问 [http://localhost:8080](http://localhost:8080)。

1. 打开「建库」，选择素材目录和图库输出目录，点击「运行」。
2. 打开「生成马赛克」，填写目标图片、图库和输出路径。
3. 可先到「预览」页点击「开启预览」，从画面中心以最大倍率查看选图。可用「适应窗口」查看全图，或开启「手动选图」替换素材。返回生成页点击「运行」后，结果区会给出实际文件路径、格式和尺寸。

相对路径以页面显示的服务工作目录为准。参数说明和更多示例可在顶部「使用帮助」查看。

### 命令行

在解压目录运行。以下 `./mosaicraft` 写法也可在 Windows PowerShell 中调用 `mosaicraft.exe`。示例将 `photos` 中的图片建成图库，再用较小的输出 Tile 生成主图及 JPEG 预览。

```powershell
./mosaicraft build -i photos -o library -r
./mosaicraft mosaic -i target.jpg -d library/mosaicraft.db -o result.png --output-tile 18x32 --preview
```

图库可以反复用于生成其他目标图。完整命令和选项见 `./mosaicraft --help`。

## 使用指南

### 建库与追加

建库按指定尺寸等比缩放、中心裁剪，提取特征并按解码像素去重，最后按综合色调对素材重编号。输入与图库使用独立目录时，程序处理素材副本。原图尺寸和格式均符合要求时，直接保留其文件字节。 [同尺寸免重编码](docs/SAME_SIZE_NORMALIZATION.md)

```powershell
# 建立 270×480 图库
./mosaicraft build -i photos -o library/270x480 --normalize-size 270x480 -r

# 追加素材时沿用同一图库目录和归一化尺寸
./mosaicraft build -i new_photos -o library/270x480 --normalize-size 270x480 --append -r
```

- `--normalize-size` 默认 `180x320`，宽高各支持 1～8192。换尺寸时为新图库指定独立输出目录。
- 追加后素材的数字文件名可能变化，数据库中的素材 ID 和使用统计会保留。
- 搬移图库时，关闭正在使用图库的程序后复制整个目录，包括 `mosaicraft.db`、素材和 `features/`。 [可搬移图库](docs/PORTABLE_LIBRARIES.md)
- v1.15.0 以前的图库需在新目录重新建库。当前格式的图库若因建库中断留下 `.color-order.json`，恢复文件访问后，用原参数加 `--append` 继续。 [图库兼容与恢复](docs/BREAKING_1.15.0.md)

### 匹配尺寸与输出尺寸

| 尺寸 | 参数 | 影响 |
| --- | --- | --- |
| 图库归一化尺寸 | `build --normalize-size WxH` | 统一图库素材规格，决定 CUDA 模板。生成时从图库读取。 |
| 匹配 Tile | `--tile-w`、`--tile-h` | 决定目标图的分格密度，默认 9×16 像素。格子越小，匹配次数越多。 |
| 输出 Tile | `--output-tile WxH` | 决定每个格子在成品中占多少像素。默认 180×320，横屏图库采用 320×180。 |

例如，90×160 的目标图以 9×16 分格会得到 10×10 个格子。输出 Tile 为 180×320 时，成品为 1800×3200。改用 18×32 时，成品为 180×320，匹配次数仍为 100。`--out-w`、`--out-h` 和 `--upscale` 可在切块前调整目标图尺寸。

### 生成与导出

```powershell
# 流式写入大图，同时保存便于查看的 JPEG 预览
./mosaicraft mosaic -i target.jpg -d library/mosaicraft.db -o result.png --write-mode stream --preview

# 使用 20% 的目标图叠加强度，并保存选图质量报告
./mosaicraft mosaic -i target.jpg -d library/mosaicraft.db -o blended.png --overlay-strength 0.2 --analyze
```

| 需求 | 参数 |
| --- | --- |
| 调节随机选图 | `--candidates` 控制 ANN 候选数量，默认 150。`--topn-random` 控制精排后的随机池，默认 10。 |
| 调节重复惩罚 | `--neighbor-window` 控制邻域范围，`--neighbor-penalty` 控制相邻重复的惩罚，`--penalty` 控制素材复用惩罚。 |
| 保留目标图轮廓 | `--overlay-strength` 取 0～1，默认 0。设为 0.2 会混合 80% 素材和 20% 对应位置的目标图。 |
| 微调素材亮度 | 启用 `--color-adjust` 后，`--color-strength` 控制亮度扰动，默认 ±4%。 |
| 查看质量和耗时 | `--analyze` 生成报告和热力图，`--benchmark` 显示分阶段耗时。 |
| 使用纯 CPU 生成 | `--cpu`。 |

单图格式按 **显式 `--format` → 输出后缀 → 输入后缀 → PNG** 的顺序确定。自定义输出路径时先创建其父目录。

| 格式 | 选项与超限处理 |
| --- | --- |
| JPG、WebP | `--quality` 取 1～100，默认 100。单轴上限分别为 65,500 和 16,383 像素。超限时，自动模式转为等尺寸 PNG，显式选格式则缩小输出 Tile。 |
| PNG | `--png-level 1～9`，默认 1。支持 `--write-mode stream` 降低大图写入时的内存占用。 |
| TIFF | 支持 BigTIFF 和流式写入，适合超大单图。 |

`--preview` 是单图导出选项，会在实际主图旁保存 `<主图名>_preview.jpg`。Web UI 的「预览」用于生成前检查和更换素材。需要分块或缩放浏览时，可使用 `--tiled` 或 `--deepzoom`。 [输出格式与超限处理](docs/OUTPUT_FORMAT_VERIFICATION.md) · [实时预览和 JPEG 预览](docs/PREVIEW_VERIFICATION.md)

### 图库管理

| 命令 | 用途 |
| --- | --- |
| `./mosaicraft db-stats -d library/mosaicraft.db` | 查看图库规模、LAB 分布和亮度覆盖。 |
| `./mosaicraft db-health -d library/mosaicraft.db` | 检查图库完整性和缺失资源。 |
| `./mosaicraft db-usage -d library/mosaicraft.db` | 查看素材使用统计，可用 `--export <目录>` 导出使用过的素材。 |
| `./mosaicraft db-purge -d library/mosaicraft.db` | 确认后清理源图片已缺失的记录及关联特征。 |

## CUDA 分辨率模板

CUDA 可用时，建库、实时预览和生成按图库归一化尺寸自动选择模板。模板路径使用 CUDA 提取特征和进行候选评分，其他合法尺寸可使用与 CPU 兼容的通用 CUDA 路径。

| 方向 | v1.15.2 发布包中的模板 | 当前源码新增的模板 |
| --- | --- | --- |
| 正方形 | — | 256×256、512×512 |
| 竖屏 9:16 | 180×320、360×640 | 270×480、540×960 |
| 横屏 16:9 | 320×180、640×360 | 480×270、960×540 |

采用当前格式的图库可以直接使用。此前使用通用 CUDA 的尺寸切换到模板评分后，选图可能变化。预检发现 CUDA 不可用时，任务使用 CPU 并记录回退原因。

## 安装与构建

源码构建使用 CMake 4.2+、PowerShell 7 和 Git。Windows 使用 Visual Studio 2026 的 C++ 工具。Linux 和 macOS 使用 Ninja 与系统 C++ 编译器，Linux 还需要 `pkg-config`、`zip`、`unzip` 等基础工具。

### 安装依赖

在 PowerShell 7 中执行。已有对应提交的 vcpkg 时，设置 `VCPKG_ROOT` 后即可依次运行 `Prepare` 和 `Install`。

```powershell
git clone https://github.com/CialloKing/Mosaicraft.git
cd Mosaicraft
git clone https://github.com/microsoft/vcpkg.git build/tools/vcpkg
git -C build/tools/vcpkg checkout a0400024711b283056538ac19ced80b91a83c24c
$env:VCPKG_ROOT = (Resolve-Path build/tools/vcpkg).Path
./scripts/setup-dependencies.ps1 -Mode Prepare
./scripts/setup-dependencies.ps1 -Mode Install
```

### 编译

| 平台 | CPU Preset | CUDA Preset |
| --- | --- | --- |
| Windows x64 | `windows-cpu` | `windows-cuda` |
| Linux x64 | `linux-cpu` | `linux-cuda` |
| macOS ARM64 | `macos-arm64-cpu` | — |

把 `windows-cpu` 替换为对应 Preset。测试目标显式列出，程序位于 `build/<preset>/Release/bin`。

```powershell
$preset = 'windows-cpu'
cmake --preset $preset
cmake --build --preset $preset --target mosaicraft MosaicraftWebUI mosaicraft_tests mosaicraft_regression_tests --parallel
ctest --preset $preset
cmake --build --preset $preset --target mosaicraft_webui_smoke
```

使用 CUDA Preset 前，设置 `CUDAToolkit_ROOT` 指向 CUDA Toolkit 13.5.0（nvcc 13.5.37）。Windows 示例为 `$env:CUDAToolkit_ROOT = 'C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v13.5'`。要求 CTest 在实卡上运行时，先设置 `$env:MOSAICRAFT_REQUIRE_CUDA = '1'`。 [CUDA 工具链](docs/CUDA_13_5_MIGRATION.md) · [架构与单卡构建](docs/CUDA_ARCHITECTURES.md)

## 开发文档

- 接口文档列出请求字段、异步任务和诊断信息。运行 Web UI 后可通过 `GET /api/info` 查看版本和能力。 [REST API](docs/API.md)
- 匹配流程使用 AvgLAB、8×8 Grid、TinyImage、边缘密度和 LBP，结合候选召回、评分与邻域去重。 [项目百科](https://github.com/CialloKing/Mosaicraft/blob/main/docs/ENCYCLOPEDIA.md)
- 固定样本的预览耗时、流式 PNG 导出和 CPU 建库结果。 [v1.15.2 性能对照](docs/PREVIEW_VERIFICATION.md#v1152-预览性能对照)
- 原生包构建、GPU 验收和 SHA256 校验流程。 [CI 与打包](docs/CI_PACKAGING.md) · [发布清单](docs/RELEASE_CHECKLIST.md)

## Star 历史

GitHub Actions 每天北京时间 03:17 将 Star 数量更新到独立的 `star-history` 分支。

<p align="center">
  <a href="https://github.com/CialloKing/Mosaicraft/blob/star-history/stars.csv">
    <img src="https://raw.githubusercontent.com/CialloKing/Mosaicraft/refs/heads/star-history/star-history.svg" alt="Mosaicraft Star 数量历史图" width="960">
  </a>
</p>

首次运行按当前仍有效的 Star 时间重建历史，后续记录每日实测值。漏跑日期在实测记录中保留为空缺。 [查看 CSV 原始数据](https://github.com/CialloKing/Mosaicraft/blob/star-history/stars.csv)

## 许可证

本项目采用 GNU 通用公共许可证第 3 版（`GPL-3.0-only`）。 [许可证全文](LICENSE)
