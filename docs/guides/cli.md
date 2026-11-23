# 命令行指南

> 本指南对应当前源码。已安装版本的可用参数以程序 --help-all 和包内 `BUILD_INFO.txt` 为准；源码开发能力不等于稳定包已发布。

在解压目录运行以下命令。`photos` 是素材目录，`target.jpg` 是目标图。示例生成 `result.png` 和便于查看的 `result_preview.jpg`。Windows PowerShell 也可运行 `./mosaicraft.exe`。

```sh
./mosaicraft library build photos -o library --size 180x320 -r
./mosaicraft create target.jpg -l library -o result.png --output-tile 18x32 --thumbnail
```

CLI 已重新组织，不保留旧命令别名。每个命令支持 `--help`，全部选项用 `--help-all`。
`--json` 使 stdout 只输出一个 JSON 结果，进度和诊断写入 stderr；`--quiet` 隐藏普通进度，保留结果和错误。

## 性能诊断

```sh
./mosaicraft create target.jpg -l library -o result.png --benchmark --json
./mosaicraft render project.json -o result.png --write-mode stream --benchmark --json
./mosaicraft library add new_photos -l private_library --benchmark --json
```

`--benchmark` 默认关闭。`create` 与 `render` 使用相同的图片写出指标名；`library add` 额外报告编码、去重与提交，`library build` 和 `library normalize` 不接受该开关。JSON 指标位于 `runtime.metrics`；结构化错误仍应看 `diagnostic`。线程槽位和 AVX 分派的含义、耗时边界与查重占比的算法见[性能指标说明](../reference/benchmark-metrics.md)。正式比较整条命令的性能时请关闭诊断，用外部计时。

## 建库与图库管理

```sh
./mosaicraft library build photos -o library/270x480 --size 270x480 -r
./mosaicraft library add new_photos -l library/270x480 -r
./mosaicraft library report library/270x480 --check
./mosaicraft library report library/270x480 --deep
./mosaicraft library usage library/270x480 --sort tiles --limit 0 --unused
./mosaicraft library export library/270x480 -o used_photos --selection used
./mosaicraft library purge library/270x480
./mosaicraft library purge library/270x480 --apply
./mosaicraft library normalize photos -o normalized --size 180x320
```

仅归一化支持 `--crop center|smart-real|smart-anime`。例如：`mosaicraft library normalize photos -o normalized --size 180x320 --crop smart-real`。两种智能模式都使用 U²-NetP 保留主体，并分别使用原图尺寸检测真人或动漫脸，不限制最长边。智能模式每批逐张推理，大图可能明显增加耗时和内存。按目录手动分批选择，不自动分类；中心裁剪仍是默认。保持原图比例、输出尺寸和同尺寸快路径；固定比例容不下主体时仍可能裁掉部分内容。智能模式需要支持该功能的构建及本地模型，缺失时会报错。可用 `--crop-device auto|cpu|cuda` 选择智能裁剪推理设备。`auto`（默认）优先 CUDA，遇到可捕获的 GPU 故障后用 CPU 继续并探测恢复，恢复后从后续图片切回；`cpu` 不探测 CUDA；`cuda` 不可用时明确失败。自动切换不改变人脸输入尺寸。它与生成马赛克的 `--device` 无关，中心裁剪、建库和追加不接受此选项。CUDA 需要独立启用的构建及兼容 NVIDIA 设备。自动策略不覆盖随包 DLL 缺失造成的启动失败或进程崩溃。

建库归一化尺寸默认 180×320，每轴 1～8192。`library build` 和 `library add` 保留 JPG/JPEG、PNG、WebP 的格式及后缀拼写，将 BMP 和 TIFF/TIF 转为 PNG。同尺寸的 JPG/JPEG、PNG、WebP 通常直接复制原字节。若 PNG 含超过解码器上限的 `iTXt` 文本块，建库、追加或单独归一化时会先在内存中的副本移除这些块；移除文本不改变像素，原图不改动；此类素材需要使用独立输出目录。原地建库要求新素材已经符合目标尺寸及格式，需要转换或缩放时请使用独立输出目录。历史图库素材继续可读。

下列参数用于建库和追加时的重新编码。压缩和质量设置不影响直接复制的素材。

| 参数 | 取值和默认值 |
| --- | --- |
| `--jpg-quality` | `1–100`，默认 `100` |
| `--webp-quality` | 省略时无损，指定 `1–100` 时有损 |
| `--png-compression` | `0–9`，默认 `9`，`0` 为不压缩 |

独立 `library normalize` 使用原有编码策略，包括在输出更小时采用同尺寸 PNG 的无损重压结果。成品 `create` / `render` 的 `--png-compression` 范围为 `1–9`，默认 `6`。

`build` 只创建新图库，`add` 继承已有规格，保持素材 ID 和统计，并沿用颜色排序。改变规格请新建目录。`normalize` 只复制和归一化，不创建数据库，输入输出目录不得重叠。`--threads 0`（默认）与建库、追加共用归一化线程策略，按系统报告的逻辑 CPU 数并行处理图片，不再限制为 8 个线程；可用 `--threads N` 限制线程数。

`report` 默认快速统计，`--check` 检查资源和特征，`--deep` 再解码并核对像素摘要。LAB 亮度使用 OpenCV 8 位口径，分段为 50/150。覆盖不足和使用不均属于提示；缺失、不可读和损坏属于完整性失败。`purge --apply` 仅删除确认缺失的记录，权限错误和未使用状态不属于自动清理依据。

