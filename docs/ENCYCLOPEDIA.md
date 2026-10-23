# Mosaicraft 项目百科全书 / Project Encyclopedia

> 最后更新 / Last updated：2026-10-23 | 版本 / Version：v1.14.5
>
> English readers: each major section begins with a brief English summary. The detailed technical reference, logs, and code examples are primarily in Chinese — the project's working language.

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

当前四种固定尺寸 180×320、320×180、360×640、640×360 使用模板 CUDA 提取和评分；其他合法尺寸（宽高各 1～8192）使用与原 CPU 精确兼容的通用 CUDA 特征统计和候选评分，保留 CPU 的邻域处理、排序、选图及分析。提取与评分独立准备资源，计算前资源不足只回退相应能力；v6 特征及数据库格式不变，旧自定义尺寸图库无需重建。详见 [动态尺寸特征验证](DYNAMIC_CUDA_FEATURES.md)及 [独立 CUDA 评分验证](INDEPENDENT_CUDA_SCORING.md)。

> **EN**: Fixed sizes retain template CUDA extraction and scoring. Other valid dimensions (1–8192 per axis) use CPU-compatible generic CUDA feature statistics and candidate scoring, retaining CPU neighborhood processing, sorting, selection and analysis. Extraction and scoring prepare resources independently; shortages before computation fall back only for the affected capability. Features remain v6; existing custom-size libraries need no rebuild.

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
- 发布包：~5 MB zip，解压即用（含 EXE + DLL + index.html）

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

> **EN**: `mosaicraft build -i <dir>` builds a library. `mosaicraft mosaic -i <img> -d <db>` generates a mosaic. `mosaicraft inspect -i <img>` inspects an image. Full flag reference below.

### build

```
mosaicraft build -i <dir> [-o <dir>] [-d <path>] [-t <n>]
```

| 参数 | 默认 | 说明 |
|------|------|------|
| `-i --input <dir>` | 必填 | 图库图片目录 |
| `-o --output <dir>` | `normalized` | 归一化输出目录 |
| `-d --db <path>` | `mosaicraft.db` | 数据库路径 |
| `-t --threads <n>` | 自动 | 归一化线程数 |
| `--append` | — | 追加图片后按颜色重排全库 |
| `-r --recursive` | — | 递归扫描子目录 |
| `--normalize-only` | — | 仅归一化不建库（初筛用） |

### mosaic

```
mosaicraft mosaic -i <img> -d <db> [-o <path>] [选项...]
```

#### 输入输出

| 参数 | 默认 | 说明 |
|------|------|------|
| `-i --input <path>` | 必填 | 目标图 |
| `-d --db <path>` | `mosaicraft.db` | 数据库 |
| `-o --output <path>` | `mosaic.jpg` | 输出路径（扩展名推格式） |
| `--format <ext>` | 扩展名 | `jpg`/`png`/`webp`/`tiff` |
| `--quality <n>` | 95 | JPEG/WebP 质量 |

#### tile 控制

| 参数 | 默认 | 说明 |
|------|------|------|
| `--tile-w <n>` | 9 | 目标图每格宽度 |
| `--tile-h <n>` | 16 | 目标图每格高度 |
| `--output-tile <WxH>` | 180x320 | 拼接图输出尺寸 |
| `--out-w <n>` | 原图 | 目标图缩放宽度 |
| `--out-h <n>` | 原图 | 目标图缩放高度 |
| `--upscale <n>` | 关闭 | 先放大原图 n× 再分块 |

#### 匹配控制

| 参数 | 默认 | 说明 |
|------|------|------|
| `--candidates <n>` | 150 | ANN 粗筛候选数（105K 图库 sweep：150≈200≈300，最佳平衡点） |
| `--topn-random <n>` | 10 | Top-N 随机选取（多样性) |
| `--lab-weight <f>` | 0.20 | LAB 权重 |
| `--grid-weight <f>` | 0.45 | Grid 权重 |
| `--tiny-weight <f>` | 0.25 | TinyImage 权重 |
| `--edge-weight <f>` | 0.05 | 边缘权重 |
| `--lbp-weight <f>` | 0.05 | LBP 权重 |
| `--penalty <f>` | 0.01 | 使用频率惩罚（usePenalty） |
| `--neighbor-penalty <f>` | 100.0 | 邻域重复惩罚（越大越多样） |
| `--neighbor-window <n>` | 0=auto | 邻域窗口大小（≥2×tilesX） |
| `--l-range <f>` | 20 | ANN 候选的 L 亮度差上限（0..255） |

