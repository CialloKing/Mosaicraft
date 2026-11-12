# HTML 便携工程真实图库验收

> 历史归档：本文保留当时的命令、版本与测量结果，不作为当前操作指南。当前用法请从[文档中心](../../README.md)进入。


> 本文保留实施过程中的历史验收数据。当前工程已取消格式版本及旧总览/缩略图兼容逻辑，现行契约见 `CLI_PROJECTS.md`。

日期：2026-11-12。测试源码提交：`55ba1280b`，软件 1.15.4、API 1.4、工程格式 1。本轮实际使用项目中的现有图库和测试图片，没有重建图库。

## 输入与产物

| 项目 | 实际值 |
| --- | --- |
| 图库 | `library/180x320/mosaicraft.db`，45,613 张，180×320 |
| 目标 | `test_input_photos/target.png`，2500×3750，RGB |
| 后端 | RTX 4060 Laptop GPU，模板 CUDA 提取与评分，无回退 |
| 参数 | seed=20261112，threads=8，target-blend=0.2，brightness-jitter=0 |
| 匹配格 | 默认 9×16，补边后工作目标 2502×3760 |
| 最终网格 | 278×235，共 65,330 格，无空格、无匹配失败 |
| 默认输出图块 | 180×320，对应默认输出尺寸 50,040×75,200 |
| 唯一原素材副本 | 6,388 份，每份与原图库文件 SHA-256 一致 |
| 工程文件 | 19,168 个资源加 project.json，共 19,169 个文件 |
| 工程体积 | 356,878,263 字节，340.35 MiB |
| 完整 ZIP | 362,206,673 字节，345.43 MiB，19,169 个条目 |
| 导出端到端耗时 | 75.15 秒 |

产物保存在项目目录：

```text
output/html-portable-real-20261112-182215/
  便携工程/
    index.html
    project.json
    target.png
    overview.jpg
    assets/
    thumbnails/
  artwork.zip
  render-1.png
  render-4.png
  chromium-overview.png
  chromium-detail.png
  chromium-reference.png
  firefox-overview.png
  firefox-detail.png
  firefox-reference.png
  validation-results.json
  browser-results.json
```

工程先导出为 artwork，再移动到中文目录「便携工程」完成后续测试，入口和相对资源仍可使用。工程本身未加入测试报告或截图，完整 ZIP 只含清单声明的资源及清单，不包含同级测试文件。

```powershell
build/cli-project-install/cuda/mosaicraft.exe create test_input_photos/target.png `
  -l library/180x320 -o output/html-portable-real-20261112-182215/artwork `
  --mode html --seed 20261112 --backend cuda --threads 8 `
  --target-blend 0.2 --benchmark --resource-monitor --json
```

## 校验结果

- 移动目录后以 index.html 作为输入，`project check --deep` 通过：资源摘要、大小、解码尺寸、排列和浏览快照均有效。
- 对实际图库中符合文件大小条件的 12,468 个文件计算文件 SHA-256，找齐工程内全部 6,388 个原素材摘要，验证导出保存原文件字节。
- Chromium 151.0.7922.34 与 Firefox 153.0 均通过 file:// 离线查看；检查缩放、平移、目标参考、素材详情和只读边界。
- 初始全景使用总览，缓存 11.5 MiB；放大后切换可视窗口加载，采集时缓存分别为 37.5/40.4 MiB，低于 64 MiB。两个浏览器均实际加载原素材，未发生脚本或图片读取错误。
- 使用 CPU 安装版，从无图库目录的独立临时工作目录回导，输出 18×32 图块、5004×7520 PNG，完整解码通过。回导的 featureBackend/scoreBackend 均为 none。
- 1/4 线程回导 PNG 字节一致；ZIP 解压后的 deep check 通过，再回导的 PNG 与打包前字节一致。
- ZIP 条目集合与工程清单精确一致，CRC 校验通过，保留一个中文顶层工程目录；解压验证副本在验证后清理。
- 目标图 SHA-256 和图库素材数量不变。create 按正常规则记录 65,330 格使用；之后的浏览、检查、打包、回导没有增加使用统计。

本次回导验证使用缩小的 18×32 输出图块；50,040×75,200 是工程保存的默认成品尺寸，本轮没有生成该尺寸的完整单图。

最终回导 PNG SHA-256：

```text
bf132dcf67c85a72565787a7d3818536b584db4fcb48f3237985f15ed0c44dfe
```

## 资源与耗时

创建工程时采样进程 RSS 峰值约 358 MiB。设备总显存采样峰值约 2.08 GiB，包含其他程序占用，不能当作 Mosaicraft 单进程显存。

| 操作 | 本次观测耗时 |
| --- | ---: |
| 工程信息 | 3.45 s |
| 工程 deep check | 17.83 s |
| CPU 回导，1 线程 | 21.04 s |
| CPU 回导，4 线程 | 18.51 s |
| 打包 | 24.80 s |
| 解压后 deep check | 18.89 s |
| 解压后 CPU 回导 | 17.86 s |

这些是单次集成验证的墙钟时间，部分操作与浏览器检查并行，不作为线程加速比或性能基准。ZIP 采用 STORE，不重复压缩媒体，众多文件的 ZIP64 头部使归档略大于原工程。本例未触及 4 GiB 或 65,535 条目边界，边界覆盖见已有 CTest 计数流测试。

原始 create、check、render、pack JSON 和日志位于产物根目录；最终自动断言汇总为 `validation-results.json`、`browser-results.json`，控制台日志为 `build/real-html-final.log`、`build/real-html-browser.log`。实际产物和临时验证脚本位于 Git 忽略目录，此次只提交验收文档。
