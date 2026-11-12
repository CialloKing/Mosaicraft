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