#### 输出/工具

| 参数 | 说明 |
|------|------|
| `--tiled` | 分块输出 |
| `--deepzoom` | Deep Zoom + HTML |
| `--write-mode <mode>` | 写入模式：`auto`/`stream`/`batch` |
| `--png-level <n>` | PNG 压缩级别 (1-9，默认 1) |
| `--color-adjust` | 选图后对每个小块施加对称随机 LAB L 扰动（默认关闭） |
| `--color-strength <f>` | L 通道最大相对扰动，范围 `0..0.5`，默认 `0.04`（`±4%`） |
| `--overlay-strength <f>` | 基础图透明叠加强度，范围 `0..1`，默认 `0`（关闭） |
| `--adaptive-weights` | 🧪 自适应权重（实验） |
| `--cpu` | 强制 CPU |
| `--benchmark` | 耗时分解 |
| `--analyze` | 匹配质量报告 + 热力图 |

单独设置强度不会启用亮度调整。每个输出小块从 1001 个等概率档位中抽取乘数，范围为 `1±strength`（默认 `0.96～1.04`），平均值为 1；仅统一缩放 LAB L 通道，A、B 通道保持不变。强度为 0 时保持原像素。乘数分布对称，但像素取整、高亮截断和颜色转换仍可能影响最终图像的平均亮度。

基础图透明叠加使用 `素材 × (1 − overlayStrength) + 基础图 × overlayStrength`，在随机亮度调整后执行。参考为尺寸调整、upscale 和镜像补边后的匹配工作图；使用全图像素中心坐标逐块双线性采样，只有参考图外边界夹紧，避免块间插值接缝。普通、流式、分块及 DeepZoom 共用合成入口，DeepZoom 先合成源小图再生成金字塔。强度 `0` 跳过额外采样和复制；开启时保留工作图并为最多 8 个线程分配小块缓冲，不创建完整输出尺寸的参考图。素材缓存只读，各位置修改独立副本。选图和特征版本保持不变，分析分数仍表示叠加前的匹配质量。验证数据见 [基础图叠加验证](BASE_IMAGE_OVERLAY_VERIFICATION.md)。

### inspect

```
mosaicraft inspect -i <img> [-d <db>]
```

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

> **EN**: JPG (max 65,500px) and WebP (max 16,383px) reject oversized single images. PNG and TIFF preserve dimensions up to the signed 32-bit axis limit. Output extension determines format unless explicitly selected.

| 格式 | 限制 | 超限行为 |
|------|------|----------|
| JPG (默认) | 65500px | 报错，请选择 PNG/TIFF/分块输出 |
| TIFF | 单轴 `INT_MAX` (BigTIFF) | 报错 |
| WebP | **16383px** | 报错，请选择 PNG/TIFF/分块输出 |
| PNG | 单轴 `INT_MAX` | 报错 |

输出路径扩展名自动推断格式：`-o result.tiff` → TIFF，无需 `--format`。

超大输出默认允许，无需额外开关；PNG/TIFF 可使用流式写入降低内存占用。工作图像最多 1 亿像素。程序不再自动切换格式或缩小尺寸；显式格式会调整文件后缀。v6 特征与兼容性变化见 [修复验证记录](BUGFIX_VERIFICATION.md)。

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

> **EN**: Windows build with CMake + vcpkg (OpenCV, SQLite3, libtiff). CUDA optional via `MOSAICRAFT_CUDA=OFF`. Release package: ~4.8 MB zip.

### 依赖安装

```powershell
vcpkg install opencv4 sqlite3 tiff --triplet x64-windows
```

### 编译

