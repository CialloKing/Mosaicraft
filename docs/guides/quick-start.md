# 快速开始

以下步骤适用于 v1.15.19 的发布包。使用旧版本时可查看对应的附件及包内 `BUILD_INFO.txt`。[版本历史](../releases.md)

## 下载与运行

在 Release 的 **Assets** 中按下表下载并解压。智能裁剪包提供 CLI 和 Web UI。Qt 包自带桌面界面及其 CLI。下载后可用同一 Release 的 `SHA256SUMS.txt` 核对文件。[下载稳定版](https://github.com/CialloKing/Mosaicraft/releases/latest)

| 平台 | 智能裁剪包后缀 | Qt 包后缀 |
| --- | --- | --- |
| Windows x64 | `_windows-x64_cuda-vulkan-smart-crop.zip` | `_windows-x64_qt_cuda-vulkan.zip` |
| Linux x64 | `_linux-x64_cuda-vulkan-smart-crop.tar.gz` | `_linux-x64_qt_cuda-vulkan.tar.gz` |
| macOS ARM64 | `_macos-arm64_cpu-only-smart-crop.tar.gz` | `_macos-arm64_qt.tar.gz` |

Windows 支持基线为 Windows 10 22H2。Linux 的 CLI / Web UI 包以 Ubuntu 22.04（glibc 2.35）为基线，Qt 包以 Ubuntu 24.04 为基线。macOS 原生包以 macOS 13 ARM64 为基线，Qt 客户端的构建要求见[桌面包说明](../development/packaging.md#qt-桌面发布包)。

Windows/Linux 的双后端包自动优先 CUDA，准备阶段无法使用时走 CPU。使用 Vulkan 时，在表单或 Qt 的「设置 → 通用设置」显式选择。GPU 计算与智能裁剪推理可独立设置。[后端与设备](gpu-backends.md)

## Qt 桌面

Windows 启动 `bin/MosaicraftDesktop.exe`，Linux 运行 `./bin/MosaicraftDesktop`，macOS 打开 `MosaicraftDesktop.app`。

1. 在设置的素材库页面选择素材目录和图库目录。可指定已有图库，也可从原始素材创建图库。
2. 打开目标图，点击「生成预览图」。点击成品中的任意子图查看候选并换图。
3. 切换到「导出」，设置输出路径、格式和图块尺寸，再点击「开始导出」。

## Qt 桌面预览的内存

无需设置缓存大小。在生成页把鼠标停在画布底部、缩放框旁的「成品图」「目标图」或对比说明上，可查看所有工程的已缓存贴图和当前自动预算（MiB）。普通工程按实际生成的不同贴图估算需求：多个格子复用同一张贴图只算一份；便携工程按金字塔分块估算。

桌面贴图与匹配程序的素材像素缓存合计最多使用物理内存的四分之一，且不超过 8 GiB，按需增长，不预分配。系统余量不足时自动收缩；后台标签页只保留少量热块，开始导出时桌面贴图缓存至多保留 64 MiB。匹配、总览和导出缓冲不计入这项预算，因此它不是整个程序的内存上限。缓存淘汰不改变选图和输出像素。

## 本地 Web UI

解压智能裁剪包后，Windows 启动 `MosaicraftWebUI.exe`，Linux/macOS 在解压目录运行 `./MosaicraftWebUI`。浏览器默认打开 [http://localhost:8080](http://localhost:8080)。

1. 在「建库」页选择素材目录与新图库目录，完成后点击「使用此图库生成」。
2. 在「生成马赛克」页选择目标图与输出路径，首次尝试可将输出 Tile 尺寸设为 18x32。
3. 可直接点击「生成马赛克」，也可先点「预览并选图」，调整选图后返回生成。
4. 在「任务与结果」查看输出、实际后端和阶段耗时。

详细说明见 [Web UI](web-ui.md)。

## 命令行

在解压目录运行以下命令。`photos` 是素材目录，`target.jpg` 是目标图。示例生成 `result.png` 和便于查看的 `result_preview.jpg`。Windows PowerShell 也可运行 `./mosaicraft.exe`。

```sh
./mosaicraft library build photos -o library --size 180x320 -r
./mosaicraft create target.jpg -l library -o result.png --output-tile 18x32 --thumbnail
```

如果只想先试智能裁剪，可输出一份归一化图片，之后再决定是否建库：

```sh
./mosaicraft library normalize photos -o normalized --size 180x320 --crop smart-real --crop-device auto
```

动漫图片使用 `--crop smart-anime`。每个命令支持 `--help`，完整选项用 `--help-all`。

后续操作见[命令行指南](cli.md)、[GPU 后端](gpu-backends.md)及[便携工程](portable-projects.md)。
