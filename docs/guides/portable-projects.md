# CLI、便携工程与回导

本页对应当前源码版本 1.16.0，公开发布状态见[版本状态](../releases.md)。便携工程仅使用当前内部结构，不设置工程格式版本，不保留旧总览/缩略图工程的兼容读取；HTTP API 能力版本为 1.5、主版本保持 1。图库、特征、ANN 和去重规则保持不变。

## 使用流程

```sh
./mosaicraft create target.jpg -l library -o artwork --mode html --target-blend 0.2
./mosaicraft project info artwork
./mosaicraft project check artwork --deep
./mosaicraft render artwork -o result.tiff --output-tile 360x640
./mosaicraft render artwork/project.json -o result.png --target-blend 0.2 --brightness-jitter 0.04
./mosaicraft project pack artwork -o artwork.zip
```

HTML 输出是完整目录：`project.json`、`index.html`、保留原名的目标图和 `assets/<素材索引>/<原图库图片文件名>`。目标 JPG/JPEG、PNG 直接复制源文件，其他格式按原始分辨率转为 PNG；匹配所需的缩放、放大和补边单独记录，回导时重建。每份唯一素材只复制一次，保留原文件名与字节，不生成 overview、缩略图或兼容显示副本。查看器复用实时预览的缩放、裁剪和中心优先加载队列，初始图块高 256 CSS 像素并居中，最大可放大到 65536 CSS 像素，从窗口中心向四周加载当前窗口内素材；拖动后取消窗口外请求并释放其解码位图，页面不可见时也会释放。不设位图缓存大小上限，仅保留当前窗口所需的素材，并发为 8；主动缩小或点击“适应窗口”后可能加载更多素材。复制整个目录即可移动；传输时用 `project pack` 生成完整 ZIP64，解压后离线打开 `index.html`。浏览器支持缩放、平移、全屏、目标参考和素材查看，不提供编辑或原生图片导出。工程采用当前内部结构，不设置格式版本，不再支持旧总览/缩略图工程。

`render` 接受工程目录、其中的 `project.json` 或 `index.html`，不直接读取 ZIP。排列固定，不重新匹配、不打开原图库、不执行 CUDA 预检。可调整输出模式、编码、图块尺寸、叠加、亮度和外观种子；省略尺寸、外观和种子时继承工程。相同工程、参数及环境下重复输出具有相同解码像素；`render --seed` 只改变外观随机性。输出到新的 HTML 目录可以保存新的叠加效果。

Web UI 的「便携工程」页支持信息、检查、回导与打包，全部复用现有任务队列。HTML 下载需先打包；单独下载入口文件会被拒绝。Deep Zoom 继续保留。

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
  <原目标图主名>.jpg / .jpeg / .png（合法后缀大小写沿用输入）
  assets/0/<原图库图片文件名>
  assets/1/<原图库图片文件名>
  ...
```

`project.json` 是唯一原生渲染数据源。包含 type、grid、outputTile、seed、appearance、entry、target、targetPreparation、assets、layout 和 resources，不含 version 字段。`targetPreparation` 记录目标图的匹配缩放尺寸及放大倍数，补边由 grid 和 matchTile 推导；资源表记录实际保存图片的尺寸。旧工程缺少该字段时，目标图按已完成预处理和补边读取。`project info` 及 HTML 浏览快照也不输出工程格式版本。`layout` 使用包内素材表的零基索引；-1 表示匹配时无候选，保留背景。导出采用最终排列，包含手动替换。

原素材按文件字节 SHA-256 去重并完整复制，每份唯一内容只保留一个文件，保留其图库文件名（含大小写和扩展名）。不同内容可以同名，各自位于素材索引子目录，避免跨平台同名覆盖；相同字节有多个来源名称时采用最终排列首次出现的文件名。每个 resource 记录相对路径、文件字节数、SHA-256、图片尺寸；清单不包含自身摘要。

目标图保留输入文件主名，首次叠加为零也保存。`.jpg`/`.jpeg` 和 `.png` 来源直接复制原文件，文件字节、元数据和分辨率保持不变；其他来源按解码后的原始宽高转为 PNG，不缩放或补边。合法后缀及大小写沿用输入，例如 `旅行照片.JPEG` 仍为 `旅行照片.JPEG`，`旅行照片.webp` 转为 `旅行照片.png`。HTML 再导出继续复制已存目标资源。匹配时指定的目标尺寸、放大倍数及反射补边只影响工作图，不改变工程中保存的目标文件；原生回导复用原预处理算法，浏览器按记录的尺寸及反射边界映射目标参考与叠加，不额外保存整幅放大图。旧工程的 `target.png` 仍可读取及再导出，但未记录的原名无法自动恢复。没有 overview、缩略图、兼容显示副本、数据库、ANN 索引、特征文件、未使用素材或来源绝对路径。图库已归一化时保留的是图库图片名和内容，不是归一化前原照片的名字或细节。

HTML 内嵌从清单派生的只读浏览快照和全部 JS/CSS。查看时不 fetch 外部 JSON、不请求 CDN、不需要服务或模块加载。查看器与实时预览共用初始缩放（图块高 256 CSS 像素）、最大缩放（图块高 65536 CSS 像素）、可见区域裁剪、网格范围及物理像素边界计算。打开时居中并使用初始缩放，窗口容纳整个工程所需的最小比例更大时沿用该比例；初始尺度不随缩放上限提高而改变。只请求和绘制当前窗口内有实际显示像素的素材；拖动或缩放后取消移出可视窗口的加载请求，队列更新前不再派发上一帧的请求。页面隐藏或画布滚出窗口时暂停图片加载。画布按可视窗口尺寸创建，图片到达时只更新对应可见格。

HTML 与实时预览共用中心优先加载队列：按素材所覆盖格子中心到窗口中心的最小距离排序，从中心向四周扩散请求，同一素材只加载一次，并发上限为 8。请求完成的实际次序仍由浏览器解码速度决定。

HTML 界面使用“窗口浏览”。查看器不设置位图缓存大小上限，但只保留当前窗口需要的素材；窗口改变时取消离开的请求，并关闭对应 ImageBitmap、移除图片源引用。页面隐藏或画布滚出窗口时释放全部解码位图，返回时按需重载。图片对话框关闭（包括 Esc）也会清除图片引用。浏览器自己的文件缓存和系统内存回收不由应用控制。只有用户主动缩小或点击“适应窗口”时才展示更大范围，实时 Web UI 预览的缓存预算保持原策略。

查看器和原生回导共用 assets 中唯一的素材副本，目标参考和叠加直接使用清单 `target` 指定的 JPEG/PNG。浏览器不支持的原素材格式会显示资源读取失败，不另生成替代文件，仍可由支持该格式的原生解码器回导。透明度、位深和颜色解码遵从浏览器语义，不承诺与 OpenCV 逐像素一致。旧总览/缩略图结构的工程不再支持查询、检查、打包或回导，需要从目标图与图库重新生成。

## 校验、安全与发布

- `project info` 读取元数据，不完整解码资源。
- `project check` 验证当前结构、排列、索引、路径边界、资源表、大小、摘要和 HTML 快照；`--deep` 再解码并核对尺寸。
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
