# CLI、便携工程与回导

本次为源码更新，软件发布版本仍为 1.15.4。工程格式为 1，HTTP API 能力版本为 1.4、主版本保持 1；图库、特征、ANN 和去重规则保持不变。

## 命令迁移

旧 CLI 命令和选项不再作为别名解析。以下对照用于迁移已有脚本，HTTP API 的旧字段仍受支持。

| 旧命令或选项 | 新命令或选项 |
| --- | --- |
| `mosaic -i target -d library` | `create target -l library` |
| `build -i photos -o library` | `library build photos -o library` |
| `build --append` | `library add photos -l library` |
| `build --normalize-only` | `library normalize photos -o normalized` |
| `db-stats -d library` | `library report library` |
| `db-health -d library` | `library report library --check` |
| `db-usage -d library` | `library usage library` |
| `db-usage --export dir` | `library export library -o dir --selection used` |
| `db-purge -d library` | `library purge library`；真正删除加 `--apply` |
| `inspect -i target -d library` | `inspect target -l library` |
| `--tile-w W --tile-h H` | `--match-tile WxH` |
| `--out-w W --out-h H` | `--target-size WxH`，单轴可为 `auto` |
| `--upscale` / `--topn-random` / `--penalty` | `--target-upscale` / `--random-top` / `--reuse-penalty` |
| `--overlay-strength` | `--target-blend` |
| `--color-adjust --color-strength N` | `--brightness-jitter N`，默认 0 |
| `--normalize-size` / `--png-level` / `--preview` | `--size` / `--png-compression` / `--thumbnail` |
| `--cpu` / `--tiled` / `--deepzoom` | `--backend cpu` / `--mode tiles` / `--mode deepzoom` |
| `--yes` / 结束等待按键 | 无交互补参和等待；覆盖使用 `--overwrite` |

命令定义集中于 `cli-options.json`，CLI 帮助和 Web UI 参数映射均从该文件生成。不引入大型命令框架。数值解析、服务校验和输出编码继续复用原逻辑。

## 目录和数据契约

```text
artwork/
  project.json
  index.html
  target.png
  overview.jpg
  assets/<sha256>.<extension>
  thumbnails/<sha256>-32.jpg
  thumbnails/<sha256>-128.jpg
  thumbnails/<sha256>-display.jpg  # 必要时生成
  thumbnails/target.jpg
```

`project.json` 是唯一原生渲染数据源。包含 type/version、grid、outputTile、seed、appearance、entry、target、targetDisplay、overview、assets、layout 和 resources。`layout` 使用包内素材表的零基索引；-1 表示匹配时无候选，保留背景。导出采用最终排列，包含手动替换。

原素材按文件字节 SHA-256 去重并完整复制。每个 resource 记录相对路径、文件字节数、SHA-256、图片尺寸；清单不包含自身摘要。无损目标为正式匹配预处理和补边后的工作目标，首次叠加为零也保存。没有数据库、ANN 索引、特征文件、未使用素材或来源绝对路径。

HTML 内嵌从清单派生的只读浏览快照和全部 JS/CSS。查看时不 fetch 外部 JSON、不请求 CDN、不需要服务或模块加载。总览和视口素材使用分级显示；应用图片缓存上限 64 MiB、加载并发上限 8。浏览器渲染不承诺与 OpenCV 逐像素一致。

原文件始终保留。浏览器不兼容格式、透明度或位深显示语义不一致时生成受控显示副本。最长边 32/128 的缩略图只用于浏览，原生回导从 assets 读取；丢失派生资源会使工程完整性检查失败，但不会阻止原生图片渲染。

## 校验、安全与发布

- `project info` 读取元数据，不完整解码资源。
- `project check` 验证版本、排列、索引、路径边界、资源表、大小、摘要和 HTML 快照；`--deep` 再解码并核对尺寸。
- `render` 校验必需原素材和目标图。外观扰动由 seed 和格子位置确定，不依赖线程调度。
- `project pack` 只打包清单认可资源及清单本身，ZIP 内保留一个完整顶层目录。旁边无关文件不纳入包内。
- ZIP 默认 STORE，固定 miniz 3.1.2 流式写 ZIP64；PicoSHA2 计算摘要。来源和许可证在 third_party 中锁定，并随安装及发布包交付。
- 拒绝绝对资源路径、越界路径和工程外链接；读取 HTML 输入时只定位同目录清单，不执行脚本。
- 工程和归档先写同目录暂存产物，全部成功后发布；失败恢复旧输出。目录覆盖要求可识别的工程清单或输出所有权标记，并且不得有额外文件。
- 发布前按实际编码产物后缀再次保护目标图、图库和源工程，防止格式调整后覆盖输入。

回导不修改源工程，也不更新图库使用统计。不提供 ZIP 直接导入、unpack、project import 或独立 serve 命令；服务入口仍为 MosaicraftWebUI。

## 验证入口

```powershell
ctest --test-dir build/windows-cpu -C Release --output-on-failure
pwsh -NoProfile -File tests/webui_smoke.ps1 -WebUiExe build/windows-cpu/Release/bin/MosaicraftWebUI.exe
node tests/browser_project.cjs <已导出的合成工程目录> [http://localhost:8080]
```

浏览器检查使用可选的 Playwright Chromium/Firefox；常规 CTest 仅使用原有 doctest 和 Python 标准库。`portable_project` 使用运行时合成图片，覆盖移动、原图库不可用、PNG 确定性、损坏资源、路径越界、ZIP 解压回导和覆盖保护。ZIP64 计数流测试覆盖 65,536 项和超过 4 GiB 的偏移，不生成数 GiB 临时文件。

平台运行和 GPU 实机验证必须独立记录，不能以编译成功代替运行验收。
