# Mosaicraft 项目百科全书

> 最后更新：2026-06-26 | 版本：v1.12.3

## 目录

1. [项目概述](#1-项目概述)
2. [架构设计](#2-架构设计)
3. [命令行完全参考](#3-命令行完全参考)
4. [性能数据](#4-性能数据)
5. [质量分析体系](#5-质量分析体系)
6. [输出格式行为](#6-输出格式行为)
7. [Bug 历史与经验教训](#7-bug-历史与经验教训)
8. [版本演进](#8-版本演进)
9. [构建与发布](#9-构建与发布)
10. [开发规范](#10-开发规范)
11. [已知限制与未来方向](#11-已知限制与未来方向)

## 目录

1. [项目概述](#1-项目概述)
2. [架构设计](#2-架构设计)
3. [命令行完全参考](#3-命令行完全参考)
4. [性能数据](#4-性能数据)
5. [输出格式行为](#5-输出格式行为)
6. [Bug 历史与经验教训](#6-bug-历史与经验教训)
7. [版本演进](#7-版本演进)
8. [构建与发布](#8-构建与发布)
9. [开发规范](#9-开发规范)
10. [已知限制与未来方向](#10-已知限制与未来方向)

---

## 1. 项目概述

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
- 发布包：4.8 MB zip，解压即用

---

## 2. 架构设计

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

## 3. 命令行完全参考

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
| `--output-tile <w> <h>` | 180 320 | 拼接图输出尺寸 |
| `--out-w <n>` | 原图 | 目标图缩放宽度 |
| `--out-h <n>` | 原图 | 目标图缩放高度 |
| `--upscale <n>` | 关闭 | 先放大原图 n× 再分块 |

#### 匹配控制

| 参数 | 默认 | 说明 |
|------|------|------|
| `--candidates <n>` | 200 | ANN 粗筛候选数 |
| `--topn-random <n>` | 3 | Top-N 随机选取 |
| `--lab-weight <f>` | 0.20 | LAB 权重 |
| `--grid-weight <f>` | 0.45 | Grid 权重 |
| `--tiny-weight <f>` | 0.25 | TinyImage 权重 |
| `--edge-weight <f>` | 0.05 | 边缘权重 |
| `--lbp-weight <f>` | 0.05 | LBP 权重 |
| `--penalty <f>` | 0.01 | 复用惩罚 |

#### 输出/工具

| 参数 | 说明 |
|------|------|
| `--tiled` | 分块输出 |
| `--deepzoom` | Deep Zoom + HTML |
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

## 4. 性能数据

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

## 5. 输出格式行为

| 格式 | 限制 | 超限行为 |
|------|------|----------|
| JPG (默认) | 65500px | 未显式→TIFF；显式`--format jpg`→等比缩放 |
| TIFF | 无限制 (BigTIFF) | — |
| WebP | **16383px** | 等比缩放 |
| PNG | 65500px | 自动 tiled |

输出路径扩展名自动推断格式：`-o result.tiff` → TIFF，无需 `--format`。

---

## 6. Bug 历史与经验教训

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

## 7. 版本演进

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
| 1.11.0 | HTML Report Pro, 打包/安装器, 配置文件 | 计划中 |
| 1.11.0 | 预设管理, 图库诊断增强 | 计划中 |
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

## 8. 构建与发布

### 依赖安装

```powershell
vcpkg install opencv4 sqlite3 tiff --triplet x64-windows
```

### 编译

```powershell
cmake -B build -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

### 发布打包

```powershell
# 收集文件
copy build\Release\*.exe release_pkg\
copy build\Release\*.dll release_pkg\
copy C:\Windows\System32\vcruntime140.dll release_pkg\
copy C:\Windows\System32\vcruntime140_1.dll release_pkg\
copy C:\Windows\System32\msvcp140.dll release_pkg\

# 压缩
Compress-Archive -Path release_pkg\* -Destination Mosaicraft_release.zip
```

发布包 4.8 MB，20 个文件，解压即用。需 NVIDIA 驱动支持 GPU 加速。

---

## 9. 开发规范

- C++17，CMake 3.20+，MSVC 2022
- 左花括号 `{` 另起一行
- `if` 语句显式使用 `{}`
- 关键逻辑加注释解释为什么
- Git 日志使用中文
- CUDA 代码注意整数除法截断，用 ceil 除法 `(N+M-1)/M`
- 修改评分/匹配逻辑后必须跑 `--analyze` A/B 对比

---

## 10. 已知限制与未来方向

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

## 11. v1.11 �� v1.12 ������־ (2026-06-26)

### v1.11: Web ����������
- ����̬ HTML: `tools/command-builder/index.html` ˫������
- 7 ��������ȫ���ǣ�46 �������� CLI ͬ��
- GitHub ��ɫ����

### v1.12: �ܹ��ع� + �����Ż�

**�������**
- PNG ��ʽд�� (PngStreamWriter): ����д�̣��ڴ�㶨 ~162KB
- JPG ��ʽд�� (JpgStreamWriter): libjpeg ���� API
- `--write-mode stream/batch/auto` ͳһ���� PNG/TIFF/JPG

**���ݿ�**
- DB �԰���: ��һ��Ŀ¼�ں� `mosaicraft.db`
- DB ������: `meta` ��洢 `feature_w/h`����������Ӧ
- `--normalize-size <WxH>` �Զ����һ���ֱ���
- GPU ��ֱ���: 180x320 / 320x180 / 360x640 / 640x360
- ��� tile �Զ����� DB �ߴ�(�����320x180, ������180x320)

**���� (45K ͼ��, RTX 4060)**
- ����: 30min �� 2.3min (13x ����)
- GPU_BATCH 32��256
- CPU/GPU ��ˮ�߲���
- FeaturePack ���̶߳�ȡ
- �����н� (MAX_QUEUE=512) �� OOM

**CLI �淶**
- `--output-tile WxH` ��������ʽ
- `--force`/`-y` ��ȫ���� + TTY ���
- �˳��� 0/1/2/3/4 + help �ĵ���
- 31 ������������ȷ�������ı�ͳһ
- Ĭ��ֵ: db=`library/mosaicraft.db`, output=`output/output.jpg`

**Web UI ������**
- `MosaicraftWebUI.exe`: cpp-httplib HTTP ����
- Web UI ����ʹ�ýṹ�� API, API �б���� `docs/API.md`
- `POST /api/run` 仅保留为旧命令兼容入口
- 30min ��ʱ, ���ֽڶ�ȡ, �˿ڳ�ͻ���

**Linux ֧�� (v1.13 Ŀ��)**
- �������� `isatty`/`getchar`/`unistd.h` �Ѳ�
- �����㷨��ƽ̨���� WSL/ԭ��ʵ��

**��׳��**
- GPU cudaMalloc ȫ����鷵��ֵ
- `--normalize-size`/`--output-tile`/`--write-mode` ����У��
- `\r` ������� + ����̨��ͣ(AutoPause)
- ��һ��ѹ���������� (JPEG q90, PNG lv9)

### v1.12.3: ����� MOSAICRAFT_CUDA
- `cmake -D MOSAICRAFT_CUDA=OFF` �� CPU ����
- ������������� CUDA ͷ�ļ�/��������
- `core/CudaStubs.cpp`: �� CUDA ʱ�Ŀ�ʵ��
- Windows/Linux ������
