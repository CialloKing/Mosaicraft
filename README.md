# Mosaicraft v1.10.0

GPU 加速的照片马赛克拼贴生成器。将数千张图片建立特征索引，目标图分割为小格后逐格匹配图库中最相似的图片，拼接输出超大分辨率马赛克。

## 快速开始

```powershell
mosaicraft build -i ./photos -o ./normalized -d ./lib.db   # 建库（一次）
mosaicraft mosaic -i target.jpg -d ./lib.db -o output.jpg  # 生成
```

## 特性

- **Grid-first 匹配** — 8×8 Grid (192维) 贡献 58%，空间加权提升 7.4%
- **五层特征** — AvgLAB + 8×8 Grid + TinyImage + 边缘密度 + LBP
- **ANN + GPU 四层检索** — hnswlib 粗筛 → GPU 精排 → Selection 去重 → 16 线程贴图
- **GPU 特征提取** — 复用 featureKernel 批量处理, Features 96s→2.2s (43×)
- **BigTIFF** — libtiff 直写，突破 65500px，实测 50040×75200
- **质量分析** — `--analyze` 量化报告 + 热力图 + HTML 报告 + 最差 tile 导出
- **图库诊断** — `db-stats` 亮度直方图 + 覆盖缺口；`db-usage` 全局热点图统计
- **数据库管理** — `db-purge` 减量清理；`build --append` 增量建库；`build --recursive` 递归扫描
- **FeaturePack** — 50K 文件 → 2 次 fread，加载 <300ms
- **智能格式** — 默认 JPG，超限自动 TIFF/等比缩放
- **参数自适应** — candidates 默认 150，neighborWindow O(√N) 动态缩放
- **Deep Zoom** — 金字塔 + HTML viewer
- **Unicode 路径** — 中文或其他语言文件名

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
mosaicraft build    -i <dir> [-o <dir>] [-d <db>] [--append] [-r] [--normalize-only]
mosaicraft mosaic   -i <img> -d <db> [-o <path>] [选项]
mosaicraft inspect  -i <img> [-d <db>]
mosaicraft db-stats -d <db>
mosaicraft db-purge -d <db>
mosaicraft db-usage -d <db> [-n <N>] [--export <dir>]
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

**默认**：9×16 tile、180×320 拼接图、150 候选 ANN→GPU 精排、JPG 输出。

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
| `--candidates` | 150 | ANN 候选数 |
| `--neighbor-window` | auto | 邻域窗口（默认 O(√N) 动态） |
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
| PNG | **无限制** | 空闲内存不足自动流式 |

### 使用统计

```powershell
mosaicraft db-usage -d ./lib.db            # 查看 Top 50 热点图
mosaicraft db-usage -d ./lib.db -n 100     # Top 100
mosaicraft db-usage -d ./lib.db --export ./hot  # 导出全部使用过的图
```

记录每次马赛克中各图片的出现次数（Runs = 出现过的项目数，Tiles = 累计 tile 使用量）。

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

## 性能表现 (46K 图库, RTX 4060)

| 目标图 | 原图 | Tiles | 输出 | Score | Total |
|--------|------|-------|------|-------|-------|
| target5.png | 1134×1712 | 13,482 | 22680×34240 JPG | 0.110 | 19s |
| target10.png | 1503×2286 | 22,879 | 30060×43840 JPG | 0.099 | 36s |
| target8.jpg | 1584×2224 | 24,464 | 31680×44480 JPG | **0.092** | 40s |
| target3.jpg | 1593×1836 | 20,355 | 31860×36800 JPG | 0.094 | 29s |
| target4.jpg | 1782×3008 | 37,224 | 35640×60160 JPG | 0.108 | 53s |
| target7.jpg | 2007×3008 | 41,924 | 40140×60160 JPG | 0.141 | 60s |
| target.png | 2500×3750 | 65,330 | 43368×65330 JPG* | 0.121 | 82s |
| target9.jpg | — | 86,925 | 39345×65265 JPG* | 0.097 | 100s |
| target6.png | — | 93,600 | 38064×65400 JPG* | 0.098 | 106s |
| target2.png | — | 93,900 | 35100×65417 JPG* | 0.105 | 105s |

`*` = JPG 超 65500px 自动等比缩放。平均 1.3ms/tile。

## 项目结构

```
Mosaicraft/
├── src/main.cpp              # CLI (build/mosaic/inspect/db-stats/db-purge/db-usage)
├── core/                     # CPU 核心
│   ├── MosaicEngine          # 主引擎 (生成+分析+HTML报告)
│   ├── Database              # SQLite 读写 + 使用统计
│   ├── FeatureIndex          # hnswlib ANN 索引
│   ├── FeatureUtils.h        # Grid/Tiny/Edge/LBP 特征计算
│   ├── FeaturePack.h         # 二进制特征缓存 (tiny.bin/lbp.bin)
│   ├── ImageCache.h          # 贴图 LRU 缓存 (16 分片)
│   ├── BigTiffWriter.h       # libtiff BigTIFF 写出
│   ├── DeepZoomWriter.h      # Deep Zoom 金字塔
│   └── ImageNormalizer       # 图片归一化
├── compute/                  # CUDA 加速
│   ├── CudaBackend.cu        # GPU 评分 + 特征提取
│   └── FeatureExtractorCuda.cu # GPU 建库特征提取
├── docs/
│   └── ENCYCLOPEDIA.md       # 项目百科全书
├── tests/
│   └── regression.ps1        # 回归测试
├── CMakeLists.txt
└── README.md
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
| v1.6 | candidates sweep (150最优)、CPU=GPU | ✅ |
| v1.7 | GPU Features (43×加速)、sqrt移除 | ✅ |
| v1.8 | 增量建库、动态Window、HTML报告 | ✅ |
| v1.9 | db-health 健康度诊断 | ✅ |
| v1.10 | WorstTile对照、--unused、Unicode全链路 | ✅ |
| v2.0 |  GUI | 计划中 |

## 许可证

GPL — 详见 [LICENSE](LICENSE)
