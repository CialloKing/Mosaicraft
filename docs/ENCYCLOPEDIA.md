# Mosaicraft 项目百科全书 / Project Encyclopedia

> 最后更新 / Last updated：2026-07-05 | 版本 / Version：v1.13.9
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

> **EN**: Two-stage retrieval: ANN (hnswlib) narrows 25K images → 200 candidates, then GPU kernel scores all five features (LAB, 8×8 Grid, TinyImage, Edge, LBP) to pick the winner. Spatial weight map and neighbor deduplication resolve visual quality. Storage uses SQLite + FeaturePack binary cache.

### 2.1 双层检索

```
Tile 特征 (708维)
     ↓
 ANN (hnswlib)     ← LAB + Grid + Edge (约200维有效信号)
     ↓
 Top 200 候选
     ↓
 GPU kernel        ← 五特征全量评分 (708维 + Tiny 256 + LBP 256)
     ↓
 邻域去重 + Top-N随机
     ↓
 最终选择
```

设计理由：ANN 负责缩小搜索空间（25K→200），GPU 负责精确排序。Tiny/LBP 在 ANN 阶段零填充（仅占索引维度 91%，但粗筛仍有效），完整评分留在 GPU 端。

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
    └── lib.ann          # HNSW 图索引 (~几十MB)

lib.db                   # SQLite (avgLAB, Grid BLOB, 路径, 元数据)
```

FeaturePack v2 二进制格式：每条记录显式存储 `int32_t image_id` + 特征数据，解决 ID 不连续问题。

### 2.4 处理流程

```
Prep (~300ms)
├─ FeaturePack 加载 (2次 fread 替代 50K 文件 I/O)
├─ ANN 索引加载 (loadIndex)
├─ GPU 库构建 + 上传
└─ 缓存命中 → 300ms；首次 → ~1400ms

Features (~800ms)
├─ 16线程并行
├─ ROI resize → 180×320 (匹配DB)
├─ computeGrid8x8 (64格)
├─ computeTinyImage (16×16)
├─ computeEdgeDensity
└─ computeLBPHistogram

ANN 查询 (~12s)
├─ 单线程 buildTileVector + hnswlib searchKnn
├─ image_id → allRecords 索引映射
└─ 瓶颈：hnswlib visited pool 非线程安全，无法并行

GPU 评分 (~60ms)
├─ scoreBatchKernel (一次 kernel 处理全部 tile×candidate)
├─ 五特征加权 + 使用惩罚
└─ 30M+ scores/s

Placement (~15s)
├─ 16线程 ImageCache (16分片锁)
├─ cv::Mat clone → colorAdjust (可选) → copyTo
└─ 瓶颈：copyTo 内存带宽 (11.2GB 写入)

Selection (~200ms)
├─ 邻域频率惩罚 (滑动窗口 ≥2×tilesX)
└─ Top-N 随机选取 (默认 Top-3)
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
| `--append` | — | 增量建库（仅新增图片） |
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
| `--l-range <f>` | 0.03 | L 通道微调范围 |

#### 输出/工具

| 参数 | 说明 |
|------|------|
| `--tiled` | 分块输出 |
| `--deepzoom` | Deep Zoom + HTML |
| `--write-mode <mode>` | 写入模式：`auto`/`stream`/`batch` |
| `--png-level <n>` | PNG 压缩级别 (1-9，默认 3) |
| `--color-adjust` | LAB L 微调（默认关闭） |
| `--color-strength <f>` | 微调强度 (0.04) |
| `--adaptive-weights` | 🧪 自适应权重（实验） |
| `--cpu` | 强制 CPU |
| `--benchmark` | 耗时分解 |
| `--analyze` | 匹配质量报告 + 热力图 |

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
| candidates 30≈200 | → 默认保持 200（安全余量） | — |
| Top3 Random 代价 2.1% | → 保留默认 | — |
| 图库暗光 0.2% | → `db-stats` 诊断 | 补夜景图 |
| Adaptive Weights 退步 | → 冻结为实验选项 | — |
| Edge 密度在 tile 上恒为 0 | → 9×16→180×320 上采样抹平边缘 | 已知限制 |

---

## 5. 输出格式行为 / Output Formats

> **EN**: JPG (max 65,500px), TIFF (unlimited / BigTIFF), WebP (max 16,383px), PNG (max 65,500px, auto-tiled). Output extension determines format.

| 格式 | 限制 | 超限行为 |
|------|------|----------|
| JPG (默认) | 65500px | 未显式→TIFF；显式`--format jpg`→等比缩放 |
| TIFF | 无限制 (BigTIFF) | — |
| WebP | **16383px** | 等比缩放 |
| PNG | 65500px | 自动 tiled |

输出路径扩展名自动推断格式：`-o result.tiff` → TIFF，无需 `--format`。

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
```

发布脚本会生成 `Mosaicraft_v<version>_<platform>-<arch>_<runtime>.zip`。当前正式 Windows CUDA 包名为 `Mosaicraft_v<version>_windows-x64_cuda.zip`；CI CPU-only 候选包名为 `Mosaicraft_v<version>_windows-x64_cpu-only_ci-cpu.zip`。历史更新说明统一维护在本文件中，打包时会复制为包内的 `ENCYCLOPEDIA.md`。脚本会复制 EXE、DLL、`index.html`、`README.md`、`API.md`、`LICENSE`、`ENCYCLOPEDIA.md` 和第三方版本清单，并验证：

- 包内 CLI `--version` 与项目版本一致；
- 包内 `MosaicraftWebUI.exe` 可启动；
- Web UI/API smoke 可提交 build/mosaic job；
- SHA256 可直接用于发布校验。

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

> **EN**: Known limits: WebP 16,383px cap, single-threaded ANN, Edge density washed out at tile scale. Roadmap: incremental build, face/saliency weighting, GUI.

### 已知限制

- WebP 限制 16383px（低于 JPEG 65500px）
- ANN 查询单线程（hnswlib visited pool 非线程安全）
- Edge 密度在 9×16→180×320 上采样后恒为 0
- 自适应权重当前版退步，已冻结
- 无增量建库（需全量重建）

### 已验证结论

- candidates=200 足够（ANN 召回率 100%）
- topNrandom=3 代价仅 2.1%
- Grid 8×8 是核心竞争力（贡献 69%）
- 颜色校正 LAB L 通道方式正确但默认关闭（视觉差异有限）

### 未来方向

| 优先级 | 方向 |
|--------|------|
| P1 | Analyze 增强（分类细分、最差 tile 定位） |
| P1 | 增量建库 |
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
- GPU 多分辨率: 180x320 / 320x180 / 360x640 / 640x360
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
