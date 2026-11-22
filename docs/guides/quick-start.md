# 快速开始

> 本指南对应当前源码。已安装版本的可用参数以程序 --help-all 和包内 `BUILD_INFO.txt` 为准；源码开发能力不等于稳定包已发布。

## 下载与运行

在 [GitHub Releases](https://github.com/CialloKing/Mosaicraft/releases/latest) 的 **Assets** 中，按平台和文件名后缀选择发布包。NVIDIA 用户可选 CUDA 包；装有兼容 Vulkan 驱动的 Windows/Linux 用户可选 Vulkan 包；只想使用 CPU 则选 CPU 包。

| 平台 | 系统基线 | CPU 包后缀 | CUDA 包后缀 | Vulkan 包后缀 |
| --- | --- | --- | --- | --- |
| Windows x64 | Windows 10 22H2 | `_windows-x64_cpu-only.zip` | `_windows-x64_cuda.zip` | `_windows-x64_vulkan.zip` |
| Linux x64 | Ubuntu 22.04 / glibc 2.35 | `_linux-x64_cpu-only.tar.gz` | `_linux-x64_cuda.tar.gz` | `_linux-x64_vulkan.tar.gz` |
| macOS ARM64 | macOS 13 | `_macos-arm64_cpu-only.tar.gz` | — | — |

需要 Qt 界面时，选择对应平台的 `_qt` CPU 包，或 Windows/Linux x64 的 `_qt_cuda-vulkan` 双后端包，无需另装 Qt。双后端包的「自动」依次尝试 CUDA、CPU；用 Vulkan 时请在设置的「默认后端」中显式选择。独立的原生 `_vulkan` 包默认优先 Vulkan，启动计算前不可用则回退 CPU。Metal 不随正式包发布。见[桌面包说明](../development/packaging.md#qt-桌面发布包)和[本版校验值](https://github.com/CialloKing/Mosaicraft/releases/tag/v1.15.14)。

## Qt 桌面预览的内存

无需设置缓存大小。在生成页把鼠标停在画布底部、缩放框旁的「成品图」「目标图」或对比说明上，可查看所有工程的已缓存贴图和当前自动预算（MiB）。普通工程按实际生成的不同贴图估算需求：多个格子复用同一张贴图只算一份；便携工程按金字塔分块估算。

桌面贴图与匹配程序的素材像素缓存合计最多使用物理内存的四分之一，且不超过 8 GiB，按需增长，不预分配。系统余量不足时自动收缩；后台标签页只保留少量热块，开始导出时桌面贴图缓存至多保留 64 MiB。匹配、总览和导出缓冲不计入这项预算，因此它不是整个程序的内存上限。缓存淘汰不改变选图和输出像素。

## 图形界面

解压后，Windows 双击 `MosaicraftWebUI.exe`；Linux/macOS 在解压目录运行 ./MosaicraftWebUI。浏览器默认打开 http://localhost:8080。

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

CLI 已重新组织，不保留旧命令别名。每个命令支持 `--help`，全部选项用 `--help-all`。
`--json` 使 stdout 只输出一个 JSON 结果，进度和诊断写入 stderr；`--quiet` 隐藏普通进度，保留结果和错误。

后续操作见[命令行指南](cli.md)、[GPU 后端](gpu-backends.md)及[便携工程](portable-projects.md)。
