# HTML 最大缩放与可视窗口加载检查

日期：2026-11-12。实现提交：`697808da6`。

## 本次修改

- 实时预览和 HTML 查看器共用 `MosaicraftViewport.maxTileHeight`（256 CSS 像素）、`clip`、`visible` 和 `pixelBands`，不再各自维护缩放上限和裁剪算法。
- HTML 打开时以最大缩放居中显示，只对浏览器窗口内有实际显示像素的格子加载和绘制素材。
- 拖动或缩放后，取消已移出可视窗口的图片加载请求；可视窗口更新前不再派发上一帧队列中的请求。
- 页面隐藏、画布移出窗口时暂停加载，恢复可见时重新计算窗口内的可见范围。
- 已加载素材继续缓存，不恢复缓存大小上限；只有用户主动缩小或点击“适应窗口”才会显示并加载更大范围。

## 检查范围和产物

构建 Windows CUDA CLI/Web UI，检查改动 JS 的语法，并使用上一份无叠加工程的既有排列，通过 `render --mode html --target-blend 0` 生成新的验证包。没有重新匹配或增加图库使用统计，没有跑完整 CTest、Web UI smoke、ZIP 或原生图片回导流程。

输出目录：`output/html-viewport-20261112-193029/便携工程/`。保留 6,388 份唯一素材、65,330 格，排列和素材表与输入工程一致，仍不生成 overview 或缩略图。

## 浏览器实测

使用 `tests/browser_project.cjs` 的 `MOSAICRAFT_BROWSER_VIEWPORT_ONLY=1` 分支，在 1000×760 窗口离线检查：

| 项目 | Chromium 151.0.7922.34 | Firefox 153.0 |
| --- | --- | --- |
| 默认缩放 | 最大缩放 | 最大缩放 |
| 首屏可见格数 | 24 | 24 |
| 首屏素材请求数 | 23 | 23 |
| 首屏应用位图缓存 | 5.1 MiB | 5.1 MiB |
| 一次拖动后累计请求数 | 35 | 35 |
| 画布完全移出窗口后 | 请求数不增加 | 请求数不增加 |
| 窗口外素材绘制调用 | 0 | 0 |
| 脚本/图片加载错误 | 0 | 0 |

首屏不再请求全部 6,388 份素材。测试仅验证当前可视窗口所需的局部加载，主动适应窗口后仍允许展示全景。

机器结果保存在同级 `viewport-chromium.json`、`viewport-firefox.json`、`render-result.json`；日志为 `build/html-viewport-build.log`、`build/html-viewport-render.log`、`build/html-viewport-browser.log`。未推送或发布。

## 初始缩放与 4096 上限拆分（2026-11-12）

后续调整将共用参数拆为 `initialTileHeight = 256` 和 `maxTileHeight = 4096`，同时用于实时预览和 HTML 查看器。打开时保留图块高 256 CSS 像素的尺度，继续放大可达 4096；适应窗口所需的最小比例更大时仍沿用该比例。上文“默认最大缩放”及对应结果为调整前的历史记录。

- Windows CUDA 的 CLI、Web UI 增量构建通过；此次没有改动 CUDA 内核，也未重新进行 GPU 验收。
- `ctest --test-dir build/windows-cuda -C Release -R '^portable_project$' --output-on-failure` 通过。复用小型合成图片创建新的工程，`tests/browser_project.cjs` 在 Chromium 151.0.7922.34、Firefox 153.0 上通过离线浏览、百万格局部加载及缩放边界检查：初始 256，最大 4096，继续放大保持上限，缩小恢复至 2560。
- 指定的 `output/html-Nahaki-pid100639369-20261112-194725/便携工程` 已更新 HTML，并同步清单中的 HTML 大小与 SHA-256；内嵌快照、排列、素材和目标图未改。该工程 6239 个资源的深度完整性检查通过。
- 两种浏览器分别以 1100×760 和 375×812 窗口检查该实际工程，初始 256、最大 4096、缩小与适应窗口均通过，仓库链接保留，无脚本错误或横向溢出。

命令结果、日志、截图及更新前的 HTML/清单备份保存在忽略目录 `build/zoom-4096-20261112/`。此次未修改已发布的 v1.15.5 标签或附件。

## 最大缩放提高到 16384（2026-11-12）

在上述拆分基础上，仅将共用 `maxTileHeight` 提高到 16384 CSS 像素，`initialTileHeight` 保持 256。上节 4096 的结果保留为历史记录。

- Windows CUDA CLI、Web UI 增量构建通过。
- `ctest --test-dir build/windows-cuda -C Release -R '^portable_project$' --output-on-failure` 通过（1/1）。复用合成工程重新导出 HTML 后，`node tests/browser_project.cjs build/zoom-16384-20261112/artwork` 在 Chromium 151.0.7922.34、Firefox 153.0 上通过离线浏览、百万格局部加载和缩放边界检查：初始 256，最大 16384，再放大保持上限，缩小一次为 10240。
- 指定的 `output/html-Nahaki-pid100639369-20261112-194725/便携工程` 已同步 HTML 及清单中的 HTML 大小、SHA-256；内嵌快照、排列、素材和目标图未改。`project check --deep --json` 完整性检查通过（6239 个资源）。
- 两种浏览器分别在 1100×760 和 375×812 窗口实测该工程，初始 256、最大 16384、缩小及适应窗口均通过，仓库链接保留，无脚本错误或横向溢出。

实测结果、日志、截图和更新前备份保存在忽略目录 `build/zoom-16384-20261112/`。此处记录功能验证；发布包及 GPU 验收另外执行，以最终附件的 `BUILD_INFO.txt` 和发布审计为准。
