# Mosaicraft

GPU 加速的照片马赛克拼贴生成器。

将数千张图片建立特征索引，把目标图分割为小格，每格从图库中匹配最相似的图片，拼接输出马赛克效果图。

## 特性

- **五层特征匹配** — AvgLAB + 4×4 Grid + 16×16 TinyImage + 边缘密度 + LBP 纹理
- **ANN 近似最近邻** — hnswlib 索引，O(log n) 查询，恒定 200 候选
- **GPU 加速** — CUDA kernel 并行计算候选距离（RTX 4060 / sm_89）
- **自动退化** — 无 GPU 时静默切换 CPU，零配置
- **SQLite 索引** — 标量存库、大特征存文件，支持 10 万+ 图库
- **Deep Zoom 输出** — 生成多级金字塔 + `.dzi` 清单 + 内嵌 OpenSeadragon HTML viewer，双击浏览
- **分块输出** — `--tiled` 每 tile 独立文件，无 JPEG 65500px 尺寸限制
- **局部颜色校正** — 逐 tile 随机微调亮度/饱和度，减少重复感
- **Unicode 路径** — 日文/中文文件名原生支持
- **Linux 风格 CLI** — `build` / `mosaic` / `inspect` 子命令

## 依赖

| 依赖 | 用途 |
|------|------|
| OpenCV 4.x | 图像处理 |
| SQLite 3 | 特征索引 |
| CUDA Toolkit（可选） | GPU 加速 |
| CMake 3.20+ | 构建 |
| MSVC / GCC | 编译器 |

通过 vcpkg 管理：

```bash
vcpkg install opencv4 sqlite3 --triplet x64-windows
```

## 构建

```bash
git clone git@github.com:CialloKing/Mosaicraft.git
cd Mosaicraft

# CMake 配置（使用 vcpkg toolchain）
cmake -B build \
  -DCMAKE_TOOLCHAIN_FILE=<vcpkg-root>/scripts/buildsystems/vcpkg.cmake

# 编译
cmake --build build --config Release
```

无 CUDA 时，CMake 会自动跳过 GPU 后端。

## 使用

### 建库

```bash
mosaicraft build -i ./photos -o ./normalized -d ./lib.db
```

| 参数 | 说明 |
|------|------|
| `-i, --input` | 图片目录（必填） |
| `-o, --output` | 归一化输出目录（默认 normalized） |
| `-d, --db` | 数据库路径（默认 mosaicraft.db） |
| `-t, --threads` | 线程数（默认自动） |

### 拼接

```bash
mosaicraft mosaic -i target.jpg -d ./lib.db -o output.jpg
```

| 参数 | 默认 | 说明 |
|------|------|------|
| `-i, --input` | — | 目标图（必填） |
| `-d, --db` | mosaicraft.db | 数据库路径 |
| `-o, --output` | mosaic.jpg | 输出路径 |
| `--tile-w` | 9 | tile 宽度 |
| `--tile-h` | 16 | tile 高度 |
| `--out-w/h` | 原图 | 输出分辨率（0=auto） |
| `--candidates` | 200 | 粗筛候选数（ANN→GPU 精筛） |
| `--lab-weight` | 0.20 | LAB 颜色权重 |
| `--grid-weight` | 0.45 | Grid4x4 结构权重 |
| `--tiny-weight` | 0.25 | TinyImage 纹理权重 |
| `--edge-weight` | 0.05 | 边缘密度权重 |
| `--lbp-weight` | 0.05 | LBP 纹理权重 |
| `--penalty` | 0.01 | 使用次数惩罚 |
| `--topn-random` | 1 | 从 Top-N 随机选取（>1 增加多样性） |
| `--quality` | 95 | JPEG 质量 1-100 |
| `--tiled` | — | 分块输出（每 tile 独立文件） |
| `--deepzoom` | — | 生成 Deep Zoom 金字塔 + HTML viewer |
| `--no-color-adjust` | — | 禁用颜色微调 |
| `--color-strength` | 0.10 | 颜色微调强度 0-0.5 |
| `--benchmark` | — | 打印各阶段耗时分解 |
| `--cpu` | — | 强制 CPU |

