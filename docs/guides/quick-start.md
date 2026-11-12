# 快速开始

> 本指南对应当前源码。已安装版本的可用参数以程序 --help-all 和包内 `BUILD_INFO.txt` 为准；源码开发能力不等于稳定包已发布。

## 下载与运行

在 [GitHub Releases](https://github.com/CialloKing/Mosaicraft/releases/latest) 的 **Assets** 中，按平台和文件名后缀选择发布包。使用支持 CUDA 13.5 的 NVIDIA 驱动时可选 CUDA 包，其余情况选 CPU 包。

| 平台 | 系统基线 | CPU 包后缀 | CUDA 包后缀 |
| --- | --- | --- | --- |
| Windows x64 | Windows 10 22H2 | `_windows-x64_cpu-only.zip` | `_windows-x64_cuda.zip` |
| Linux x64 | Ubuntu 22.04 / glibc 2.35 | `_linux-x64_cpu-only.tar.gz` | `_linux-x64_cuda.tar.gz` |
| macOS ARM64 | macOS 13 | `_macos-arm64_cpu-only.tar.gz` | — |

包内 `BUILD_INFO.txt` 记录构建配置和 GPU 验收状态。Windows CUDA 正式包须通过本机 GPU 验收；Linux CUDA 包进行构建、CPU 回退测试和独立包验收，GPU 实机状态保留为 `not-run`。 [v1.15.6 发布说明与校验值](https://github.com/CialloKing/Mosaicraft/releases/tag/v1.15.6)

## 图形界面

解压后，Windows 双击 `MosaicraftWebUI.exe`；Linux/macOS 在解压目录运行 ./MosaicraftWebUI。浏览器默认打开 http://localhost:8080。

1. 在「建库」页选择素材目录与新图库目录，完成后点击「使用此图库」。
2. 在「生成马赛克」页选择目标图与输出路径，首次尝试可将输出 Tile 尺寸设为 18x32。
3. 可先开启预览查看选图与覆盖，再点击「运行」。
4. 在「任务与结果」查看输出、实际后端和阶段耗时。

详细说明见 [Web UI](web-ui.md)。

## 命令行

在解压目录运行以下命令。`photos` 是素材目录，`target.jpg` 是目标图。示例生成 `result.png` 和便于查看的 `result_preview.jpg`。Windows PowerShell 也可运行 `./mosaicraft.exe`。

```sh
./mosaicraft library build photos -o library --size 180x320 -r
./mosaicraft create target.jpg -l library -o result.png --output-tile 18x32 --thumbnail
```

CLI 已重新组织，不保留旧命令别名。每个命令支持 `--help`，全部选项用 `--help-all`。
`--json` 使 stdout 只输出一个 JSON 结果，进度和诊断写入 stderr；`--quiet` 隐藏普通进度，保留结果和错误。

后续操作见[命令行指南](cli.md)、[GPU 后端](gpu-backends.md)及[便携工程](portable-projects.md)。
