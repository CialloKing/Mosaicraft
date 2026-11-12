# 命令行指南

> 本指南对应当前源码。已安装版本的可用参数以程序 --help-all 和包内 `BUILD_INFO.txt` 为准；源码开发能力不等于稳定包已发布。

在解压目录运行以下命令。`photos` 是素材目录，`target.jpg` 是目标图。示例生成 `result.png` 和便于查看的 `result_preview.jpg`。Windows PowerShell 也可运行 `./mosaicraft.exe`。

```sh
./mosaicraft library build photos -o library --size 180x320 -r
./mosaicraft create target.jpg -l library -o result.png --output-tile 18x32 --thumbnail
```

CLI 已重新组织，不保留旧命令别名。每个命令支持 `--help`，全部选项用 `--help-all`。
`--json` 使 stdout 只输出一个 JSON 结果，进度和诊断写入 stderr；`--quiet` 隐藏普通进度，保留结果和错误。

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

建库归一化尺寸默认 180×320，每轴 1～8192；同尺寸素材保留原文件字节。`build` 只创建新图库，`add` 继承已有规格，保持素材 ID 和统计，并沿用颜色排序。改变规格请新建目录。`normalize` 只复制和归一化，不创建数据库，输入输出目录不得重叠。

`report` 默认快速统计，`--check` 检查资源和特征，`--deep` 再解码并核对像素摘要。LAB 亮度使用 OpenCV 8 位口径，分段为 50/150。覆盖不足和使用不均属于提示；缺失、不可读和损坏属于完整性失败。`purge --apply` 仅删除确认缺失的记录，权限错误和未使用状态不属于自动清理依据。

成功 `create` 自动记录最终排列的使用情况，无需 `--analyze`。`usage --sort targets` 按已记录独立目标数排名，`--sort tiles` 按累计格数；“未使用”表示未出现在已记录历史中。浏览、检查、打包和工程回导不会增加图库统计。

## 尺寸与输出

| 参数 | 行为 |
| --- | --- |
| `--match-tile WxH` | 目标每格覆盖像素，默认 9×16。 |
| `--output-tile WxH` | 输出每格像素，默认 180×320；横向图库默认取 320×180。放大仅重采样包内素材。 |
| `--target-size WxH` | 匹配前调整目标尺寸；允许单轴 `auto`，双轴指定可改变比例。 |
| `--target-upscale N` | 调整目标尺寸后再放大，随后按既有规则补边。 |
| `--mode image\|html\|deepzoom\|tiles` | 决定单图或完整目录结构。已知图片后缀可推导 image，其他路径必须指定模式。 |
| `--format jpg\|png\|webp\|tiff` | 图片编码；HTML 与编码参数互斥，Deep Zoom/分块固定使用 JPEG。 |
| `--target-blend 0..1` | 目标叠加，默认 0。 |
| `--brightness-jitter 0..0.5` | LAB 亮度扰动，默认 0；HTML 必须为 0。 |
| `--seed N` | 无符号 32 位种子；未指定时生成并记录。 |
| `--backend auto\|cpu\|cuda\|vulkan\|metal` | 建库和匹配后端；显式 GPU 请求不可用时失败；候选后端须显式启用。 |
| `--device auto\|<设备标识>` | 建库、追加和生成使用同一 GPU；运行 `mosaicraft devices --json` 查看实际设备与不可用原因。 |
| `--score-mode compatible\|fast` | 默认 compatible；Vulkan/Metal 使用 GPU 整数统计和 CPU FP64 评分。fast 使用 GPU FP32 距离及 CPU FP64 权重/惩罚。 |
| `--threads N` | CPU 工作上限，0 自动，各阶段可进一步收缩。 |

```sh
./mosaicraft create target.jpg -l library -o result.png --write-mode stream --thumbnail
./mosaicraft create target.jpg -l library -o blended.png --target-blend 0.2 --analyze
./mosaicraft create target.jpg -l library -o zoom-view --mode deepzoom
./mosaicraft create target.jpg -l library -o tiles --mode tiles
```

单图 `--format` 优先于后缀；JPG/WebP 超限时继续使用既有策略：推导格式转等尺寸 PNG，显式格式缩小输出图块。JPEG/WebP `--quality` 默认 100，PNG `--png-compression` 默认 1、范围 1～9。`--overwrite` 允许替换已有文件；目录只允许覆盖可识别且没有额外文件的 Mosaicraft 输出。工程与 ZIP 先暂存后发布，失败保留旧输出。

匹配策略与默认权重保持不变。`--random-top` 默认 10、`--reuse-penalty` 默认 0.01；`--candidates`、`--l-range`、邻域与权重参数仍可用。`inspect <target> -l library` 分别提供整图特征和按正式预处理计算的网格覆盖。

完整选项以 `cli-options.json` 生成的 --help-all 为准。设备与精度见[GPU 后端](gpu-backends.md)，离线 HTML 和回导见[便携工程](portable-projects.md)，图库复制见[搬移图库](portable-libraries.md)。
