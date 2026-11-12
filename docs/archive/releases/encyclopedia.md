# Mosaicraft 项目百科全书 / Project Encyclopedia

> 历史归档：本文保留当时的命令、版本与测量结果，不作为当前操作指南。当前用法请从[文档中心](../../README.md)进入。


> 最后更新 / Last updated：2026-11-13 | 版本 / Version：v1.16.0
>
> English readers: each major section begins with a brief English summary. The detailed technical reference, logs, and code examples are primarily in Chinese — the project's working language.

当前源码提供 API 1.5 和 HTML 便携工程及固定排列回导，支持实时预览、候选查询及手动替换。图库结构、特征、缓存及恢复格式沿用 v1.15.0 定义的版本 1。早于 v1.15.0 的图库需要在新目录重建，详见 [1.15.0 不兼容更新](breaking-1.15.0.md) 和 [API 文档](../../reference/api.md)。下文历史版本记录保留发布时的说明。

## 目录 / Table of Contents

1. [项目概述 / Overview](#1-项目概述--overview)
2. [架构设计 / Architecture](#2-架构设计--architecture)
3. [命令行完全参考 / CLI Reference](#3-命令行完全参考--cli-reference)
4. [性能数据 / Performance](#4-性能数据--performance)
5. [输出格式行为 / Output Formats](#5-输出格式行为--output-formats)
6. [Bug 历史与经验教训 / Bug History](#6-bug-历史与经验教训--bug-history)
7. [版本演进 / Version History](#7-版本演进--version-history)
8. [构建与发布 / Build & Release](#8-构建与发布--build--release)
9. [开发规范 / Coding Conventions](#9-开发规范--coding-conventions)
10. [已知限制与未来方向 / Limitations & Roadmap](#10-已知限制与未来方向--limitations--roadmap)
11. [更新日志 / Changelog](#11-更新日志--changelog)

---

## 1. 项目概述 / Overview

> **EN**: Mosaicraft is a GPU-accelerated photo mosaic generator. It indexes thousands of library images, divides the target image into tiles, matches each tile to the most similar library image via a two-stage ANN+GPU pipeline, and assembles the result at massive resolutions (up to 50,040×75,200 pixels, 376 megapixels).

Mosaicraft 是一个 GPU 加速的照片马赛克拼贴生成器。将数千张图库图片建立索引，目标图分割为小格后逐格匹配最相似图片，拼接输出超大分辨率马赛克。

CUDA 可用时，按图库归一化尺寸自动选择模板提取和评分。固定尺寸包括正方形 256×256、512×512，竖屏 180×320、270×480、360×640、540×960，以及横屏 320×180、480×270、640×360、960×540。其他合法尺寸（宽高各 1～8192）使用与原 CPU 精确兼容的通用 CUDA 特征统计和候选评分，保留 CPU 的邻域处理、排序、选图及分析。通用提取与评分独立准备资源，计算前资源不足只回退相应能力。1.15.0 的特征编号为 1，数学定义沿用原 v6，早于 1.15.0 的图库须重新建库。历史后端验证见 [动态尺寸特征验证](../gpu/dynamic-cuda-features.md)及 [独立 CUDA 评分验证](../gpu/independent-cuda-scoring.md)。

> **EN**: Fixed sizes retain template CUDA extraction and scoring. Other valid dimensions (1–8192 per axis) use CPU-compatible generic CUDA statistics and scoring. Resource preparation remains independent. Version 1.15.0 resets the feature version to 1 with the former v6 mathematics; old libraries must be rebuilt.

### 核心能力

```
图库 → 归一化180×320 → 8×8 Grid + Tiny + LBP + Edge + LAB → SQLite + FeaturePack
                            ↓
目标图 → 9×16 tile → 特征提取 → ANN粗筛200候选 → GPU五特征精排 → 邻域去重 → 输出
```

### 关键数字

- 图库规模：25,034 张（已验证），理论支持 10 万+
- 输出分辨率：最高 50040×75200（3.76 亿像素）
- 典型耗时：8,446 tiles / 33 秒（RTX 4060）
- 发布包：Windows ZIP、Linux/macOS ARM64 tar.gz，附带运行库及 Web UI，解压即用

---

## 2. 架构设计 / Architecture

> **EN**: Two-stage retrieval: ANN (hnswlib) narrows 25K images → 150 candidates, then GPU kernel scores all five features (LAB, 8×8 Grid, TinyImage, Edge, LBP) to pick the winner. Spatial weight map and neighbor deduplication resolve visual quality. Storage uses SQLite + FeaturePack binary cache.

### 2.1 双层检索

```
Tile 特征 (五特征共708维，粗筛196维)
     ↓
 ANN (hnswlib)     ← LAB 3 + Grid 192 + Edge 1 (196维)
     ↓
 Top 150 候选（默认）
     ↓
 GPU kernel        ← 五特征全量评分 (合计708维)
     ↓
 邻域去重 + Top-N随机
     ↓
 最终选择
```

设计理由：ANN 负责缩小搜索空间，CPU/GPU 精排负责五特征完整评分。v1.14.0 移除了 Tiny/LBP 的 512 维零值占位；两者仍参与精排，权重和邻域去重规则保持不变。

### 2.2 五特征匹配

| 特征 | 维度 | 权重 | 作用 | 实际贡献 |
|------|------|------|------|----------|
| AvgLAB | 3 | 0.20 | 全局颜色 | 13.2% |
| 8×8 Grid | 192 | 0.45 | 空间布局 (64格×LAB) | **69.2%** |
| TinyImage | 256 | 0.25 | 16×16 灰度缩略图 | — |
| 边缘密度 | 1 | 0.05 | Canny 边缘占比 | 17.5% |
| LBP 纹理 | 256 | 0.05 | 纹理直方图 | — |

Grid 实际贡献 69.2%远超权重 0.45，是核心区分特征。8×8 Grid 升级（v1.0）是项目最重要的质量提升。

### 2.3 存储架构

```
build 阶段产出:
normalized/
├── 000001.jpg          # 归一化图 (180×320)
├── ...
└── features/
    ├── tiny.bin         # N × 256B = 6.4MB (25K图库)
    ├── lbp.bin          # N × 1024B = 25.6MB
    ├── lib.ann196       # 196维 HNSW 索引（旧 lib.ann 保留）
    └── lib.ann196.meta  # 格式版本、维度、图片特征指纹及索引文件校验值

lib.db                   # SQLite (avgLAB, Grid BLOB, 路径, 元数据)
```

FeaturePack v2 二进制格式：每条记录显式存储 `int32_t image_id` + 特征数据，解决 ID 不连续问题。

### 2.4 处理流程

```
Prep
├─ SQLite / FeaturePack 加载
└─ GPU 图库分配、上传（benchmark 分别记录）

Features
├─ CPU 复用 LAB 和灰度转换结果
└─ GPU 任务内复用特征工作区；原特征算法保持不变

ANN
├─ 校验并加载 lib.ann196，失效时重建
├─ 196维查询；最多 min(硬件并发数, 8, 任务数) 个工作线程
└─ 索引只读、独立查询局部状态，结果按原小块顺序保存

GPU 评分
├─ 临时显存预算不超过256 MiB，结合可用显存缩小批次
├─ 分配不足时减半重试；单块仍失败则返回错误
└─ 五特征、权重及使用惩罚保持不变

Selection
├─ 邻域频率惩罚按原顺序执行
└─ Top-N 随机选取保持原默认值

Output
├─ 512 MiB LRU，共享只读图片，颜色调整才复制
├─ CPU/GPU 复用加载、缩放、贴图、编码流程
└─ PNG/JPG/TIFF 流式输出：固定线程池、最多两行小块缓冲
```

---

## 3. 命令行完全参考 / CLI Reference

当前命令树和全部迁移说明见 [CLI、便携工程与回导](../../guides/portable-projects.md)；常用参数使用 `<命令> --help`，完整参数使用 `--help-all`。旧 CLI 名称与选项不再解析，历史章节中的命令只反映当时版本。

```powershell
mosaicraft library build photos -o library --size 180x320
mosaicraft library add new-photos -l library
mosaicraft create target.jpg -l library -o result.png
mosaicraft create target.jpg -l library -o artwork --mode html
mosaicraft create target.jpg -l library -o zoom --mode deepzoom
mosaicraft render artwork -o rendered.png --output-tile 360x640
mosaicraft project check artwork --deep
mosaicraft project pack artwork -o artwork.zip
mosaicraft library report library --check
mosaicraft library usage library --sort tiles --limit 0
mosaicraft inspect target.jpg -l library
```

`--mode image|html|deepzoom|tiles` 决定结构，`--format` 决定编码。亮度扰动统一为 `--brightness-jitter 0..0.5`，默认 0；HTML 要求 0。基础图叠加为 `--target-blend 0..1`。匹配和输出图块分别为 `--match-tile` 和 `--output-tile`，目标尺寸为 `--target-size WxH`（允许单轴 auto），放大为 `--target-upscale`。默认匹配权重、ANN、归一化及输出尺寸未改。

HTML 包保存使用过的原素材副本、最终排列、无损工作目标和派生浏览资源。浏览器只查看；原生回导允许输出、编码和外观调整，不重新匹配、不依赖原图库。单图、分块和 Deep Zoom 继续支持亮度扰动；固定种子和格子位置决定外观随机性。

普通说明和进度走 stderr，`--json` 的 stdout 只有一个结构化结果。无结束按键等待，也不交互补充参数。HTTP API 保持旧字段兼容，新增能力见 [API 1.4](../../reference/api.md)。

---

## 4. 性能数据 / Performance

> **EN**: Benchmarks on 25,034-image library, RTX 4060, 180×320 tiles. Typical mosaic: 8,446 tiles in ~18s, 65K tiles in ~34s. See detailed breakdown below.

基于 25,034 张图库、RTX 4060、8×8 Grid、180×320 tile。

| 目标图 | 配置 | Tiles | 输出 | 耗时 |
|--------|------|-------|------|------|
| target2.jpg | 默认 | 8,446 | 9270×13120 JPG | ~18s |
| target6.jpg | 默认 | 15,142 | 15120×24000 JPG | ~22s |
| target3.jpg | 默认 | 20,355 | 31860×36800 JPG | ~17s |
| target3.jpg | `--upscale 2` | 81,420 | 31860×36800 JPG | ~50s |
| target.png | 默认 | 65,330 | 50040×75200 TIFF | ~34s |

### 分析报告示例 (target6.jpg, 15K tiles)

```
=== Match Quality Analysis ===
  Score: mean=0.1524 median=0.1429 p90=0.2404  (-7.4% vs 等权)
  Feature: LAB=13.2%  Grid=69.2%  Edge=17.5%
  Gap: -0.0035  (TopN=3 代价 2.1%)
  Rank: #1=33.6% #2=33.4% #3=33.0%
  ANN recall: Top1=7.2% Top5=25.1% Top20=54.7%  (30≈200)
  Grid cell: 中心7.8~8.0 → 底行19.3 (2.5x差异)
  Grid weights: {0.88,0.94,...,0.46} (auto-tuned)
  Reuse: 3277/15142 unique  ratio=4.62x
  Worst tiles: *_analysis/ (20 pairs)
  Heatmap: *_heatmap.png
```

### 图库诊断示例

```
=== Database Statistics ===
  Images: 25034  Grid: 8×8 (192 dim)
  LAB: L[41.9,246.8] avg=174  A[106.5,177.9] avg=135  B[81.2,178.8] avg=128
  Brightness: dark=0 mid=95 bright=24939 (99.6% 偏亮)
  Coverage gaps: dark(95) ← 仅50张暗图，需补夜景
```

### 已验证的核心发现

| 发现 | 数据 | 行动 |
|------|------|------|
| Grid 69% 贡献，中心 2.5× 底行 | → Spatial Weight Map | Score -7.4% |
| candidates 30≈150 | → 默认保持 150（最佳平衡点） | — |
| Top10 Random | → 保留默认 | — |
| 图库暗光 0.2% | → `db-stats` 诊断 | 补夜景图 |
| Adaptive Weights 退步 | → 冻结为实验选项 | — |
| Edge 密度在 tile 上恒为 0 | → 9×16→180×320 上采样抹平边缘 | 已知限制 |

---

## 5. 输出格式行为 / Output Formats

> **EN**: Explicit format overrides the output suffix, then input suffix, then PNG fallback. Inferred oversized JPG/WebP becomes full-size PNG; explicit JPG/WebP reduces output tiles. PNG/TIFF preserve dimensions within existing integer and resource limits.

| 格式 | 限制 | 超限行为 |
|------|------|----------|
| JPG | 65500px | 推导格式时转 PNG；显式选择时缩小输出 Tile |
| TIFF | 单轴 `INT_MAX` (BigTIFF) | 报错 |
| WebP | **16383px** | 推导格式时转 PNG；显式选择时缩小输出 Tile |
| PNG | 单轴 `INT_MAX` | 报错 |

格式优先级为 `--format` / API `format` → 输出文件后缀 → 输入文件后缀 → PNG。默认文件名为 `output/output`，最终追加实际后缀。有效的显式格式覆盖冲突后缀；兼容的 `.jpeg/.tif` 和大小写保持原样。未知输出后缀在没有显式格式时失败，输入后缀无法推导时回退 PNG，但输入仍须能够解码。

超大输出默认允许，无需额外开关；PNG/TIFF 可使用流式写入降低内存占用。工作图像最多 1 亿像素。显式 JPG/WebP 通过统一比例缩小输出 Tile，整数取整可能改变比例；网格块数本身超限时仍失败。分块/DeepZoom 不按整图上限适配。输出计划与已保存产物分开记录，预览及报告使用实际主图路径。规则和验收见 [输出格式与超限适配](../verification/output-format-verification.md)。

---

## 6. Bug 历史与经验教训 / Bug History

> **EN**: Major historical bugs and lessons learned. Highlights: CUDA integer truncation (180/8=22), non-contiguous database IDs, shallow-copy + in-place mutation in ImageCache.

### v1.0 8×8 Grid 迁移发现的 4 个 bug

| # | 文件 | Bug | 影响 |
|---|------|-----|------|
| 1 | `CudaBackend.cu:62` | Grid 距离循环 `i<16`（应为 64） | 仅算 1/4 格子 |
| 2 | `FeatureExtractorCuda.cu:117` | Cell 索引 `*4`（应为 `*8`） | 行覆盖 |
| 3 | `FeatureExtractorCuda.cu:20` | `GRID_CW=180/8=22` 截断 | 🔴 越界写共享内存 |
| 4 | `FeatureIndex.h:78` | `addPoint(data, 0)` 仅加 1 点 | 🔴 所有 tile 同一张图 |

### v0.7 FeaturePack v1 ID 映射 bug

SQLite `INSERT OR IGNORE` 消耗自增 ID 导致间隙，FeaturePack v1 假设 ID 连续 → 所有 tile 错配。v2 每记录显式存 image_id。

### v0.8 ImageCache 浅拷贝 bug

`cv::Mat` 返回浅拷贝 + `adjustColor` 原地修改 → 热门图被反复调整颜色变暗。修复：返回 `.clone()`。

### 经验

1. CUDA kernel 整数截断 (`180/8=22`) 是常见陷阱，用 ceil 除法 + 边界钳制
2. 数据库 ID 不能假设连续，尤其在 `INSERT OR IGNORE` 场景
3. 缓存返回浅拷贝时，任何修改操作都会污染源数据
4. 每次迁移必须有可量化对比（SHA256 + Analyze），否则无法发现退化

---

## 7. 版本演进 / Version History

> **EN**: SemVer. v0.x = prototype → v1.x = stable CLI → planned v2.0 = Avalonia GUI. See timeline below.

采用 [SemVer](https://semver.org)：`MAJOR.MINOR.PATCH`

| 版本 | 关键变更 | Score |
|------|----------|-------|
| 0.4.0 | inspect 命令 | — |
| 0.5.0 | ANN hnswlib 候选选择 | — |
| 0.6.0 | DeepZoom HTML、颜色校正、benchmark、topn-random | — |
| 0.7.0 | FeaturePack 缓存 (50K→2 fread)、BigTIFF、ANN 持久化 | — |
| 0.8.0 | ImageCache、8×8 Grid (192维)、智能格式切换、自适应权重 | — |
| 1.0.0 | --upscale、--output-tile、WebP 支持、默认 tile 180×320 | — |
| 1.1.0 | `--analyze` 质量评估体系（分数统计/特征贡献/复用率/热力图） | 0.1646 |
| 1.3.0 | Grid 贡献分析(8×8热图)、Candidate sweep(30≈200)、db-stats | — |
| 1.4.0 | **Spatial Weight Map**（首个数据驱动质量优化）、最差tile导出 | **0.1524** |
| 1.5.0 | 覆盖缺口分析、权重自动生成 | 0.1524 |
| 1.6.0 | **candidates sweep**, CPU=GPU, 诊断报告 | **0.1256** |
| 1.7.0 | **GPU Features** (43×); sqrt移除; Grid去重优化 | 0.0993 |
| 1.8.0 | 增/减量 DB, neighborWindow 动态, HTML 报告, db-usage | 0.0993 |
| **1.9.0** | **db-health** 健康度诊断 + 建议 | 0.0993 |
| **1.10.0** | **流式TIFF/PNG** (超大图免全图Mat, 17×加速), HTML WorstTile, 回归测试 | 0.0993 |
| 1.11.0 | Web 命令生成器 (静态 HTML, 7子命令, 46参数) | — |
| 1.12.0 | 架构重构: 流式写入、DB自包含、GPU多分辨率、WebUI集成、Linux支持 | — |
| 1.12.3 | MOSAICRAFT_CUDA 编译宏 (纯CPU编译) | — |
| **1.13.0** | **REST API**: 15端点, 异步任务, 合约版本化, 服务层解耦 | — |
| 1.13.1 | API 合约完善, 文档化 | — |
| 1.13.2 | CLI 缺值校验 + PngStreamWriter 清理 | — |
| 1.13.3 | **全面 bug 审查**: 5 HIGH + 6 MEDIUM 修复, 数值校验, 死代码清理 | — |
| 1.13.9 | 发布准入清单、平台/运行时包命名、远端 CPU-only CI、vcpkg 缓存、Web UI/API 错误反馈打磨 | — |
| 1.13.10 | tag/Release/zip 一致性、BUILD_INFO、真实附件验收强制化 | — |
| 1.14.1 | 有界邻域比较、按使用次数释放缓存、CPU特征包校验、PNG多行写入 | 见性能报告 |
| 1.14.0 | 196维并行ANN、字节预算缓存、公共流式输出、CUDA缓冲复用与失败处理 | 见性能报告 |
| 1.14.2 | 默认平均色HSV建库、可恢复全库重编号、v6特征一致性与API 2.0边界修复 | 见验证记录 |
| 1.14.3 | 特征缓存一致性、有界建库与匹配、任务内缓存及工作区复用、无偏对称亮度调整 | 见性能记录 |
| 1.14.4 | 基础图透明叠加、全图坐标逐块采样、CLI/API/Web UI 参数同步 | 见叠加验证记录 |
| 1.14.5 | CUDA 传输复用、动态尺寸特征提取、独立 CPU 兼容 CUDA 评分 | 见性能与兼容说明 |
| 1.14.6 | 可选预览、同尺寸素材原字节保存、不同尺寸 JPEG 建库质量 100 | 见本版更新记录 |
| 1.14.7 | 三平台 CPU 发布、云端 CUDA 候选、精简 Release 依赖与独立包验收 | 见 CI 与打包说明 |
| 1.14.8 | 七架构 CUDA、Linux CUDA 正式附件与实际内核预检 | 见 CUDA 验证报告 |
| 1.14.9 | 统一运行诊断、阶段进度、API 2.1 与 Web UI 任务和结果体验 | 见运行诊断验收 |
| 1.14.10 | Web UI 离线使用帮助、参数说明、明确预览默认状态与窄屏排版 | 见对应更新记录 |
| **1.14.11** | 按来源选择输出格式、超限转 PNG 或缩小 Tile、API 2.2 输出计划与实际产物 | 见本版更新记录 |
| **1.15.0** | 内部版本重置、严格新版图库与 API 1.0、删除旧库自动升级和命令接口 | 见不兼容更新验收 |
| **1.15.4** | 实时预览进度条；按维护者要求跳过全部测试，CUDA 未验收 | 见本版更新记录 |
| **1.15.3** | 十种 CUDA 分辨率模板、预览取消总格数上限、默认放大与进度日志、GPL-3.0 | 见本版更新记录 |
| **1.15.2** | 预览热调参、按需推荐、高密度平移、流式 PNG 与 CPU 建库 | 见预览验收 |
| **1.15.1** | 独立实时预览页、整库手动选图、窗口调节及 API 1.2 | 见预览验收 |
| **2.0.0** | **Avalonia GUI** 首发 (CLI→GUI) | 计划中 |

> v2.0 不代表算法更强，代表使用方式从 CLI 变为 GUI。Major 版本反映交互模式的根本变化。

### Bug 历史（精选）

| # | 版本 | 文件 | Bug | 影响 |
|---|------|------|-----|------|
| 1 | v1.0 | `FeatureExtractorCuda.cu:20` | `GRID_CW=180/8=22` 整数截断 | 🔴 越界写共享内存，全库 LAB 损坏 |
| 2 | v1.0 | `FeatureIndex.h:78` | `addPoint(data, 0)` 仅加 1 点 | 🔴 所有 tile 同一张图 |
| 3 | v0.7 | `FeaturePack.h` | v1 假设 ID 连续 | 🔴 全 tile 错配 |
| 4 | v0.8 | `ImageCache.h` | 浅拷贝+adjustColor 原地修改 | 热门图反复调色变暗 |
| 5 | v0.8 | `MosaicEngine.cpp` | neighborWindow=300 < tilesX(319) | 垂直邻域漏罚 |
| 6 | v1.0 | `CudaBackend.cu:62` | Grid 循环 `i<16`(应为64) | 仅 1/4 格 |
| 7 | v1.0 | `FeatureExtractorCuda.cu:117` | Cell 索引 `*4`(应为*8) | 行覆盖 |

---

## 8. 构建与发布 / Build & Release

当前源码构建的工具要求、固定依赖安装、平台 Preset、测试与发布命令统一维护在 [README：安装与构建](../../development/building.md)。首次构建请从该入口开始；测试程序不参与默认构建，须按说明显式构建测试目标后再运行 CTest。

- [CI 与原生发布包](../../development/packaging.md)：依赖缓存、运行库部署、平台包和独立附件验收。
- [正式发布准入清单](../../development/releasing.md)：发布所需的平台检查与 GPU 验收条件。
- [CUDA 多架构验证](../../development/cuda.md)与 [CUDA 13.5 迁移记录](../gpu/cuda-13-5-migration.md)：当前工具链边界和历史实测。

下方版本记录描述对应版本的历史行为；当前构建、打包和验收要求以上述入口为准。

---

## 9. 开发规范 / Coding Conventions

> **EN**: C++17, CMake 3.20+, MSVC 2026. Allman brace style. Mandatory `{}` on `if`. Chinese git commit messages. CUDA: watch integer division truncation.

- C++17，CMake 3.20+，MSVC 2026
- 左花括号 `{` 另起一行
- `if` 语句显式使用 `{}`
- 关键逻辑加注释解释为什么
- Git 日志使用中文
- CUDA 代码注意整数除法截断，用 ceil 除法 `(N+M-1)/M`
- 修改评分/匹配逻辑后必须跑 `--analyze` A/B 对比

---

## 10. 已知限制与未来方向 / Limitations & Roadmap

> **EN**: Known limits: WebP has a 16,383px cap, template CUDA and CPU floating-point features can differ, and edge density has limited discrimination at tile scale. Generic CUDA preserves CPU feature rounding in the same OpenCV/build environment. Appends are supported and renumber the entire library by color. Roadmap: face/saliency weighting and GUI.

### 已知限制

- WebP 限制 16383px（低于 JPEG 65500px）
- ANN 只读查询支持并发，v1.14.0 默认最多 8 线程；构建/修改索引期间不执行查询
- 小块放大后边缘特征的区分度有限；CPU/CUDA 均使用 Canny 定义，模板 CUDA 的其他浮点特征仍可能与 CPU 有精度差异；通用 CUDA 在相同 OpenCV/构建环境中保留 CPU 舍入结果
- 自适应权重当前版退步，已冻结
- `--append` 支持增量建库；完成时按颜色重排全库，旧图片文件名可能改变

### 已验证结论

- candidates=150 是现有默认折中，实际召回和匹配质量依赖样本
- topNrandom=10 保证多样性
- Grid 8×8 是核心竞争力（贡献 69%）
- 颜色校正 LAB L 通道方式正确但默认关闭（视觉差异有限）

### 未来方向

| 优先级 | 方向 |
|--------|------|
| P1 | Analyze 增强（分类细分、最差 tile 定位） |
| P2 | 面部/显著区域权重 |
| P2 | DeepZoom viewer 增强 |
| P3 | GUI |

---

## 11. 更新日志 / Changelog

> **EN**: Detailed version changelog. See git tags for each release.

### v1.11: Web 命令生成器
- 静态 HTML: `tools/command-builder/index.html` 双栏布局
- 7 个子命令全覆盖，46 个参数与 CLI 同步
- GitHub 明色主题

### v1.12: 架构重构 + 性能优化

**流式写入**
- PNG 流式写入 (PngStreamWriter): 逐行写盘，内存恒定 ~162KB
- JPG 流式写入 (JpgStreamWriter): libjpeg 直接 API
- `--write-mode stream/batch/auto` 统一管理 PNG/TIFF/JPG

**数据库**
- DB 自包含: 统一目录内包含 `mosaicraft.db`
- DB 元数据: `meta` 表存储 `feature_w/h`，支持多分辨率适配
- `--normalize-size <WxH>` 自动统一归一化分辨率
- GPU 多分辨率（v1.12 当时的模板范围）: 180x320 / 320x180 / 360x640 / 640x360；当前自定义尺寸支持见上文动态尺寸说明
- 输出 tile 自动匹配 DB 尺寸(横图320x180, 竖图180x320)

**性能 (45K 图库, RTX 4060)**
- 建库: 30min → 2.3min (13x 提升)
- GPU_BATCH 32→256
- CPU/GPU 流水线并行
- FeaturePack 多线程读取
- 背压控制 (MAX_QUEUE=512) 防 OOM

**CLI 规范**
- `--output-tile WxH` 统一尺寸格式
- `--force`/`-y` 安全确认 + TTY 检测
- 退出码 0/1/2/3/4 + help 文档化
- 31 个参数标签说明统一，帮助文本统一
- 默认值: db=`library/mosaicraft.db`, output=`output/output`，单图后缀按最终格式追加

**Web UI 集成**
- `MosaicraftWebUI.exe`: cpp-httplib HTTP 服务器
- Web UI 改为使用结构化 API，API 列表详见 `docs/API.md`
- `GET /api/info` 返回版本和 API 能力摘要
- `POST /api/run` 默认关闭，仅通过 `MOSAICRAFT_ENABLE_LEGACY_RUN=1` 作为旧命令兼容入口
- 30min 超时，管道批量读取，端口冲突提示

**Linux 支持 (v1.13 目标)**
- 条件编译 `isatty`/`getchar`/`unistd.h` 已补
- 核心算法跨平台（已在 WSL/原生实测）

**健壮性**
- GPU cudaMalloc 全部检查返回值
- `--normalize-size`/`--output-tile`/`--write-mode` 格式校验
- `\r` 进度行覆盖 + 控制台暂停(AutoPause)
- 统一压缩质量默认值 (JPEG q90, PNG lv9)

### v1.12.3: 支持 MOSAICRAFT_CUDA
- `cmake -D MOSAICRAFT_CUDA=OFF` 纯 CPU 编译
- 条件编译隔离 CUDA 头文件/链接依赖
- `core/CudaStubs.cpp`: 无 CUDA 时的空实现
- Windows/Linux 双平台通过


## 12. v1.13 更新日志 (2026-06-28)

### v1.13.1: REST API · 异步任务 · 合约版本化

### v1.13.2: Bug 修复 · CLI 校验 · PngStreamWriter 清理 (2026-07-01)

### v1.13.3: 全面 bug 审查修复 — 5 HIGH + 6 MEDIUM (2026-06-29)
- 17 端点 REST API: /api/jobs/*, /api/db/*, /api/mosaic, /api/inspect
- 异步任务: POST /api/jobs/* 提交 → GET /api/jobs/{id} 轮询
- 服务层解耦: MosaicService / BuildService, CLI 与 HTTP 共享 core
- API 合约: apiContractVersion(), /api/info 暴露稳定性
- API 文档: docs/API.md 完整端点目录
- 测试: 19→43 用例, 27→475 断言
- Legacy /api/run 保留: 向后兼容

### v1.13.7: 发布收口 · API 稳定性 · Web UI 可靠性 (2026-07-05)

v1.13.7 是一次发布收口版本，重点是提升可交付性、API 稳定性和 Web UI 运行可靠性。此版本没有引入新的核心算法方向，主要目标是确保 CLI、Web UI、结构化 API 和发布包行为一致。

主要变化：
- 统一版本与发布文档到 `1.13.7`
- 收紧结构化 API 输入校验，非法字段类型会返回明确错误
- Web UI 默认使用结构化 API，不再依赖旧式 `/api/run` 命令端点
- Web UI 启动后会校验 `/api/info` 和 `/api/endpoints`，确认 API 合约和必需端点可用
- Web UI 增加前置表单校验，`mosaic`、`build`、`inspect` 的必填项会在请求前检查
- Web UI 对导出和实际清理类操作增加确认步骤
- 新增服务层回归测试，覆盖建库、数据库统计、图片检查、生成马赛克和错误路径
- 新增 Web UI/API smoke 验证，真实启动 `MosaicraftWebUI.exe` 并通过 HTTP 提交 build/mosaic job

验证范围：
- CUDA 默认 Release 构建通过
- CPU-only Release 构建通过
- `mosaicraft_tests` 通过
- `mosaicraft_regression_tests` 通过
- `mosaicraft_webui_smoke` 在 CUDA 构建和 CPU-only 构建上通过
- `tools/command-builder/index.html` 内嵌脚本语法检查通过

兼容性说明：
- `/api/run` 仅作为旧命令兼容端点保留，默认禁用
- 新 Web UI 和新集成方应使用结构化 API
- CUDA 构建需要可用的 NVIDIA 驱动；无 GPU 环境可使用 `MOSAICRAFT_CUDA=OFF` 构建

### v1.13.8: 发布工程增强 · 版本一致性 (2026-07-05)

v1.13.8 是 v1.13 系列的发布工程增强版本。相比 v1.13.7，本版本将近期较多的发布一致性、API 校验、服务层回归测试和 Web UI/API 验证工作统一纳入新的补丁版本号。

主要变化：
- 将项目版本从 `1.13.7` 提升到 `1.13.8`
- 保持 CLI、API、Web UI badge、README、百科文档和测试断言中的版本一致
- 保留结构化 API 输入校验收紧结果，非法字段类型会返回明确错误
- 保留 Web UI 对 `/api/info` 和 `/api/endpoints` 的 API 合约校验
- 保留 Web UI 前置表单校验和危险操作确认
- 保留服务层回归测试，覆盖建库、数据库统计、图片检查、生成马赛克和错误路径
- 保留 Web UI/API smoke 验证，真实启动 `MosaicraftWebUI.exe` 并通过 HTTP 提交 build/mosaic job

验证范围：
- CUDA 默认 Release 构建通过
- CPU-only Release 构建通过
- `mosaicraft_tests` 通过
- `mosaicraft_regression_tests` 通过
- `mosaicraft_webui_smoke` 在发布构建上通过
- 解压发布包后，CLI 返回 `Mosaicraft 1.13.8`，Web UI/API smoke 通过

兼容性说明：
- `/api/run` 仅作为旧命令兼容端点保留，默认禁用
- 新 Web UI 和新集成方应使用结构化 API
- CUDA 构建需要可用的 NVIDIA 驱动；无 GPU 环境可使用 `MOSAICRAFT_CUDA=OFF` 构建

### v1.13.9: 发布准入与 CI 门禁稳定化 (2026-07-05)

v1.13.9 是 v1.13 系列的发布工程与交付稳定性补丁版本。相比 v1.13.8，本版本不改变核心马赛克算法，重点固化发布准入、远端 CI 门禁、vcpkg 缓存和 Web UI/API 错误反馈。

主要变化：
- 将项目版本从 `1.13.8` 提升到 `1.13.9`
- 新增正式发布准入清单 `docs/RELEASE_CHECKLIST.md`，明确本机 CUDA 包和远端 CPU-only CI 的分工
- 增强 `scripts/release.ps1`
  - 新增 `-InspectOnly` 发布前环境检查模式
  - 发布包名包含平台、架构和运行时，例如 `Mosaicraft_v1.13.9_windows-x64_cuda.zip`
  - 自动检查 CMake、CTest、PowerShell、vcpkg toolchain、发布必需文件和现有 build 输出
  - 可从 `CMakePresets.json` 自动读取默认 vcpkg toolchain
  - 不再要求源码仓库新增单版本说明文件，历史说明统一维护在本百科中
- 新增 `scripts/verify-release-asset.ps1`
  - 发布后从 GitHub Release 下载真实 zip 附件
  - 校验 GitHub asset digest、Release 页面 SHA256 和本地文件 SHA256 一致
  - 使用解压后的包内可执行文件验证 CLI/Web UI 最小可用性和 Web UI/API smoke
- 完成 GitHub Actions Windows CPU-only CI 门禁
  - 构建、CTest、Web UI/API smoke、打包、解压验证、artifact 上传
  - 启用 vcpkg binary cache，后续依赖安装从约 44 分钟降至约 19 秒
- 打磨 Web UI/API 错误展示
  - 显示 HTTP 状态、API 路径、`exitCode` 和字段名
  - 对常见字段错误提供明确修正建议
  - 取消任务、清理任务、轮询任务失败路径统一使用结构化错误展示

验证范围：
- `scripts/release.ps1 -InspectOnly -BuildDir build -Configuration Release` 通过
- CUDA 默认 Release 构建通过
- CPU-only Release 构建通过
- `mosaicraft_tests` 通过
- `mosaicraft_regression_tests` 通过
- `mosaicraft_webui_smoke` 在发布构建上通过
- `scripts/release.ps1 -BuildDir build -Configuration Release` 生成并解压验证 `Mosaicraft_v1.13.9_windows-x64_cuda.zip`
- GitHub Release 真实附件按当时的下载、SHA256、包结构和 Web UI/API smoke 策略验收通过；v1.13.10 起新增 `BUILD_INFO.txt` 与 tag commit 强校验
- 远端 GitHub Actions `CI` 在 `main` 上通过

发布包内容：
- `mosaicraft.exe`
- `MosaicraftWebUI.exe`
- `index.html`
- 运行所需 OpenCV / SQLite / TIFF / PNG / JPEG / WebP / CUDA runtime DLL
- `README.md`
- `API.md`
- `LICENSE`
- `third_party_versions.txt`
- `ENCYCLOPEDIA.md`

兼容性说明：
- 核心 CLI、HTTP API 合约主版本和输出算法保持兼容
- `/api/run` 仍仅作为旧命令兼容端点保留，默认禁用
- CUDA 正式包仍由本机或专用 GPU 环境构建；GitHub hosted CI 当前验证 CPU-only 基础门禁

### v1.13.10: Release/tag/zip 一致性标准化 (2026-07-05)

v1.13.10 是一次发布可追溯性修正版本。v1.13.9 发布后，源码继续加入真实附件验收脚本、Web UI/API 任务体验优化和发布文档整理，导致 `v1.13.9` tag、Release 页面和最终 zip 内容不再天然对应。本版本通过新的补丁号重新收束，确保 tag、源码、zip 和 Release 页面一致。

主要变化：
- 将项目版本从 `1.13.9` 提升到 `1.13.10`
- 发布包新增 `BUILD_INFO.txt`
  - 记录版本、包名、平台、架构、运行时、构建配置和 git commit
  - `scripts/release.ps1` 打包后会解压校验 `BUILD_INFO.txt`
- `scripts/release.ps1` 正式打包前要求 git 工作区干净，避免包内容包含未提交改动
- 增强 `scripts/verify-release-asset.ps1`
  - 读取 GitHub tag 指向的真实 commit
  - 校验 Release zip 内 `BUILD_INFO.txt` 的 commit 与 tag commit 一致
  - 继续校验 GitHub asset digest、Release 页面 SHA256、包结构和 Web UI/API smoke
- 固化发布规则：如果发布后源码或发布流程继续变更，应进入下一个 patch 版本，不继续替换旧 tag 下的正式附件

验证范围：
- PowerShell 语法检查通过
- CUDA 默认 Release 构建通过
- `mosaicraft_tests` 通过
- `mosaicraft_regression_tests` 通过
- Web UI/API smoke 通过
- 发布包解压验证和 `BUILD_INFO.txt` 校验通过
- `scripts/verify-release-asset.ps1 -Tag v1.13.10` 验证 GitHub Release 真实附件通过

### v1.14.0: 性能与稳定性 (2026-10-08)

本轮保留五特征定义、评分权重、邻域去重规则及 CLI/API 默认值。粗筛改用 196 维，新缓存失效自动重建且保留旧缓存。CPU/GPU 共用并行候选查询与输出流程。图片缓存默认 512 MiB；颜色调整修改独立副本；流式输出固定线程池与两行小块缓冲。CUDA 特征工作区按任务复用，评分使用不超过 256 MiB 的临时显存，失败立即中止生成。

新增关键回归覆盖缓存失效、串并行查询、缓存预算与修改隔离、输出模式像素一致性，以及真实 CUDA 工作区复用和分批评分。使用相同数据库快照、固定 `--topn-random 1`、关闭颜色调整进行预热和三次采样。完整数据、方法和验收边界见 `docs/PERFORMANCE_v1.14.0.md`。

> **EN**: Version 1.14.0 keeps all five matching features and public defaults. A validated 196-dimensional ANN cache supports concurrent read-only queries. A byte-budgeted image cache and shared output pipeline bound streaming buffers. CUDA feature allocations persist for a task, scoring uses bounded batches, and runtime failures abort generation. See the performance report for measured results and validation limits.


### v1.14.1: 选图提速与内存收敛 (2026-10-08)

邻域 Grid 去重使用阈值提前退出和任务内有界判断缓存，CPU 只排序需要的前 N 项。图片缓存按剩余使用次数释放，单次图片不驻留，同图并发请求共享解码结果；512 MiB 缓存上限保持不变。CPU 候选特征复用 v2 特征包的顺序读取，新增 `features.meta` 验证源文件路径指纹、大小、修改时间及包内特征摘要，失效时回退逐文件读取。元数据是可丢弃的加速层，不要求重新建图库，不改 GPU 特征读取和算法。

PNG 使用有界多行 RGB 转换及写入；过滤器、压缩等级和每 1000 行刷新策略保持不变。`--benchmark` 增加邻域惩罚、排序、分析、加载、转换、写入、等待与缓存统计；工作线程加载时间是累加时间，不与墙钟总耗时相加。基准脚本新增 `--analysis-mode off|on|both`，默认分别采样普通生成和分析模式。性能数据及验收边界见 `docs/PERFORMANCE_v1.14.1.md`。

> **EN**: Version 1.14.1 preserves matching rules and public defaults. Bounded neighbor comparisons, candidate feature buffers, usage-aware image caching and multirow PNG output reduce repeated work. PNG filters, compression and flush boundaries remain unchanged. Benchmark output separates normal generation from analysis overhead; the performance report records measured results and limitations.

### v1.14.2: 默认 HSV 颜色建库与边界修复 (2026-10-12)

此版本将平均颜色排序设为默认建库行为，同时发布已完成的建库、匹配特征、文件输出和 HTTP API 边界修复。外部 HTTP API 合约升级为 **2.0**；同步 `/api/mosaic` 返回生成结果，维护清理仅接受 POST，结构化请求要求 JSON 并校验来源。命令行与内置 Web UI 已同步。

建库直接复用数据库平均 LAB，描述整张图片的综合色调；不读取缩略图、不使用主色回退。按 OpenCV 编码还原 `L*=avgL×100/255`、`a*=avgA−128`、`b*=avgB−128`，在保留小数时判断 `C < 0.5` 的灰度区。彩色将平均 LAB 转为 BGR，再转 HSV，以 345° 为起点，每 15° 一个区间，按交替明暗、精细色相和色度排列，相同键按内容哈希和图片 ID 排列。普通、重复、追加及同目录建库均执行全库重排；仅归一化模式保持原行为。独立颜色缓存升级为 v2，并记录来源均值，匹配特征仍为 v6。CPU/CUDA 独立建库允许平均颜色精度差异，同一数据库重复排序稳定。

两阶段重命名、数据库路径事务、持久化恢复清单和跨进程图库锁共同保护图片与索引。图片 ID、使用统计及特征引用保持稳定，新特征文件使用内容哈希命名。未索引的冲突文件不会被覆盖，已索引图片跨卷移动会失败。实际验证和样本耗时见 [颜色排序验证](../verification/color-order-verification.md)。

匹配特征 v6 统一 CPU/CUDA 的 8×8 分区、Canny 边缘定义、统计字段及评分尺度，去重使用落盘图像像素哈希。旧图库需运行 `build --append` 刷新特征并完成颜色重排；图片字节、ID 和使用统计保持对应，选图结果可能与旧特征版不同。还修复重复建库误删、中文特征及索引路径、特征包 ID 校验、数据库损坏检查与并发使用统计、取消竞态，以及 PNG/DeepZoom/TIFF 的失败处理。超大输出默认允许，超出编码器或整数范围时明确报错。具体用例与兼容边界见 [边界与一致性修复验证](../verification/bugfix-verification.md)。

验收使用实际 CUDA 和 CPU-only 构建、核心及服务回归、Web UI/API smoke。45,625 张现有图库重排后，连续编号为 `000000`–`045624`，灰度区 146 张、彩色区 45,479 张；图片与独立特征 SHA256、ID、使用统计和特征引用保持一致，重复运行补算 0 张、改名 0 张。颜色缓存算法版本为 2，与软件版本和匹配特征版本分别管理。

> **EN**: Version 1.14.2 reuses stored mean LAB for default HSV v2 ordering and renumbers indexed files after appends. Chroma below 0.5 enters grayscale; 15-degree hue bands start at 345 degrees and alternate lightness direction. Color caches retain source means; independent CPU/CUDA builds can differ, while repeated ordering of one database is stable. Locks and recoverable two-phase renames preserve image bytes, IDs, statistics and feature references. Matching features v6 align backend definitions; old libraries need `build --append` to refresh. HTTP API contract 2.0 tightens JSON and origin checks and returns synchronous mosaic results. See the verification records for compatibility changes, fault checks and the 45,625-image library acceptance.


### 第一轮缓存与性能更新（版本号保持 1.14.2）

CPU/GPU 共用独立特征文件的 Unicode 读取和来源校验。缓存的 `features.pack` 清单指向同代次的 TinyImage、LBP 和二进制校验元数据；绑定记录 ID、特征版本、图片内容哈希及来源文件的路径、大小、修改时间和数据摘要。不支持的特征缓存回退到新版源特征并重新生成；新代次完整落盘后才切换清单，失败保留旧清单。无变化追加复用有效缓存，旧代次只在建库独占锁下清理。

颜色重排复用本次路径信息，输出目录卷号只读取一次，路径、别名、目标冲突、跨卷和恢复检查保留。CPU 特征按最多 256 张、64 MiB 待处理像素分批并行计算，去重与数据库写入按扫描顺序进行，事务失败回滚整批；同目录输入采用单张批次，以兼容不同尺寸和超预算图片。CUDA 目标块准备和 Canny 补算各复用最多 8 个默认工作线程，计算公式不变。

大图库在 Windows 批量枚举特征目录的文件属性，减少逐文件系统调用；失败或未命中项仍查询原文件属性。建库日志分别记录颜色校验、排序与文件/SQL 写入，缓存校验/来源读取与写入，以及包含扫描和缓存的墙钟总耗时。流水线阶段存在重叠，不能直接相加。内存和性能验收见 [第一轮验收记录](../performance/performance-phase1.md)。

> **EN**: This update keeps version 1.14.2, matching features v6 and API contract 2.0. Both backends share Unicode source reads, cache validation and missing-feature penalties. Immutable cache generations publish through one manifest; unverified v2 packs fall back to source files and builds upgrade them automatically. Unchanged builds reuse valid packs and color checks reuse path information. CPU feature batches retain scan-order database commits and roll back failed transactions. CUDA preparation and Canny use reusable worker pools. Timings separate validation, writing and wall time; see the phase-one report for measured performance and memory limits.

### v1.14.3: 缓存一致性、有界匹配与对称亮度调整 (2026-10-14)

此版本发布两轮缓存与性能优化及无偏对称亮度调整，匹配特征仍为 **v6**，HTTP API 合约仍为 **2.0**，生产参数保持兼容。

- CPU/CUDA 共用来源校验和 Unicode 特征读取；旧包、损坏包及来源变化回退独立特征文件。新缓存按完整代次原子发布，失败保留旧包，无变化追加复用有效缓存。缺失特征保留评分惩罚。
- CPU 建库采用最多 256 张、64 MiB 待处理像素的并行批次，按扫描顺序去重并提交事务；颜色重排复用路径信息和 SQL，CUDA 准备与 Canny 使用线程池。详细数据见 [第一轮验收记录](../performance/performance-phase1.md)。
- 匹配主机缓冲及 CPU 特征缓存预算各为 256 MiB，每批最多 1024 块，按任务需要分配。任务内复用特征读取会话、预载/LRU、ANN、线程池和 CUDA 工作区；预算容许时提前准备一批。选图结果保存图库索引，分析仅保留最差 20 块的详细特征。预算不代表进程总内存上限。
- 亮度调整继续默认关闭；开启后使用 1001 档均匀分布，LAB L 乘数改为 `1±colorStrength`，默认 `0.04` 对应 `0.96～1.04`。与此前 `0.988～1.028` 相比，扰动更宽且乘数平均值为 1；强度 0 保持原像素。像素取整、高亮截断和颜色转换仍可能影响最终平均亮度。CLI、Web UI 和 API 说明已同步。

本机固定 160,000 块样本的 CUDA 总耗时中位数由 43.800 s 降至 33.168 s（约 24.3%），其他场景的收益及内存口径见 [第二轮验收记录](../performance/performance-phase2.md)。常规 CPU/CUDA 样本分别通过索引、输出哈希及解码像素比较；CUDA 大样本仍存在基线自身的浮点累加顺序与近似同分选图波动，不保证该场景逐字节一致。亮度调整关闭及零强度的 1600 块样本在两种后端中均与调整更新前完全一致。

> **EN**: Version 1.14.3 ships validated feature-cache generations, bounded parallel library builds and bounded matching with task-local caches and reusable workspaces. Host matching buffers and CPU feature payload caches each have a 256 MiB budget, with at most 1024 tiles per batch; these are not process memory limits. Features remain v6 and the HTTP API remains 2.0. Optional brightness jitter remains off by default and now samples 1001 unbiased symmetric LAB L multipliers in `1±strength`; default strength 0.04 spans 0.96–1.04, and zero strength preserves pixels. The fixed local 160,000-tile CUDA benchmark improved by about 24.3%; the reports retain scene-specific results and the existing floating-point near-tie reproducibility limitation.

### v1.14.4: 基础图透明叠加 (2026-10-15)

新增 `--overlay-strength` / API `overlayStrength`，范围为有限数 `0..1`，默认 `0` 关闭。设置大于零的强度即可在选图后混合基础图，例如 `0.2` 为 80% 素材与 20% 基础图。Web UI 同步提供叠加强度输入框和命令生成；同步与异步生成接口均支持。匹配特征仍为 **v6**，HTTP API 合约仍为 **2.0**，无需重建图库。

参考图沿用匹配流程的尺寸调整、upscale 和镜像补边，按全图像素中心坐标逐块双线性采样，跨块读取相邻像素，只有参考图外边界夹紧。普通、流式、分块和 DeepZoom 共用合成入口，DeepZoom 先合成源小图再生成金字塔。素材缓存保持只读，各位置修改独立副本；同时启用随机亮度调整时，先调整素材亮度，再叠加基础图。缺失素材沿用失败计数和背景，分析报告注明分数衡量选图质量，而非最终合成画面。

CUDA 和 CPU-only 构建、完整 CTest、Web UI/API smoke、实际 CUDA 匹配及 24 次样图运行均已验证。关闭叠加的固定样本输出与 1.14.3 完全一致；同条件 PNG 样本的 20% 叠加耗时中位数约 13.37 s，原版约 13.38 s，此结果不代表其他图库或格式的性能。叠加仅保留目标工作图和每个工作线程的小块缓冲，不分配完整输出尺寸的基础图。输出面积相差四倍的两档样本中，新增工作线程缓冲预算均约 2.66 MiB；预算不代表进程总内存上限。详细方法、数据及效果边界见 [基础图叠加验证](../verification/base-image-overlay-verification.md)。

> **EN**: Version 1.14.4 adds optional base-image overlay through `--overlay-strength` and API `overlayStrength`, with range 0–1 and default 0 (off). The Web UI, synchronous API and asynchronous jobs expose the same setting. Overlay blends the processed working target after matching and optional brightness jitter. Global-coordinate bilinear sampling is shared by regular, streaming, tiled and DeepZoom output; source tiles are composited before pyramid generation. Cached pixels remain immutable and matching features v6/API contract 2.0 stay compatible. Fixed default-output samples match 1.14.3 exactly. Validation covers CPU-only and actual CUDA matching, output paths, parameter bounds and 24 image runs; reported performance and memory budgets are scene-specific.

### v1.14.5: 动态尺寸 CUDA 与提取、评分独立加速 (2026-10-23)

本版本发布 CUDA 传输流水线优化、通用尺寸特征统计与 CPU 兼容 CUDA 候选评分。四种固定模板保留原有内核算术，通过任务内锁页缓冲、异步传输与 CPU Canny 重叠、设备特征复用减少重复工作。其他合法尺寸（宽高各 1～8192）默认尝试通用 CUDA 提取和兼容评分，无需增加 CLI/API 参数。特征版本仍为 **v6**，HTTP API 合约仍为 **2.0**，现有 v6 图库无需重建。

自定义尺寸继续使用原 OpenCV 预处理，GPU 归约 LAB、Grid 和 LBP 的整数统计，CPU 按原运算顺序完成特征收尾；兼容评分严格保持原 CPU 算术、缺失特征惩罚与使用次数惩罚。CPU 保留 ANN、邻域处理、排序、随机选图和分析。相同标准构建/OpenCV 环境、输入、图库快照、使用计数和随机种子下，特征、分数及最终选图保持精确一致；不承诺跨 OpenCV 版本一致。

提取和评分独立准备资源、独立回退。两者的设备暂存合计不超过 256 MiB，常驻图库单独计量；按预算缩小批次。计算前资源不足可回退对应 CPU 能力，上传、内核和同步故障则收束并报错。`--cpu`、无 CUDA 和 CPU-only 构建保持纯 CPU。修复原评分工作区显存余量重复扣减。

CUDA/CPU-only CTest、两套 Web UI/API smoke、真实 GPU 精确回归及 Compute Sanitizer 已完成，动态特征和兼容评分内存检查均为 0 错误。两项新增能力合并后的直接对比使用私有 96 张图库、25,600 块：256×256 普通/分析总耗时减少 21.5% / 18.2%，512×512 普通减少 8.0%、分析基本持平；1600 块的 256×256 短任务反而慢 10.3%～13.6%。这些是固定本机样本的完整生成结果，不能用评分内核倍数推算整个生成过程，也不承诺所有场景提速。详见 [v1.14.5 性能与兼容说明](../performance/performance-v1.14.5.md)、[传输流水线验证](../gpu/cuda-transfer-pipeline.md)、[动态特征验证](../gpu/dynamic-cuda-features.md)和 [独立评分验证](../gpu/independent-cuda-scoring.md)。

> **EN**: Version 1.14.5 ships reusable asynchronous CUDA transfers, generic feature statistics for valid custom dimensions, and independently selected CPU-compatible CUDA scoring. Existing v6 libraries, CLI/API parameters and API contract 2.0 remain compatible. Custom-size preprocessing, scoring arithmetic, missing-feature penalties and CPU selection semantics are preserved under the same build and inputs. Resource shortages before submission fall back independently; runtime failures abort. A direct 96-image, 25,600-tile comparison reduced total generation time by 18.2–21.5% at 256×256 and 8.0% for normal generation at 512×512, while 512×512 analysis was essentially unchanged. Short 256×256 tasks became 10.3–13.6% slower. See the report for exact baselines, full samples and scope.

### v1.14.6: 可选预览与同尺寸素材免重编码 (2026-11-08)

新增命令行 `--preview`，默认关闭。JPG、PNG、TIFF、WebP 单图输出可同步生成 `<主图名（不含扩展名）>_preview.jpg`，沿用最终主图路径，支持中文路径和 `--format` 改后缀。预览保持比例、不放大，最大宽度 1400、总像素不超过 400 万、单轴不超过 65500，JPEG 质量固定 92。使用已完成亮度调整和基础图叠加的合成像素做全图坐标面积平均，普通与流式输出共用有界累加器，不同条带大小不会改变结果。主图成功关闭后才保存预览；预览保存失败保留主图并返回非零退出码。关闭时不改动已有预览文件；与 `--tiled`、`--deepzoom` 组合会在生成前报参数错误。Web UI 和 REST API 暂不提供预览控制项，`--benchmark` 将其耗时列为 Encoding 的细分项。

建库默认保留解码宽高与目标完全一致素材的原始编码字节，保存独立副本，跳过缩放、裁剪和重编码，保留压缩数据及内嵌元数据。一次读取的原字节同时用于解码和保存，避免源文件变化导致特征错配；同一文件跳过自复制，显式格式转换仍编码。CLI、API、Web UI 的普通建库及 `--normalize-only` 共用此逻辑；同目录建库沿用已有处理方式。模板 CUDA 流水线复用解码像素并移动到有界队列；需要重编码的素材仍重读落盘结果，CPU/通用 CUDA 沿用有界分批读取。解码或保存失败会报告路径，不新增相应数据库记录。

尺寸不同的素材继续等比覆盖和中心裁剪，JPEG 建库编码质量由 90 提高到 **100**，PNG 压缩等级仍为 9；编码参数匹配支持大小写后缀。质量 100 仍采用 JPEG 编码，文件可能比旧版更大。同尺寸快速路径保留源压缩质量。特征版本仍为 **v6**，HTTP API 合约仍为 **2.0**，已有有效特征、记录 ID 和使用统计保留，无需迁移图库。去重依据实际保存图片的解码像素；旧版重压缩 JPEG 与新导入原图可能具有不同像素，再次导入可能新增记录。

CUDA 和 CPU-only 的完整 CTest 各覆盖 57 个核心/API 用例及 30 个服务/输出回归用例，包括预览开关、像素一致性、跨条带缩放、中文路径、失败处理、同尺寸文件字节一致、独立副本、去重及已有特征不重写。预览的历史配对测量不能解释为端到端提速；同尺寸归一化三次中位数采样显示收益依格式和工作负载变化，完整建库未显示稳定提速。归一化性能记录采样时 JPEG 重编码质量仍为 90，不作为本版质量 100 配置的性能结论。详细方法与数据见 [可选预览图验证](../verification/preview-verification.md)和 [同尺寸免重编码验证](../verification/same-size-normalization.md)。

> **EN**: Version 1.14.6 adds opt-in CLI JPEG previews and automatically preserves original encoded bytes for exactly sized library images. Preview generation shares bounded area averaging across regular and streaming single-image outputs, runs after composition, and preserves the completed main image on preview failure. Library imports keep independent copies and compute hashes/features from stored pixels. Resized JPEG imports now use quality 100, including uppercase extensions. Features remain v6 and API contract 2.0; old records and valid features remain compatible. Reimported original JPEGs may differ from previously recompressed copies. Historical timing results retain their original sampling and quality settings and do not promise universal speedups.


### v1.14.7: 三平台 CPU 发布与 CI 依赖优化 (2026-11-09)

正式发布增加 Windows x64、Linux x64、macOS ARM64 CPU 包，Windows CUDA 包继续由本机 GPU Required 模式验收；云端 Windows/Linux CUDA 构建只提供候选，不进入正式附件。macOS 不提供 Intel 包。三平台 CPU 包及本机正式 CUDA 包使用同一源码提交，保留已发布 v1.14.6 的标签和附件。

依赖工具与 ports 基线固定到 a0400024711b283056538ac19ced80b91a83c24c（OpenCV 4.12.0），仅启用 JPEG/PNG/TIFF/WebP、intrinsics 和 thread；依赖只构建 Release 动态库。日常 Windows/Linux CPU CI 共享按实际工具链和依赖配置生成的缓存，依赖成功后立即保存；CPU/CUDA 复用兼容缓存。冷、热缓存的单次对照分别为 Windows 12 分 11 秒/2 分 52 秒、Linux 6 分 28 秒/1 分 15 秒，完整分项与采样边界见 [CI 与打包](../../development/packaging.md)。

统一程序目录发现及 Unicode 路径，使用系统浏览器入口，随包收集运行依赖并修复 Unix 相对库路径；tar.gz 保留执行权限和符号链接，macOS 使用 ad-hoc 签名。发布脚本支持 ZIP/tar.gz，BUILD_INFO 记录 GPU 验收状态；无可用 GPU 的 Required 模式失败。无设备时评分工作区直接拒绝准备，避免无驱动环境的 CUDA 分配错误。产品 CLI、特征 v6、API 合约 2.0 和图库格式保持兼容。

> **EN**: Version 1.14.7 adds CPU release packages for Windows x64, Linux x64 and macOS ARM64, pinned minimal Release dependencies, shared binary caches and standalone archive verification. Formal Windows CUDA packages require local GPU validation; hosted Windows/Linux CUDA builds remain candidates. Native executable paths, Unicode handling, runtime deployment, relative loader paths and macOS ad-hoc signing support relocatable packages. Features remain v6, the API contract remains 2.0, and v1.14.6 stays unchanged.


### v1.14.8: 七架构 CUDA 与 Linux CUDA 发布 (2026-11-09)

默认通用 CUDA 包覆盖 sm_75（RTX 20/GTX 16）、sm_80（A100/A30）、sm_86（RTX 30）、sm_89（RTX 40）、sm_90（H100/H200）、sm_100（B200/GB200）、sm_120（RTX 50），每种目标同时包含对应 PTX。驱动优先选择兼容机器码，也可通过 PTX JIT 生成设备代码；保留 CUDA_ARCH 单目标覆盖，PTX 不使用 a/f 专用目标。四个 CUDA 模块共享参数，架构或编译器配置变化会重新编译；打包逐模块及最终程序检查七份 cubin 和七份 PTX，CLI/Web UI 各为 28 个 cubin、28 个 PTX。

任务在 GPU 工作前加载实际内核及四种模板实例，检查驱动、计算能力和启动资源限制。不可用时报告原因并使用 CPU；--cpu、CPU-only 和仅归一化不执行 GPU 内核预检。上传、计算和同步错误继续传播。Windows 无驱动入口先枚举设备，失败后只由主机代码格式化返回码，避免额外 CUDA 错误查询引起崩溃。模板算法、兼容评分算术、图库、特征 v6 与 API 合约 2.0 保持兼容。

正式附件增加 Linux x64 CUDA tar.gz；三平台 CPU 和 Linux CUDA 由同一提交在原生 CI 构建并独立验收。Windows CUDA 仍由本机 Required 模式完成实机 GPU 测试。Linux CUDA 保留 GpuValidation: not-run，在 Release 页面明确标注未做 Linux 实机 GPU 验收；新开关 AllowUnvalidatedCudaRelease 仅允许原生 Linux x64 的 BuildOnly tar.gz，不允许跳过测试、smoke 或归档验收，也不能用于 Windows。Windows 云端 CUDA 仍为 candidate。发布后在三个原生平台重新下载五个正式附件，验证标签、SHA256、版本、运行库和 Web UI/API。

本机 Ada 已通过禁用 PTX JIT、强制 PTX JIT 两种模式的完整 GPU 回归及 CPU-only 回归；私有 24 张素材、两种图库尺寸、三种执行模式共 24 轮采样的选图 ID 与输出像素一致，模板浮点差异符合既有容差。其他架构只完成编译覆盖，尚未实机验证；PTX JIT 仍要求驱动支持对应 PTX ISA。包内 BUILD_INFO 记录实际架构、PTX 与验收设备。性能和体积是特定本机样本，不承诺所有架构或场景提速，详见 [CUDA 验证报告](../../development/cuda.md)及 [七架构采样](../../performance/cuda-seven-architectures.json)。既有 v1.14.6/v1.14.7 标签和附件不变。

> **EN**: Version 1.14.8 ships native CUDA images and matching PTX for seven targets (75/80/86/89/90/100/120), checks real kernel compatibility before GPU work, and retains CPU fallback before execution. Runtime failures still propagate. Linux x64 CUDA joins the five formal platform/runtime archives with an explicit GPU-not-tested status; Windows CUDA still requires local hardware validation. Every archive uses the same source commit and receives standalone verification. Ada has native/JIT hardware coverage; other architectures have build coverage only. Features remain v6 and API contract 2.0.


### v1.14.9: 统一运行诊断与 Web UI 任务体验 (2026-11-09)

引擎、CLI、服务与任务管理共用运行诊断，失败点提供稳定原因码、字段和路径；阶段进度使用真实计数，未知总量不拼接整体百分比。归一化与特征处理分别展示，实际 CPU/CUDA 后端与回退原因可见。保留原退出码、任务状态与报告提交语义，主图成功关闭后才登记，后续预览或统计提交失败不隐藏已经保存的主图。

API 合约升级为 2.1，主兼容版本仍为 2，保留旧字段和端点。新增服务会话与工作目录、任务增量事件和按登记 ID 访问的产物。事件最多 256 条/256 KiB，UTF-8 安全截断；独立保留终止错误和警告汇总。最近 50 个完成任务只保留在服务内存，清理记录不删除文件。下载验证大小、修改时间与路径，固定缓冲流式发送，HTML 报告作为附件。

Web UI 沿用原参数页与命令生成器，新增任务列表、详情、恢复参数、断线退避重连、刷新恢复、会话变化提示及诊断复制/导出。仅取消排队任务；提交响应丢失不自动重发。结果区使用实际文件路径、格式、尺寸和匹配统计，可选有界 JPEG 预览默认关闭，分块与 DeepZoom 禁用预览。表单草稿和按用途分类的最近路径保存在浏览器本地，PowerShell 路径统一单引号引用并双写内部单引号。

源码构建流程统一以 README 为完整入口，CI 使用对应 Preset 显式构建产品和测试。核心引擎渐进拆出报告、性能统计打印及任务级邻域策略，保留浮点累加顺序、排序与随机数调用。未调整 CUDA 内核、评分权重、预算、特征 v6 或图库格式。功能验收中 Windows CPU/CUDA 完整 CTest、HTTP smoke 与八组严格对照通过；真实浏览器验证任务恢复、阶段计数、取消、回退、路径复用、预览和部分成功结果。诊断 JSON 的系统下载落盘未自动确认，其余具体边界见 [运行诊断验收](../verification/runtime-diagnostics-verification.md)和 [引擎整理验收](../verification/engine-refactor-verification.md)。发布包另按 [发布清单](../../development/releasing.md)执行原生构建、独立包与真实附件验收。

> **EN**: Version 1.14.9 adds shared runtime diagnostics, real stage counters, backend/fallback reporting and artifact registration. API 2.1 preserves major version 2 while adding session identity, bounded incremental events, task restoration and streamed artifact downloads. The existing Web UI gains task history/reconnection, queued cancellation, reusable paths, correct PowerShell quoting and opt-in bounded previews. Closed main images remain discoverable after later-stage failures. Engine report/metrics/neighborhood refactoring preserves algorithms, feature v6, library format and resource budgets.


### v1.14.10: Web UI 使用帮助与预览说明 (2026-11-09)

Web UI 的预览提示改为“生成结果预览默认关闭，勾选后额外保存一张用于快速查看的 JPEG 预览图”，并为复选框关联说明，方便识别默认状态与用途。预览生成条件、像素限制、编码质量和后端行为保持不变。

导航新增“使用帮助”，内容随单文件 HTML 一起提供，离线也能阅读。帮助涵盖首次建库、图库复用、路径填写、尺寸与格式、匹配参数、特征权重、各开关、建库选项、查询与管理、任务结果及常见问题。明确区分目标图匹配 Tile 与主图输出 Tile，并用尺寸例子说明最终主图的计算方式。预览选项旁可直接定位到开关说明；帮助目录可展开对应参数分组，快速开始按钮可返回建库或生成表单。

帮助页隐藏命令区，不生成或提交命令。切换页面保留表单参数、继续跟踪任务，刷新能恢复帮助页与同会话任务。取消和清理记录入口集中到任务区，按钮换行与导航横向滚动适应窄屏；窄屏参数表按条目上下排列，避免左右拖动阅读。README 同步增加帮助入口。

功能修改已通过真实浏览器检查：预览说明、帮助直达、参数保留、刷新恢复、帮助页任务跟踪、键盘展开、窄屏排版与服务断开后的阅读均正常；JavaScript 语法与帮助锚点检查通过。发布继续按清单执行同提交的原生打包、本机 GPU Required 和真实附件验收。CLI、REST API 2.1、特征 v6、图库格式及评分算法保持兼容。

> **EN**: Version 1.14.10 adds a self-contained Web UI help page covering setup, paths, options, task diagnostics and results. Preview wording explicitly names the option and explains its purpose. Context links open the relevant help section; form state and task tracking survive navigation, while help never generates or submits a command. Narrow screens use stacked option descriptions. API 2.1, feature v6, library formats and engine behavior remain unchanged.

### v1.16.0（候选）

- 增加 Vulkan/Metal 特征统计和快速评分，保留 CPU/CUDA 原实现与现有图库格式。
- API 1.5、设备发现与选择、评分精度、缓存身份和 GPU 诊断接通 CLI/Web UI。
- 固定验收集同时要求质量 mean/p90/p99 退化不超过 1%，以及相对 FP64 的最终选图差异不超过 1%。
- 新后端暂为显式选择的候选；实机与性能边界见 [四后端验收](../../development/portable-gpu.md)。

### v1.15.6: 原目标文件保留与更大缩放范围 (2026-11-12)

HTML 工程中的 JPG/JPEG、PNG 目标直接复制原文件，保留原名、全部字节、元数据及分辨率；其他格式保留文件主名，按原始宽高转为 PNG，不缩放或补边。`targetPreparation` 单独记录匹配时的目标尺寸与放大倍数，反射补边由网格及匹配块尺寸推导。原生回导复用既有预处理算法重建工作目标，HTML 按对应尺寸及反射边界显示目标参考和叠加，避免 JPEG 重复压缩及预处理图取代原图。缺少该字段的既有工程继续按已完成预处理的目标读取，再导出保留已有目标字节。

离线查看器和实时预览的初始图块高度保持 256 CSS 像素，最大缩放独立提高到 65536 CSS 像素；窗口加载、素材卸载及只读工程边界沿用原逻辑。图库、特征、ANN、缓存及恢复格式不变，API 能力版本仍为 1.4。功能阶段 Windows CUDA 构建、CTest 8/8、Web UI/API smoke，以及 Chromium/Firefox 的离线浏览、反射补边和最大缩放检查通过；详见 [工程验收](../verification/cli-projects-verification.md#目标源文件直接复制与原尺寸转换2026-11-12) 和 [缩放验收](../verification/html-viewport-check.md#最大缩放提高到-655362026-11-12)。正式包的同提交构建、GPU 与独立运行验收状态以发布审计及各包 `BUILD_INFO.txt` 为准。

> **EN**: Version 1.15.6 copies JPEG and PNG targets byte for byte, retaining their original names, metadata and resolution. Other formats convert to PNG at their original dimensions. Project metadata records matching resize and upscale settings so native rendering can rebuild the working target and reflected padding, while HTML displays the corresponding reference and overlay. Existing projects remain readable. Both viewers start at a tile height of 256 CSS pixels and allow zooming up to 65536 CSS pixels. Library formats and API capability version 1.4 remain unchanged.

### v1.15.5: 便携工程、命令树与 OverlayFS 兼容修复 (2026-11-12)

命令行按 `create`、`render`、`library`、`project` 组织，旧命令和旧选项不再解析；纯归一化使用 `library normalize`，追加建库使用 `library add`。API 能力版本为 1.4、主版本为 1。HTML 便携工程保存已用素材、排列和工作目标，可在原图库不可用时固定排列回导；当前工程不设置格式版本，也不兼容旧总览/缩略图工程。离线查看器与实时预览共用窗口逻辑，初始图块高度保持 256 CSS 像素，最大缩放独立提高到 16384 CSS 像素；提高上限不会改变默认打开尺度。从窗口中心扩散加载素材并释放窗口外位图，素材副本保留图库文件名。导出 HTML 顶部工具栏提供“项目地址”链接，在新标签页打开 GitHub 仓库，作品本身仍可离线浏览。命令迁移、能力与历史验证边界见 [CLI、便携工程与回导](../../guides/portable-projects.md) 及其验证文档。

目标工作图保留来源文件主名，JPG/JPEG 来源使用 JPEG，PNG 及其他来源使用 PNG；合法扩展名及大小写沿用输入。HTML 再导出复用已保存目标的字节，避免 JPEG 反复压缩；旧工程已有的 `target.png` 继续可用。

修复 POSIX 颜色重排将目录与图片 `st_dev` 不同直接视为跨卷的错误判断，允许支持硬链接的 OverlayFS 配置继续建库。实际跨文件系统仍安全失败，并报告源、目标路径；保留硬链接后删除、防覆盖、事务日志和恢复逻辑。Windows 卷号预检查不变，图库、特征、缓存及恢复格式版本不变，无需为此重建有效的 v1.15.0 起图库。代码修复阶段 Windows CPU CTest 8/8 通过；真实 OverlayFS 与 EXDEV 恢复手动验收尚未执行，不以普通文件系统测试替代，见 [修复验证记录](../verification/bugfix-verification.md#overlayfs-颜色重排兼容修复2026-11-12)。发布包的构建和 GPU 验收状态以各包 `BUILD_INFO.txt` 为准。

发布验收同时修复输出目录经过符号链接别名时的产物路径登记问题：在创建暂存目录前统一输出真实路径，避免 macOS 临时目录别名导致已完成的工程 ZIP 仍登记在已移除的暂存位置，保证完整工程归档可以下载；POSIX 回归覆盖目录别名下发布后的路径、大小与修改时间。

> **EN**: Version 1.15.5 introduces the grouped CLI and portable HTML projects with fixed-placement rendering, plus a viewport-based offline viewer. Both the offline viewer and live preview start at a tile height of 256 CSS pixels and support zooming up to 16384 CSS pixels. Old CLI options and old overview/thumbnail project layouts are not supported. API capability version is 1.4. POSIX color ordering no longer rejects OverlayFS images solely because their device IDs differ from the output directory. Hard-link moves, overwrite protection and recovery remain in place; real cross-filesystem moves are still unsupported. Output publication also resolves directory aliases so completed project archives remain downloadable on macOS. Library and feature formats remain compatible with v1.15.0 and later. Actual OverlayFS and EXDEV recovery experiments remain pending.

### v1.15.4: 实时预览进度条 (2026-11-12)

实时预览在 `GET /api/mosaic/preview/{id}` 中返回 `progress`，包含当前阶段和 `matching` 活动的 `completed`/`total`。Web UI 在预览窗口下方显示进度条：准备阶段使用不确定动画，匹配阶段显示百分比；状态文字改为当前阶段。

本次发布按维护者要求跳过全部测试：未运行 CTest、Web UI/API smoke、解压后验证或发布后附件验收，五个平台包只完成编译、打包和静态元数据校验。Windows 和 Linux CUDA 包均为 `GpuValidation: not-run`，不能视为通过 GPU 实机验收；API 1.2、特征编号 1 和图库格式不变。

> **EN**: Version 1.15.4 adds a live preview progress bar driven by the preview progress field. Per maintainer request, this release skipped all tests; CUDA packages are marked not-run and are not GPU-validated.

### v1.15.3: 十种 CUDA 模板与预览网格 (2026-11-12)

CUDA 模板从四种扩展到十种：正方形 256×256、512×512，竖屏 180×320、270×480、360×640、540×960，横屏 320×180、480×270、640×360、960×540。尺寸表同时驱动内核实例化、特征提取分派、GPU 预检以及 CLI 和使用帮助；其余合法尺寸继续使用通用 CUDA 路径。新增尺寸沿用原有模板算术，特征编号、图库格式和 API 1.2 不变。

实时预览取消 160,000 格总上限，后端返回完整匹配网格。前端按可见区域请求缩略图，可视窗口内超过 16,000 格时提示可能卡顿；新预览从画面中心以最大单格高度打开，仍可缩放、平移和适应窗口。预览匹配按秒输出控制台进度，并补充排队、缓存复用、ANN 准备和收尾日志。README 重写下载、快速开始、参数与模板说明，项目许可证改为 GPL-3.0-only；日常 CI 只编译产品，完整测试由版本标签触发的打包流程执行。

> **EN**: Version 1.15.3 expands template CUDA to ten resolutions (square 256/512, portrait 180/270/360/540 widths and landscape 320/480/640/960 widths), removes the 160,000-tile preview total cap while warning above 16,000 visible tiles, opens previews at the maximum tile height, and logs throttled preview progress. Licensing moves to GPL-3.0-only. Feature version 1, library format and API 1.2 remain compatible.

### v1.15.2 性能与预览体验 (2026-11-11)

实时预览复用目标格特征及基础分数，沿用顺序选图和固定种子。可用分数快照时，点击格子再计算最多 60 张推荐。缩略图优先加载可视窗口中央，平移复用画布重叠部分。素材选择面板支持按页码跳转，使用帮助补充了通用示例。性能对照见 [预览验收](../verification/preview-verification.md)。

流式 PNG 在拼贴时直接组装 RGB 行，沿用 libpng 的压缩策略。CPU 建库按批复用归一化像素，原尺寸文件保持原字节，重新编码的有损素材以实际写入的字节提取特征。兼容 API 1.2 和 v1.15.0 起的图库。

> **EN**: Version 1.15.2 reuses preview matching data, computes recommendations on demand, and reuses overlapping canvas pixels while panning. Streaming PNG builds RGB rows directly. CPU library builds pass normalized pixels to feature extraction in bounded batches.

### v1.15.1: 独立实时预览与手动选图 (2026-11-11)

Web UI 在生成与建库之间加入独立预览页。后端返回逐格选图，Canvas 按需加载可见素材，重复图片复用位图，窗口同时最多显示 16,000 格。支持缩放、平移、拖动调整窗口宽高及恢复默认尺寸。每格提供最多 60 张推荐候选，也可从整个图库按文件名或素材 ID 搜索并手动替换。

正式生成可绑定预览的选图与手动替换。目标、图库或选图参数变化时要求刷新，质量报告按实际输出的排列统计。API 增至 1.2，主版本为 1。预览页显示实际 CPU/CUDA 后端，模板 CUDA 复用批次预取，手动开启时直接发起请求。浏览器、输出对照及性能数据见 [预览验收](../verification/preview-verification.md)。

> **EN**: Version 1.15.1 adds a resizable interactive preview with viewport-only thumbnails, up to 60 recommendations per tile, and manual replacement from the full library. Export can reuse the edited placement. API 1.2 adds preview endpoints, and the Web UI shows the active CPU or CUDA backend.

### v1.15.0: 内部版本重置与兼容逻辑清理 (2026-11-10)

所有版本由 contracts.json 统一定义。图库使用 user_version=1，特征定义与原 v6 相同但编号为 1，API 为 1.0。移除路径迁移、自动补列、旧特征刷新、请求别名与 /api/run。旧库和历史证据保持原样，需要在新目录重新建库；不提供迁移器，不增加契约族或世代标识。路由、CLI 便捷参数、默认选图与 CPU/CUDA 能力保留。验证与回退说明见 [不兼容更新](breaking-1.15.0.md)。

### v1.14.11: 输出格式来源与超限适配 (2026-11-10)

单图输出按显式 `--format` / API `format`、输出文件后缀、输入文件后缀、PNG 的优先级确定格式。默认文件名改为无后缀的 `output/output`，最终追加实际后缀；有效显式格式覆盖冲突后缀，合法的 `.jpeg/.tif` 别名和大小写保持原拼写。输入后缀无法推导时回退 PNG，未知输出后缀在没有显式格式时明确报错。

自动推导的 JPG 或 WebP 超出各自单轴 65,500 / 16,383 上限时，改存 PNG 并保留完整尺寸；显式选择 JPG/WebP 时，通过统一比例缩小输出 Tile 并保留格式。尺寸计算使用 64 位整数；取整可能改变比例，网格本身超出编码上限时明确失败。PNG/TIFF 继续保留完整尺寸并执行原有整数及资源检查，分块和 DeepZoom 沿用原分片规则。匹配特征、网格、评分、随机调用和使用统计提交语义保持不变。

API 合约升至 2.2，主兼容版本保持 2，增加 `output-format-policy` 能力和可选 `runtime.outputPlan`，区分计划尺寸与已保存结果。格式回退和尺寸缩小通过稳定原因码发布警告；主图成功关闭后才登记实际产物。Web UI 展示格式来源、适配前后尺寸和真实文件名，自动模式不补充显式格式；恢复参数、命令生成、预览和报告共用同一份格式语义。生成结果预览继续默认关闭。

实现验收覆盖 Windows CPU/CUDA CTest、扩展 HTTP smoke、13 个 CLI 场景、八组模板/自定义尺寸严格基线，以及真实浏览器的提交、刷新、参数恢复、帮助和下载。固定种子回归比较完整 PNG 与缩小后的 JPG/WebP，确认选图及使用统计一致。详情见 [输出格式与超限适配验收](../verification/output-format-verification.md)；正式附件继续按发布清单执行同提交原生打包、本机 GPU Required 和真实下载验收。

> **EN**: Version 1.14.11 resolves output format from an explicit selection, output suffix, input suffix, then PNG. Inferred oversized JPG/WebP becomes full-size PNG; explicit JPG/WebP reduces output tile dimensions. API 2.2 and the Web UI expose the output plan separately from saved artifacts. Actual filenames, previews and reports remain consistent. Feature v6, matching algorithms, library format and usage-commit semantics are preserved.
