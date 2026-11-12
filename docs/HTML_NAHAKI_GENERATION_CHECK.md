# 指定目标图 HTML 工程生成测试

日期：2026-11-12。使用包含最大缩放和可视窗口加载优化的当前 Windows CUDA 构建，执行一次 create 及 Chromium/Firefox 简短离线浏览检查，没有运行完整回归、ZIP 或原生图片回导流程。

## 输入与参数

- 目标图：`test_input_photos/Nahaki_pid100639369_スメールp0.png`，2266×3776，RGBA PNG。
- 现有图库：`library/180x320`，45,613 张，素材规格 180×320；未重建图库。
- 参数：`--mode html --seed 20261112 --backend cuda --threads 8 --target-blend 0`，其余沿用默认值。
- 原目标图生成前后 SHA-256 一致：`57b6ea889bc7eb408d4631eff2162ab0a7a47961c208acc949fa3ab1252be021`。

## 生成结果

| 项目 | 结果 |
| --- | --- |
| 生成状态 | 成功，CUDA 特征与评分后端，无回退 |
| 工作目标 | 2268×3776，按既有规则补边 |
| 网格 | 252×236，共 59,472 格，无失败格 |
| 唯一素材 | 6,237 份，保留原图库文件名 |
| 工程文件 | 6,240 个，仅清单、HTML、无损目标及唯一素材 |
| 工程大小 | 304,962,514 字节，约 290.83 MiB |
| create 流程计时 | 69.418 秒；包含后续简短文件检查约 76.342 秒 |
| 原图叠加 | 0，未叠加 |
| 默认成品尺寸记录 | 45360×75520；本轮未生成该尺寸的单张图片 |

清单与内嵌浏览快照一致，均无工程版本字段，默认最大缩放。所有列出资源存在且大小与清单一致，素材文件名均来自图库；抽查首、中、末三份素材的文件字节 SHA-256，与清单及图库源文件一致。没有生成 overview 或缩略图。成功 create 正常记录本次图库使用统计。

## 首屏与拖动检查

1000×760 窗口离线打开，使用 `MOSAICRAFT_BROWSER_VIEWPORT_ONLY=1`：

- Chromium 151.0.7922.34、Firefox 153.0 均默认最大缩放，首屏可见 32 格，只请求 32 张素材，没有加载整库。
- 状态采集时应用位图缓存为 Chromium 5.5 MiB（当时完成 25 张）、Firefox 7.0 MiB（完成 32 张）；这是采集时状态，不是缓存上限。
- 一次拖动后累计请求数均为 38，画布移出窗口后请求数不再增加。
- 窗口外素材绘制调用为 0，脚本及图片加载错误均为 0。

## 产物

目录：`output/html-Nahaki-pid100639369-20261112-193528/`。

入口为 `便携工程/index.html`；记录包含 `create.json`、`create.log`、`generation-result.json`、`viewport-chromium.json`、`viewport-firefox.json` 及首屏截图。过程日志为 `build/html-nahaki-generation.log`、`build/html-nahaki-browser.log`。

本轮未修改程序代码，未推送或发布。
