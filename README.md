# Mosaicraft

GPU 加速的照片马赛克拼贴生成器。将数千张图片建立特征索引，目标图分割为小格后逐格匹配图库中最相似的图片，拼接输出超大分辨率马赛克。

## 快速开始

```powershell
# 1. 准备图库（假设图片在 ./photos 目录）
mosaicraft build -i ./photos -o ./normalized -d ./lib.db

# 2. 生成马赛克
mosaicraft mosaic -i target.jpg -d ./lib.db -o output.jpg
```

两步即可。首次 build 需 5-10 分钟（25000 张图），之后每次 mosaic 只需几十秒。

## 特性

- **8×8 LAB Grid** — 64 格空间特征，192 维向量
- **五层特征匹配** — AvgLAB + 8×8 Grid + 16×16 TinyImage + 边缘密度 + LBP 纹理
- **ANN 近似最近邻** — hnswlib 索引 + 持久化，O(log n) 查询
- **GPU 加速** — CUDA kernel 并行评分（RTX 4060 / sm_89），吞吐 30M+ scores/s
- **BigTIFF 输出** — libtiff 直写，突破 OpenCV 65500px 限制，实测 50040×75200
- **智能格式切换** — 默认 JPG；超 65500px 自动 TIFF；显式 `--format jpg` 超限等比缩放
- **Deep Zoom 输出** — 多级金字塔 + `.dzi` + 内嵌 OpenSeadragon HTML viewer
- **FeaturePack 缓存** — 50K 小文件合并为 2 个二进制文件，加载 <300ms
- **ImageCache 内存缓存** — 热门图片只解码一次
- **自动退化** — 无 GPU 时静默切换 CPU
- **Unicode 路径** — 日文/中文文件名原生支持
- **Linux 风格 CLI** — `build` / `mosaic` / `inspect` 子命令

## 依赖与安装

### 环境要求