```powershell
# GPU 加速 (默认，需 CUDA Toolkit)
cmake -B build -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release

# 纯 CPU 编译
cmake -B build -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake -DMOSAICRAFT_CUDA=OFF
cmake --build build --config Release
```

编译后自动复制 `index.html` + CUDA DLL (`cudart64_*.dll`) 到 `build/Release/bin/`。

### 发布前验证

```powershell
# 单元/API 合约测试 + 服务层回归测试
cmake --build build --config Release --target mosaicraft_tests mosaicraft_regression_tests
ctest --test-dir build -C Release --output-on-failure

# Web UI/API 真实 smoke：启动本地 HTTP 服务，提交 build/mosaic job，检查输出
cmake --build build --config Release --target mosaicraft_webui_smoke
```

### 发布打包

```powershell
# 发布前环境检查，不构建、不打包
.\scripts\release.ps1 -InspectOnly -BuildDir build -Configuration Release

# 默认 CUDA Release 发布：配置、构建、CTest、WebUI smoke、打包、解压验证、SHA256
.\scripts\release.ps1 -BuildDir build -Configuration Release

# CI/无 CUDA 环境：生成 CPU-only 发布候选包
.\scripts\release.ps1 -BuildDir build-ci -Configuration Release -NoCuda -PackageSuffix ci-cpu

# 发布后验收 GitHub Release 上真实可下载的 zip
.\scripts\verify-release-asset.ps1 -Tag v<version>
```

发布脚本会生成 `Mosaicraft_v<version>_<platform>-<arch>_<runtime>.zip`。当前正式 Windows CUDA 包名为 `Mosaicraft_v<version>_windows-x64_cuda.zip`；CI CPU-only 候选包名为 `Mosaicraft_v<version>_windows-x64_cpu-only_ci-cpu.zip`。正式打包要求 git 工作区干净。历史更新说明统一维护在本文件中，打包时会复制为包内的 `ENCYCLOPEDIA.md`。脚本会复制 EXE、DLL、`index.html`、`README.md`、`API.md`、`LICENSE`、`ENCYCLOPEDIA.md`、`BUILD_INFO.txt` 和第三方版本清单，并验证：

- 包内 CLI `--version` 与项目版本一致；
- 包内 `BUILD_INFO.txt` 的版本、包名和 git commit 与当前源码一致；
- 包内 `MosaicraftWebUI.exe` 可启动；
- Web UI/API smoke 可提交 build/mosaic job；
- SHA256 可直接用于发布校验。

`verify-release-asset.ps1` 是发布后的真实附件验收入口。它从 GitHub Release 下载 zip，校验 asset digest、Release 页面 SHA256 和文件 SHA256，解压检查包结构和文档策略，确认包内 `BUILD_INFO.txt` 的 commit 匹配 GitHub tag，并使用解压后的 `MosaicraftWebUI.exe` 跑 Web UI/API smoke。这个步骤用于发现“本地包通过，但 Release 页面附件不是同一个产物”的问题。

发布包 ~5 MB，解压即用。需 NVIDIA 驱动支持 GPU 加速（CPU fallback 可用 `MOSAICRAFT_CUDA=OFF` 编译）。Windows CPU-only CI 门禁位于 `.github/workflows/ci.yml`，复用同一个发布脚本生成 `windows-x64_cpu-only_ci-cpu` 候选包。

正式发布以 `docs/RELEASE_CHECKLIST.md` 为准：本机 CUDA 发布包用于正式附件，远端 CPU-only CI 用于基础质量门禁。GitHub hosted CI 当前不验证 CUDA；如需自动化 CUDA 发布，应配置自托管 Windows GPU runner。

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
- 默认值: db=`library/mosaicraft.db`, output=`output/output.jpg`

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

两阶段重命名、数据库路径事务、持久化恢复清单和跨进程图库锁共同保护图片与索引。图片 ID、使用统计及特征引用保持稳定，新特征文件使用内容哈希命名。未索引的冲突文件不会被覆盖，已索引图片跨卷移动会失败。实际验证和样本耗时见 [颜色排序验证](COLOR_ORDER_VERIFICATION.md)。

