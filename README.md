# Mosaicraft

Mosaicraft 用图库里的照片拼出一张目标图，提供本地 Web UI 和命令行，支持交互预览、逐格换图、超大图流式输出与离线 HTML 工程。

（本项目纯AI生成，绝无手写代码）

[下载稳定版](https://github.com/CialloKing/Mosaicraft/releases/latest) · [快速开始](docs/guides/quick-start.md) · [文档中心](docs/README.md) · [源码构建](docs/development/building.md)

## 下载与运行

在 [GitHub Releases](https://github.com/CialloKing/Mosaicraft/releases/latest) 的 **Assets** 中，按平台和文件名后缀选择发布包。使用支持 CUDA 13.5 的 NVIDIA 驱动时可选 CUDA 包，其余情况选 CPU 包。

| 平台 | 系统基线 | CPU 包后缀 | CUDA 包后缀 |
| --- | --- | --- | --- |
| Windows x64 | Windows 10 22H2 | `_windows-x64_cpu-only.zip` | `_windows-x64_cuda.zip` |
| Linux x64 | Ubuntu 22.04 / glibc 2.35 | `_linux-x64_cpu-only.tar.gz` | `_linux-x64_cuda.tar.gz` |
| macOS ARM64 | macOS 13 | `_macos-arm64_cpu-only.tar.gz` | — |

包内 `BUILD_INFO.txt` 记录实际构建配置与验收状态。v1.15.7 保持五个标准发布包，Vulkan 与 Metal 均不编译进入这些包。需要 Vulkan 的用户须[自行从源码启用并编译](docs/development/building.md#vulkan-与-metal)，不能仅靠运行时参数开启。auto 仍为 CUDA → CPU。详见[版本状态](docs/releases.md)与[GPU 后端](docs/guides/gpu-backends.md)。

## 快速开始

Windows 解压后双击 `MosaicraftWebUI.exe`；Linux/macOS 在解压目录运行 ./MosaicraftWebUI，浏览器默认打开 http://localhost:8080。

1. 在「建库」页导入素材，建立新图库。
2. 在「生成马赛克」页选择目标图、图库和输出路径。
3. 先用预览检查选图，再运行生成；任务详情显示实际后端与耗时。

命令行可在解压目录运行：

```sh
./mosaicraft library build photos -o library --size 180x320 -r
./mosaicraft create target.jpg -l library -o result.png --output-tile 18x32 --thumbnail
```

Windows PowerShell 使用 ./mosaicraft.exe。完整步骤见[快速开始](docs/guides/quick-start.md)。

## 文档导航

| 需要做什么 | 文档 |
| --- | --- |
| 操作界面、预览、复制多种 Shell 命令 | [Web UI 指南](docs/guides/web-ui.md) |
| 管理图库、调整尺寸、选择输出格式 | [命令行指南](docs/guides/cli.md) |
| 选择显卡、后端与精度模式 | [GPU 后端](docs/guides/gpu-backends.md) |
| 复制图库、离线浏览或回导工程 | [搬移图库](docs/guides/portable-libraries.md) · [便携工程](docs/guides/portable-projects.md) |
| 接入服务、构建、测试与打包 | [HTTP API](docs/reference/api.md) · [开发文档](docs/README.md#接口与开发) |
| 查找旧版验收、性能样本与研究 | [历史档案](docs/archive/README.md) · [原始数据](docs/performance/README.md) |

## Star 历史

<p align="center">
  <a href="https://github.com/CialloKing/Mosaicraft/blob/star-history/stars.csv">
    <img src="https://raw.githubusercontent.com/CialloKing/Mosaicraft/refs/heads/star-history/star-history.svg" alt="Mosaicraft Star 数量历史图" width="960">
  </a>
</p>

GitHub Actions 每天北京时间 03:17 记录 Star 数量。首批历史按采集时仍有效的 Star 时间重建，后续以每日实测为准。 [查看 CSV 原始数据](https://github.com/CialloKing/Mosaicraft/blob/star-history/stars.csv)

## 许可证

本项目采用 GNU 通用公共许可证第 3 版（GPL-3.0-only）。[许可证全文](LICENSE)