### 查看特征

```bash
mosaicraft inspect -i image.jpg [-d lib.db]
```

输出目标图的 AvgLAB、边缘密度、LBP 熵、数据库覆盖情况、亮度分布统计。

### 示例

```bash
# 标准生成（分块 + Deep Zoom，输出后双击 .html 即可浏览）
mosaicraft mosaic -i photo.jpg -d lib.db -o result --tiled --deepzoom

# 快速预览
mosaicraft mosaic -i photo.jpg -d lib.db \
  --out-w 640 --out-h 480 --tile-w 45 --tile-h 80

# 高多样性 + 启动 benchmark
mosaicraft mosaic -i photo.jpg -d lib.db \
  --topn-random 5 --benchmark
```

## 性能概况

以下数据基于 **25,034 张图库**、RTX 4060、Release 构建，`--benchmark` 实测。

### 阶段耗时（602 tiles, 1920×1080 输出）

| 阶段 | 耗时 | 占比 | 备注 |
|------|------|------|------|
| Prep (DB + GPU 加载) | 1100-1400 ms | 71-89% | 🔴 主要瓶颈：50K 个小文件 open() |
| Features (多线程提取) | 140-160 ms | 9-10% | |
| Placement (贴图) | 220-270 ms | 14-17% | |
| ANN 构建+查询 | ~10 ms | <1% | ✅ 已优化完毕 |
| GPU 评分 | 3-4 ms | <1% | ✅ 吞吐 40-136M scores/s |
| Selection | ~0 ms | ~0% | |

### candidates 对性能的影响

| candidates | GPU 评分 | GPU 吞吐 | 总时间 |
|-----------|---------|----------|--------|
| 200 | 3.0 ms | 40M scores/s | ~1550 ms |
| 500 | 3.8 ms | 80M scores/s | ~1760 ms |
| 1000 | 4.4 ms | 136M scores/s | ~1550 ms |

**结论：candidates 已经是纯质量参数，提高至 1000 对总时间几乎无影响。**

### 当前瓶颈

```
启动阶段的 50,000 次小文件 open()/close()
  ↓
25K .tiny (256B) + 25K .hist (1024B) = 31 MB 数据
  ↓
瓶颈不在数据量（31MB SSD 顺序读 < 10ms）
   在文件句柄风暴（50K 次 syscall ≈ 1000ms+）
```

ANN、GPU 评分、特征提取均已优化至总时间 1-10% 以内，继续投入这些方向收益极低。

## 项目结构

```
Mosaicraft/
├── core/
│   ├── ImageNormalizer     归一化 180×320
│   ├── BatchProcessor      多线程批处理
│   ├── Database            SQLite 索引
│   ├── FeatureExtractor    V1~V4 特征提取
│   ├── FeatureIndex        HNSW 近似最近邻索引
│   ├── MosaicEngine        马赛克生成引擎
│   ├── DeepZoomWriter      Deep Zoom 金字塔 + HTML viewer
│   ├── FeatureUtils        特征距离计算
│   └── UnicodeIO           Unicode 路径支持
├── compute/
│   ├── CudaBackend.h       CUDA 接口
│   ├── CudaBackend.cu      GPU kernel
│   └── FeatureExtractorCuda.cu  GPU 特征提取
├── src/
│   └── main.cpp            CLI 入口 (build / mosaic / inspect)
└── CMakeLists.txt
```

## 路线图

| 版本 | 内容 | 状态 |
|------|------|------|
| v0.4 | inspect 命令、特征可视化 | ✅ |
| v0.5 | ANN 候选选择 (hnswlib) | ✅ |
| v0.6 | Deep Zoom HTML viewer、颜色校正、benchmark、topn-random CLI | ✅ |
| v0.7 | **Feature Pack Cache** — 50K 小文件 → 2 个二进制缓存，Prep 3337→291ms, Total 5.6x | ✅ |
| v1.0 | 百万图库支持、稳定版 | 计划中 |

## 许可证

GPL — 详见 [LICENSE](LICENSE)