匹配特征 v6 统一 CPU/CUDA 的 8×8 分区、Canny 边缘定义、统计字段及评分尺度，去重使用落盘图像像素哈希。旧图库需运行 `build --append` 刷新特征并完成颜色重排；图片字节、ID 和使用统计保持对应，选图结果可能与旧特征版不同。还修复重复建库误删、中文特征及索引路径、特征包 ID 校验、数据库损坏检查与并发使用统计、取消竞态，以及 PNG/DeepZoom/TIFF 的失败处理。超大输出默认允许，超出编码器或整数范围时明确报错。具体用例与兼容边界见 [边界与一致性修复验证](BUGFIX_VERIFICATION.md)。

验收使用实际 CUDA 和 CPU-only 构建、核心及服务回归、Web UI/API smoke。45,625 张现有图库重排后，连续编号为 `000000`–`045624`，灰度区 146 张、彩色区 45,479 张；图片与独立特征 SHA256、ID、使用统计和特征引用保持一致，重复运行补算 0 张、改名 0 张。颜色缓存算法版本为 2，与软件版本和匹配特征版本分别管理。

> **EN**: Version 1.14.2 reuses stored mean LAB for default HSV v2 ordering and renumbers indexed files after appends. Chroma below 0.5 enters grayscale; 15-degree hue bands start at 345 degrees and alternate lightness direction. Color caches retain source means; independent CPU/CUDA builds can differ, while repeated ordering of one database is stable. Locks and recoverable two-phase renames preserve image bytes, IDs, statistics and feature references. Matching features v6 align backend definitions; old libraries need `build --append` to refresh. HTTP API contract 2.0 tightens JSON and origin checks and returns synchronous mosaic results. See the verification records for compatibility changes, fault checks and the 45,625-image library acceptance.


### 第一轮缓存与性能更新（版本号保持 1.14.2）

CPU/GPU 共用独立特征文件的 Unicode 读取和来源校验。缓存的 `features.pack` 清单指向同代次的 TinyImage、LBP 和二进制校验元数据；绑定记录 ID、特征版本、图片内容哈希及来源文件的路径、大小、修改时间和数据摘要。未经认证的旧 v2 包回退来源文件，建库时自动升级；新代次完整落盘后才切换清单，失败保留旧清单。无变化追加复用有效缓存，旧代次只在建库独占锁下清理。

颜色重排复用本次路径信息，输出目录卷号只读取一次，路径、别名、目标冲突、跨卷和恢复检查保留。CPU 特征按最多 256 张、64 MiB 待处理像素分批并行计算，去重与数据库写入按扫描顺序进行，事务失败回滚整批；同目录输入采用单张批次，以兼容不同尺寸和超预算图片。CUDA 目标块准备和 Canny 补算各复用最多 8 个默认工作线程，计算公式不变。

大图库在 Windows 批量枚举特征目录的文件属性，减少逐文件系统调用；失败或未命中项仍查询原文件属性。建库日志分别记录颜色校验、排序与文件/SQL 写入，缓存校验/来源读取与写入，以及包含扫描和缓存的墙钟总耗时。流水线阶段存在重叠，不能直接相加。内存和性能验收见 [第一轮验收记录](PERFORMANCE_PHASE1.md)。

> **EN**: This update keeps version 1.14.2, matching features v6 and API contract 2.0. Both backends share Unicode source reads, cache validation and missing-feature penalties. Immutable cache generations publish through one manifest; unverified v2 packs fall back to source files and builds upgrade them automatically. Unchanged builds reuse valid packs and color checks reuse path information. CPU feature batches retain scan-order database commits and roll back failed transactions. CUDA preparation and Canny use reusable worker pools. Timings separate validation, writing and wall time; see the phase-one report for measured performance and memory limits.

### v1.14.3: 缓存一致性、有界匹配与对称亮度调整 (2026-10-14)

此版本发布两轮缓存与性能优化及无偏对称亮度调整，匹配特征仍为 **v6**，HTTP API 合约仍为 **2.0**，生产参数保持兼容。