| 依赖 | 用途 | 安装 |
|------|------|------|
| OpenCV 4.x | 图像处理 | vcpkg |
| SQLite 3 | 特征索引 | vcpkg |
| libtiff | TIFF/BigTIFF 输出 | vcpkg (OpenCV 自带) |
| CUDA Toolkit 12.x | GPU 加速（可选） | [NVIDIA 官网](https://developer.nvidia.com/cuda-downloads) |
| CMake 3.20+ | 构建 | [cmake.org](https://cmake.org) |
| MSVC 2022 | 编译器 | Visual Studio |

### 安装 vcpkg 依赖

```powershell
# 安装 vcpkg（如未安装）
git clone https://github.com/Microsoft/vcpkg.git
cd vcpkg
.\bootstrap-vcpkg.bat
.\vcpkg integrate install

# 安装依赖
.\vcpkg install opencv4 sqlite3 --triplet x64-windows
```

## 构建

```powershell
git clone https://github.com/CialloKing/Mosaicraft.git
cd Mosaicraft

cmake -B build `
  -DCMAKE_TOOLCHAIN_FILE=<vcpkg-root>/scripts/buildsystems/vcpkg.cmake

cmake --build build --config Release
```

编译产物在 `build/Release/mosaicraft.exe`。无 CUDA 时 CMake 自动跳过 GPU 后端。

## 使用指南

### 第一步：准备图库

图库建议 5000 张以上，图片内容越丰富、马赛克效果越好。支持 `.jpg` `.png` `.webp` `.bmp` `.tiff` 格式，不限制分辨率——程序会自动归一化到 180×320。

```
photos/
├── beach.jpg
├── sunset.png
├── portrait.jpg
└── ... (越多越好)
```

### 第二步：建库

```powershell
mosaicraft build -i ./photos -o ./normalized -d ./lib.db
```

这步会：
1. 归一化所有图片到 180×320（等比缩放 + 中心裁剪）
2. 提取五层特征（AvgLAB、8×8 Grid、TinyImage、边缘密度、LBP）
3. 存入 SQLite 数据库 + FeaturePack 二进制缓存
4. 构建 ANN 索引（`lib.ann` 文件，下次 mosaic 秒级加载）

| 参数 | 说明 |
|------|------|
| `-i, --input <dir>` | 图片目录（必填） |
| `-o, --output <dir>` | 归一化输出目录（默认 `normalized`） |
| `-d, --db <path>` | 数据库路径（默认 `mosaicraft.db`） |
| `-t, --threads <n>` | 归一化线程数（默认自动） |

产出物：

```
normalized/          # 归一化图片（180×320 JPG）
  ├── 000001.jpg
  ├── 000002.jpg
  └── features/
      ├── tiny.bin   # TinyImage 缓存 (N×256B)
      ├── lbp.bin    # LBP 直方图缓存 (N×1024B)
      └── lib.ann    # ANN 索引（持久化）
lib.db               # SQLite 特征数据库
```

### 第三步：生成马赛克

```powershell
mosaicraft mosaic -i target.jpg -d ./lib.db -o output.jpg
```

**默认行为：**
- tile 尺寸 9×16（与归一化 180×320 等比）
- 200 候选 ANN 粗筛 → GPU 精排
- 输出 JPG 大图
- 颜色校正默认关闭（`--color-adjust` 开启）

#### 常用场景

```powershell
# 快速预览（限制输出尺寸）
mosaicraft mosaic -i photo.jpg -d lib.db -o preview.jpg --out-w 640 --out-h 480

# 高质量 TIFF 输出（无损，支持超大尺寸）
mosaicraft mosaic -i photo.jpg -d lib.db -o result.tiff --format tiff

# 超大数据量目标——自动切换 TIFF 或等比缩放
mosaicraft mosaic -i huge.png -d lib.db -o output.jpg
# → 超 65500px 时自动：默认→TIFF；--format jpg→等比缩放

# 显示各阶段耗时（性能分析）
mosaicraft mosaic -i photo.jpg -d lib.db -o output.jpg --benchmark

# 增加马赛克多样性（Top-N 随机选取）
mosaicraft mosaic -i photo.jpg -d lib.db -o output.jpg --topn-random 3

# 只用 CPU（无 GPU 或调试用）
mosaicraft mosaic -i photo.jpg -d lib.db -o output.jpg --cpu
```

### 查看图库信息

```powershell
mosaicraft inspect -i photo.jpg -d ./lib.db
```

输出目标图的 AvgLAB、边缘密度、LBP 熵、数据库覆盖情况和亮度分布。

### 完整参数列表

| 参数 | 默认 | 说明 |
|------|------|------|
| `-i, --input` | 必填 | 目标图路径 |
| `-d, --db` | `mosaicraft.db` | 数据库路径 |
| `-o, --output` | `mosaic.jpg` | 输出路径（扩展名自动推断格式） |
| `--tile-w` | 9 | tile 宽度（默认 9×16 = 归一化 180×320 等比） |
| `--tile-h` | 16 | tile 高度 |
| `--out-w` | 原图 | 目标缩放宽度（0=不缩放） |
| `--out-h` | 原图 | 目标缩放高度 |
| `--format` | 自动检测 | `jpg` / `png` / `webp` / `tiff` |
| `--candidates` | 200 | ANN 粗筛候选数 |
| `--topn-random` | 1 | Top-N 随机（>1 增加多样性） |
| `--lab-weight` | 0.20 | LAB 颜色权重 |
| `--grid-weight` | 0.45 | Grid 结构权重 |
| `--tiny-weight` | 0.25 | TinyImage 纹理权重 |
| `--edge-weight` | 0.05 | 边缘密度权重 |
| `--lbp-weight` | 0.05 | LBP 纹理权重 |
| `--penalty` | 0.01 | 重复使用惩罚 |
| `--quality` | 95 | JPEG/WebP 质量 1-100 |
| `--color-adjust` | 关闭 | 逐 tile 亮度/饱和度微调 |
| `--color-strength` | 0.10 | 微调强度 0-0.5 |
| `--tiled` | — | 分块输出 |
| `--deepzoom` | — | Deep Zoom 金字塔 + HTML |
| `--benchmark` | — | 打印各阶段耗时 |
| `--cpu` | — | 强制 CPU |

### 输出格式行为

| 场景 | 行为 |
|------|------|
| 默认，输出 ≤65500px | → JPG |
| 默认，输出 >65500px | → 自动 TIFF |
| `--format jpg`，超限 | → 等比缩放 tile |
| `--format tiff` | → 无限制（BigTIFF） |
| `--format png/webp`，超限 | → 自动 tiled |

## 性能

以下基于 **25,034 张图库**、RTX 4060、8×8 Grid、Release 构建实测。

| 目标图 | Tiles | 输出分辨率 | 格式 | 耗时 |
|--------|-------|-----------|------|------|
| target2.jpg (921×1300) | 8,446 | 18540×26240 | JPG | ~33 s |
| target3.png (~1736×2456) | 29,722 | 34740×49280 | JPG | ~26 s |
| target.png (2500×3750) | 65,330 | 43368×65330 | JPG (缩放) | ~90 s |
| target.png (2500×3750) | 65,330 | 50040×75200 | TIFF | ~34 s |

### 阶段耗时 (8446 tiles, 18540×26240)

| 阶段 | 耗时 | 说明 |
|------|------|------|
| Prep (FeaturePack + GPU 加载) | ~300 ms | 缓存命中 |
| Features (16 线程提取) | ~800 ms | resize + cvtColor + Grid + LBP |
| ANN 查询 | ~12 s | 单线程 hnswlib searchKnn |
| GPU 评分 | ~60 ms | 30M+ scores/s |
| Placement | ~15 s | ImageCache + copyTo |

## 项目结构

```
Mosaicraft/
├── src/main.cpp                       # CLI 入口
├── core/
│   ├── MosaicEngine.cpp/h             # 主引擎
│   ├── FeatureExtractor.cpp/h         # CPU 特征提取
│   ├── FeatureIndex.h                  # HNSW ANN 索引 + 持久化
│   ├── FeatureUtils.h                  # 特征距离计算
│   ├── FeaturePack.h                   # 二进制特征缓存
│   ├── BigTiffWriter.h                 # BigTIFF 直写 (libtiff)
│   ├── DeepZoomWriter.h               # 金字塔 + HTML viewer
│   ├── ImageCache.h                    # 分片内存缓存
│   ├── ImageNormalizer.cpp/h           # 归一化 180×320
│   ├── BatchProcessor.cpp/h            # 多线程批处理
│   ├── Database.cpp/h                  # SQLite
│   ├── UnicodeIO.h                     # Unicode 路径
│   └── hnswlib/hnswalg.h/...           # HNSW (vendored)
├── compute/
│   ├── CudaBackend.cu/h               # GPU 评分 kernel
│   └── FeatureExtractorCuda.cu/h      # GPU 特征提取
└── CMakeLists.txt
```

## 路线图

| 版本 | 内容 | 状态 |
|------|------|------|
| v0.5 | ANN 候选选择 (hnswlib) | ✅ |
| v0.6 | DeepZoom HTML、颜色校正、benchmark | ✅ |
| v0.7 | FeaturePack 缓存 + BigTIFF + ANN 持久化 | ✅ |
| v0.8 | ImageCache + 8×8 Grid + 智能格式切换 | ✅ |
| v1.0 | 百万图库、GUI、增量建库 | 计划中 |

## 许可证

GPL — 详见 [LICENSE](LICENSE)