遇到 `unfinished color reorder` 时，保留重排日志和暂存目录，运行 `library add <素材目录> -l <图库>`。追加持有独占锁并先恢复；无新素材时可传入一个已有的空目录。恢复前 `purge` 不预览或删除记录；用旧版本程序时请勿执行 `purge --apply`。

成功 `create` 自动记录最终排列的使用情况，无需 `--analyze`。`usage --sort targets` 按每张素材使用过的独立目标数排名，同尺寸且解码像素完全相同的目标只计一次；`--sort tiles` 按累计格数。升级后独立目标数从零开始精确计数，旧统计保留在库中，格数继续累加。浏览、检查、打包和工程回导不会增加图库统计。

## 尺寸与输出

| 参数 | 行为 |
| --- | --- |
| `--match-tile WxH` | 目标每格覆盖像素，默认 9×16。 |
| `--output-tile WxH` | 输出每格像素，默认 180×320；横向图库默认取 320×180。放大仅重采样包内素材。 |
| `--target-size WxH 或 P%` | 匹配前缩放一次。像素写法允许单轴 `auto`，双轴指定可改变比例；百分比按原图等比计算。不指定则用原尺寸。 |
| `--mode image\|html\|deepzoom\|tiles` | 决定单图或完整目录结构。已知图片后缀可推导 image，其他路径必须指定模式。 |
| `--format jpg\|png\|webp\|tiff` | 图片编码；HTML 与编码参数互斥，Deep Zoom/分块固定使用 JPEG。 |
| `--target-blend 0..1` | 目标叠加，默认 0。 |
| `--brightness-jitter 0..0.5` | LAB 亮度扰动，默认 0；HTML 必须为 0。 |
| `--seed N` | 无符号 32 位种子；未指定时生成并记录。 |
| `--backend auto\|cpu\|cuda\|vulkan\|metal` | 建库和匹配后端；显式 GPU 请求不可用时失败；自动优先级由[发布包](gpu-backends.md#选择后端与设备)决定。 |
| `--device auto\|<设备标识>` | 建库、追加和生成使用同一 GPU；运行 `mosaicraft devices --json` 查看实际设备与不可用原因。 |
| `--score-mode fast\|compatible` | 默认 fast：CPU、CUDA、Vulkan、Metal 以 FP32 计算热路径，历史与邻域在主机合成。`compatible` 显式恢复各后端原有计算路径。 |
| `--threads N` | CPU 工作上限，0 自动，各阶段可进一步收缩。 |

```sh
./mosaicraft create target.jpg -l library -o result.png --target-size 1200xauto --write-mode stream --thumbnail
./mosaicraft create target.jpg -l library -o smaller.png --target-size '50%'
./mosaicraft inspect target.jpg -l library --target-size '200%'
./mosaicraft create target.jpg -l library -o blended.png --target-blend 0.2 --analyze
./mosaicraft create target.jpg -l library -o zoom-view --mode deepzoom
./mosaicraft create target.jpg -l library -o tiles --mode tiles
```

百分比必须为正数，最多三位小数；宽高分别四舍五入，最小 1 像素。原图及切块前工作图单轴不得超过 100 万像素、总面积不得超过 1 亿像素。`--target-size` 改变匹配网格；只想改变成品分辨率、保留格数时，请调 `--output-tile`。

旧 `--target-upscale` 已移除。以前只用 `--target-upscale 2` 的命令可改为 `--target-size 200%` 以获得同样的工作图尺寸，但缩放后的像素或选图可能不同。如果旧命令还指定了中间尺寸，请改用最终工作图的像素尺寸，或重新选择百分比；不再模拟先缩放再放大的效果。

单图 `--format` 优先于后缀；JPG/WebP 超限时继续使用既有策略：推导格式转等尺寸 PNG，显式格式缩小输出图块。JPEG/WebP `--quality` 默认 100，PNG `--png-compression` 默认 6、范围 1～9。`--overwrite` 允许替换已有文件；目录只允许覆盖可识别且没有额外文件的 Mosaicraft 输出。工程与 ZIP 先暂存后发布，失败保留旧输出。

成品 PNG 的压缩等级 1～9 都支持多线程，适用于 `create` / `render` 的流式和整图写出。`--threads 0` 默认以进程可用的逻辑 CPU 数为上限，优先分配尽可能多的压缩工作线程，再按行数和缓冲预算确定分块大小。显式 `--threads N` 设上限；没有足够行数、数据量或内存支撑两个压缩块时使用单线程，`--threads 1` 可强制单线程。并行分块不改变解码像素，但文件字节和大小可能与单线程不同。

`--random-top` 默认 10。`--reuse-penalty` 默认 0（关闭），匹配不读取图库的历史使用计数；设为正数才启用历史复用惩罚。邻域惩罚与生成后的使用统计不受影响。`--candidates`、`--l-range`、邻域与权重参数仍可用。`inspect <target> -l library` 分别提供整图特征和按正式预处理计算的网格覆盖。

完整选项以 `cli-options.json` 生成的 --help-all 为准。设备与精度见[GPU 后端](gpu-backends.md)，离线 HTML 和回导见[便携工程](portable-projects.md)，图库复制见[搬移图库](portable-libraries.md)。