- CPU/CUDA 共用来源校验和 Unicode 特征读取；旧包、损坏包及来源变化回退独立特征文件。新缓存按完整代次原子发布，失败保留旧包，无变化追加复用有效缓存。缺失特征保留评分惩罚。
- CPU 建库采用最多 256 张、64 MiB 待处理像素的并行批次，按扫描顺序去重并提交事务；颜色重排复用路径信息和 SQL，CUDA 准备与 Canny 使用线程池。详细数据见 [第一轮验收记录](PERFORMANCE_PHASE1.md)。
- 匹配主机缓冲及 CPU 特征缓存预算各为 256 MiB，每批最多 1024 块，按任务需要分配。任务内复用特征读取会话、预载/LRU、ANN、线程池和 CUDA 工作区；预算容许时提前准备一批。选图结果保存图库索引，分析仅保留最差 20 块的详细特征。预算不代表进程总内存上限。
- 亮度调整继续默认关闭；开启后使用 1001 档均匀分布，LAB L 乘数改为 `1±colorStrength`，默认 `0.04` 对应 `0.96～1.04`。与此前 `0.988～1.028` 相比，扰动更宽且乘数平均值为 1；强度 0 保持原像素。像素取整、高亮截断和颜色转换仍可能影响最终平均亮度。CLI、Web UI 和 API 说明已同步。

本机固定 160,000 块样本的 CUDA 总耗时中位数由 43.800 s 降至 33.168 s（约 24.3%），其他场景的收益及内存口径见 [第二轮验收记录](PERFORMANCE_PHASE2.md)。常规 CPU/CUDA 样本分别通过索引、输出哈希及解码像素比较；CUDA 大样本仍存在基线自身的浮点累加顺序与近似同分选图波动，不保证该场景逐字节一致。亮度调整关闭及零强度的 1600 块样本在两种后端中均与调整更新前完全一致。

> **EN**: Version 1.14.3 ships validated feature-cache generations, bounded parallel library builds and bounded matching with task-local caches and reusable workspaces. Host matching buffers and CPU feature payload caches each have a 256 MiB budget, with at most 1024 tiles per batch; these are not process memory limits. Features remain v6 and the HTTP API remains 2.0. Optional brightness jitter remains off by default and now samples 1001 unbiased symmetric LAB L multipliers in `1±strength`; default strength 0.04 spans 0.96–1.04, and zero strength preserves pixels. The fixed local 160,000-tile CUDA benchmark improved by about 24.3%; the reports retain scene-specific results and the existing floating-point near-tie reproducibility limitation.

### v1.14.4: 基础图透明叠加 (2026-10-15)

新增 `--overlay-strength` / API `overlayStrength`，范围为有限数 `0..1`，默认 `0` 关闭。设置大于零的强度即可在选图后混合基础图，例如 `0.2` 为 80% 素材与 20% 基础图。Web UI 同步提供叠加强度输入框和命令生成；同步与异步生成接口均支持。匹配特征仍为 **v6**，HTTP API 合约仍为 **2.0**，无需重建图库。

参考图沿用匹配流程的尺寸调整、upscale 和镜像补边，按全图像素中心坐标逐块双线性采样，跨块读取相邻像素，只有参考图外边界夹紧。普通、流式、分块和 DeepZoom 共用合成入口，DeepZoom 先合成源小图再生成金字塔。素材缓存保持只读，各位置修改独立副本；同时启用随机亮度调整时，先调整素材亮度，再叠加基础图。缺失素材沿用失败计数和背景，分析报告注明分数衡量选图质量，而非最终合成画面。

CUDA 和 CPU-only 构建、完整 CTest、Web UI/API smoke、实际 CUDA 匹配及 24 次样图运行均已验证。关闭叠加的固定样本输出与 1.14.3 完全一致；同条件 PNG 样本的 20% 叠加耗时中位数约 13.37 s，原版约 13.38 s，此结果不代表其他图库或格式的性能。叠加仅保留目标工作图和每个工作线程的小块缓冲，不分配完整输出尺寸的基础图。输出面积相差四倍的两档样本中，新增工作线程缓冲预算均约 2.66 MiB；预算不代表进程总内存上限。详细方法、数据及效果边界见 [基础图叠加验证](BASE_IMAGE_OVERLAY_VERIFICATION.md)。

