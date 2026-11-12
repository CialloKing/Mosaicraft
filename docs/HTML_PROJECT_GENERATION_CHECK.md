# 无版本工程生成检查

日期：2026-11-12。实现提交：`7c4b31cec`。工程仅采用当前内部结构，不设置工程格式版本，不再保留旧总览/缩略图结构的读取兼容。软件和 API 版本没有变化。

## 本次检查范围

按要求只构建 Windows CUDA 的 CLI/Web UI，执行一次真实图库 HTML 生成，再做清单、工程信息和 Chromium 离线浏览的简短检查。没有运行 CTest、完整 Web UI smoke、Firefox、ZIP 打包或原生回导流程。

输入为现有 `library/180x320`（45,613 张）及 `test_input_photos/target.png`，没有重建图库。

```powershell
build/windows-cuda/Release/bin/mosaicraft.exe create test_input_photos/target.png `
  -l library/180x320 -o output/html-unversioned-no-blend-20261112-191734/便携工程 `
  --mode html --seed 20261112 --backend cuda --threads 8 --target-blend 0 --json
```

## 实际结果

- 一次生成成功，匹配后端为 CUDA；278×235，共 65,330 格，无失败格。
- 6,388 份唯一素材、6,391 个文件，保留图库文件名；工程大小 311,926,431 字节，约 297.48 MiB。
- 生成流程计时 65.533 秒；本次不作为严格性能对照。
- `project.json`、HTML 内嵌快照和 `project info` 结果均不含工程 `version` 字段。
- 清单与浏览快照的 `targetBlend` 均为 0，没有原图叠加；仍按工程契约保存无损 `target.png`，供以后原生回导或目标参考使用。
- 不生成 overview、缩略图或兼容显示副本。
- 含旧总览/缩略图结构的工程信息请求明确失败，并提示重新生成 HTML 工程。
- Chromium 151.0.7922.34 离线打开、缩放和素材查看成功，无脚本或图片加载错误。本次只检查局部浏览，不等待全部素材加载；过程中没有加载 `target.png`。

产物及记录位于 `output/html-unversioned-no-blend-20261112-191734/`：`便携工程/index.html`、`create.json`、`create.log`、`info.json`、`generation-result.json`、`browser-quick-result.json` 和 `preview-detail.png`。

成功 create 正常记录本次图库使用；查看和工程信息检查不累计使用统计。未推送、打标签或发布。
