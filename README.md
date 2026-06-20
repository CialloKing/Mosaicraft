# Mosaicraft

GPU 加速的照片马赛克拼贴生成器。将数千张图片建立特征索引，目标图分割为小格后逐格匹配图库中最相似的图片，拼接输出超大分辨率马赛克。

## 快速开始

```powershell
mosaicraft build -i ./photos -o ./normalized -d ./lib.db   # 建库（一次）
mosaicraft mosaic -i target.jpg -d ./lib.db -o output.jpg  # 生成
```

## 特性

- **Grid-first 匹配** — 8×8 Grid (192维) 贡献 69%，空间加权提升 7.4%
- **五层特征** — AvgLAB + 8×8 Grid + TinyImage + 边缘密度 + LBP
- **ANN + GPU 双层检索** — hnswlib 粗筛 → CUDA 精排
- **BigTIFF** — libtiff 直写，突破 65500px，实测 50040×75200
- **质量分析** — `--analyze` 量化报告 + 热力图 + 最差 tile 导出
- **图库诊断** — `db-stats` 亮度直方图 + 覆盖缺口检测
- **FeaturePack** — 50K 文件 → 2 次 fread，加载 <300ms
- **智能格式** — 默认 JPG，超限自动 TIFF/缩放
- **Deep Zoom** — 金字塔 + HTML viewer
- **Unicode 路径** — 日文/中文文件名

## 安装

```powershell
vcpkg install opencv4 sqlite3 tiff --triplet x64-windows
```

## 构建

```powershell
git clone https://github.com/CialloKing/Mosaicraft.git
cd Mosaicraft
cmake -B build -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

## 使用

### 命令一览

```
mosaicraft build    -i <dir> [-o <dir>] [-d <db>]       建库
mosaicraft mosaic   -i <img> -d <db> [-o <path>] [选项]  生成
mosaicraft inspect  -i <img> [-d <db>]                   查看特征
mosaicraft db-stats -d <db>                              图库诊断
```

### 建库

```powershell
mosaicraft build -i ./photos -o ./normalized -d ./lib.db
```

归一化→特征提取→SQLite+FeaturePack+ANN 索引全部自动完成。

### 生成马赛克

```powershell
mosaicraft mosaic -i target.jpg -d ./lib.db -o output.jpg
```

**默认**：9×16 tile、180×320 拼接图、200 候选 ANN→GPU 精排、JPG 输出。

#### 常用选项

```powershell
# 更快预览
mosaicraft mosaic -i p.jpg -d lib.db -o out.jpg --out-w 640 --out-h 480

# 超大目标 → 自动 TIFF
mosaicraft mosaic -i p.jpg -d lib.db

# 更高密度（同分辨率 4× tile）
mosaicraft mosaic -i p.jpg -d lib.db --upscale 2 --output-tile 90 160

# 质量分析
mosaicraft mosaic -i p.jpg -d lib.db --analyze

# 性能诊断
mosaicraft mosaic -i p.jpg -d lib.db --benchmark
```

#### 完整参数

| 参数 | 默认 | 说明 |
|------|------|------|
| `-i --input` | 必填 | 目标图 |
| `-d --db` | mosaicraft.db | 数据库 |
| `-o --output` | mosaic.jpg | 输出（扩展名推格式） |
| `--tile-w/h` | 9/16 | tile 格尺寸 |
| `--output-tile <w> <h>` | 180 320 | 拼接图输出尺寸 |
| `--out-w/h` | 原图 | 目标图缩放到指定分辨率 |
| `--upscale <n>` | 关闭 | 先放大原图 n× 再分块 |
| `--format` | 扩展名 | jpg/png/webp/tiff |
| `--quality` | 95 | JPEG/WebP 质量 |
| `--candidates` | 200 | ANN 候选数（30≈200，可降至 100） |
| `--topn-random` | 3 | Top-N 随机（代价仅 2%） |
| `--lab/grid/tiny/edge/lbp-weight` | 0.20/0.45/0.25/0.05/0.05 | 特征权重 |
| `--penalty` | 0.01 | 复用惩罚 |
| `--color-adjust` | 关闭 | LAB L 微调 |
| `--adaptive-weights` | 🧪关闭 | 自适应权重（实验） |
| `--tiled` | — | 分块输出 |
| `--deepzoom` | — | Deep Zoom + HTML |
| `--cpu` | — | 强制 CPU |
| `--benchmark` | — | 耗时分解 |
| `--analyze` | — | 质量报告 + 热力图 + 最差 tile |

### 输出格式

| 格式 | 限制 | 超限行为 |
|------|------|----------|
| JPG (默认) | 65500px | 未显式→TIFF；显式`--format jpg`→等比缩放 |
| TIFF | 无限制 | — |
| WebP | 16383px | 等比缩放 |
| PNG | 65500px | 自动 tiled |

### 图库诊断

```powershell
mosaicraft db-stats -d ./lib.db
```

输出图库规模、特征维度、LAB 分布、亮度直方图、覆盖缺口（暗光/色偏）。

### 质量分析

```powershell
mosaicraft mosaic -i p.jpg -d lib.db --analyze
```

输出：分数统计(P50/P90/P99)、特征贡献%、ANN 召回率、复用统计、Grid 8×8 贡献热图、空间权重代码、最差 20 tile 导出(PNG)、热力图。

## 性能 (25K 图库, RTX 4060)

| 目标图 | Tiles | 输出 | 耗时 |
|--------|-------|------|------|
| target2.jpg | 8,446 | 18540×26240 JPG | ~18s |
| target6.jpg | 15,142 | 15120×24000 JPG | ~20s |
| target.png | 65,330 | 50040×75200 TIFF | ~34s |

## 项目结构

```
Mosaicraft/
├── src/main.cpp              # CLI
├── core/                     # CPU 核心
│   ├── MosaicEngine          # 主引擎
│   ├── FeatureExtractor      # 特征提取
│   ├── FeatureIndex (HNSW)   # ANN 索引+持久化
│   ├── FeatureUtils          # 距离计算 (含空间权重)
│   ├── FeaturePack           # 二进制缓存
│   ├── BigTiffWriter         # BigTIFF
│   ├── DeepZoomWriter        # 金字塔+HTML
│   ├── ImageCache            # 分片缓存
│   ├── ImageNormalizer       # 归一化
│   ├── Database              # SQLite
│   └── hnswlib/              # HNSW (vendored)
├── compute/                  # CUDA
│   ├── CudaBackend           # GPU 评分
│   └── FeatureExtractorCuda  # GPU 特征提取
└── docs/ENCYCLOPEDIA.md      # 项目百科全书
```

## 路线图

| 版本 | 内容 | 状态 |
|------|------|------|
| v0.7 | FeaturePack + BigTIFF + ANN 持久化 | ✅ |
| v0.8 | ImageCache + 8×8 Grid + 智能格式 | ✅ |
| v1.0 | --upscale、--output-tile、WebP 支持 | ✅ |
| v1.1 | --analyze 质量评估体系 | ✅ |
| v1.4 | Spatial Weight Map (Score -7.4%) | ✅ |
| v1.5 | db-stats 覆盖诊断 | ✅ |
| v2.0 | GUI、增量建库、百万图库 | 计划中 |

## 许可证

GPL — 详见 [LICENSE](LICENSE)