> **EN**: Version 1.14.4 adds optional base-image overlay through `--overlay-strength` and API `overlayStrength`, with range 0–1 and default 0 (off). The Web UI, synchronous API and asynchronous jobs expose the same setting. Overlay blends the processed working target after matching and optional brightness jitter. Global-coordinate bilinear sampling is shared by regular, streaming, tiled and DeepZoom output; source tiles are composited before pyramid generation. Cached pixels remain immutable and matching features v6/API contract 2.0 stay compatible. Fixed default-output samples match 1.14.3 exactly. Validation covers CPU-only and actual CUDA matching, output paths, parameter bounds and 24 image runs; reported performance and memory budgets are scene-specific.

### v1.14.5: 动态尺寸 CUDA 与提取、评分独立加速 (2026-10-23)

本版本发布 CUDA 传输流水线优化、通用尺寸特征统计与 CPU 兼容 CUDA 候选评分。四种固定模板保留原有内核算术，通过任务内锁页缓冲、异步传输与 CPU Canny 重叠、设备特征复用减少重复工作。其他合法尺寸（宽高各 1～8192）默认尝试通用 CUDA 提取和兼容评分，无需增加 CLI/API 参数。特征版本仍为 **v6**，HTTP API 合约仍为 **2.0**，现有 v6 图库无需重建。

自定义尺寸继续使用原 OpenCV 预处理，GPU 归约 LAB、Grid 和 LBP 的整数统计，CPU 按原运算顺序完成特征收尾；兼容评分严格保持原 CPU 算术、缺失特征惩罚与使用次数惩罚。CPU 保留 ANN、邻域处理、排序、随机选图和分析。相同标准构建/OpenCV 环境、输入、图库快照、使用计数和随机种子下，特征、分数及最终选图保持精确一致；不承诺跨 OpenCV 版本一致。

提取和评分独立准备资源、独立回退。两者的设备暂存合计不超过 256 MiB，常驻图库单独计量；按预算缩小批次。计算前资源不足可回退对应 CPU 能力，上传、内核和同步故障则收束并报错。`--cpu`、无 CUDA 和 CPU-only 构建保持纯 CPU。修复原评分工作区显存余量重复扣减。

CUDA/CPU-only CTest、两套 Web UI/API smoke、真实 GPU 精确回归及 Compute Sanitizer 已完成，动态特征和兼容评分内存检查均为 0 错误。两项新增能力合并后的直接对比使用私有 96 张图库、25,600 块：256×256 普通/分析总耗时减少 21.5% / 18.2%，512×512 普通减少 8.0%、分析基本持平；1600 块的 256×256 短任务反而慢 10.3%～13.6%。这些是固定本机样本的完整生成结果，不能用评分内核倍数推算整个生成过程，也不承诺所有场景提速。详见 [v1.14.5 性能与兼容说明](PERFORMANCE_v1.14.5.md)、[传输流水线验证](CUDA_TRANSFER_PIPELINE.md)、[动态特征验证](DYNAMIC_CUDA_FEATURES.md)和 [独立评分验证](INDEPENDENT_CUDA_SCORING.md)。

> **EN**: Version 1.14.5 ships reusable asynchronous CUDA transfers, generic feature statistics for valid custom dimensions, and independently selected CPU-compatible CUDA scoring. Existing v6 libraries, CLI/API parameters and API contract 2.0 remain compatible. Custom-size preprocessing, scoring arithmetic, missing-feature penalties and CPU selection semantics are preserved under the same build and inputs. Resource shortages before submission fall back independently; runtime failures abort. A direct 96-image, 25,600-tile comparison reduced total generation time by 18.2–21.5% at 256×256 and 8.0% for normal generation at 512×512, while 512×512 analysis was essentially unchanged. Short 256×256 tasks became 10.3–13.6% slower. See the report for exact baselines, full samples and scope.
