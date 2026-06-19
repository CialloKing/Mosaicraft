# Mosaicraft

GPU 加速的照片马赛克拼贴生成器。

将数千张图片建立特征索引，把目标图分割为小格，每格从图库中匹配最相似的图片，拼接输出马赛克效果图。

## 特性

- **五层特征匹配** — AvgLAB + 4×4 Grid + 16×16 TinyImage + 边缘密度 + LBP 纹理
- **GPU 加速** — CUDA kernel 并行计算候选距离（RTX 4060 / sm_89）
- **自动退化** — 无 GPU 时静默切换 CPU，零配置
- **SQLite 索引** — 标量存库、大特征存文件，支持 10 万+ 图库
- **Unicode 路径** — 日文/中文文件名原生支持
- **Linux 风格 CLI** — `build` / `mosaic` 子命令

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
| `--tile-w/h` | 32 | tile 尺寸 |
| `--out-w/h` | 原图 | 输出分辨率 |
| `--lab-weight` | 0.15 | LAB 权重 |
| `--grid-weight` | 0.25 | Grid4x4 权重 |
| `--tiny-weight` | 0.35 | TinyImage 权重 |
| `--edge-weight` | 0.05 | 边缘权重 |
| `--lbp-weight` | 0.20 | LBP 纹理权重 |
| `--candidates` | 200 | 粗筛候选数 |
| `--cpu` | — | 强制 CPU |

### 示例

```bash
# 输出 1920×1080，细粒度 tile，加重纹理匹配
mosaicraft mosaic -i photo.jpg -d lib.db \
  --out-w 1920 --out-h 1080 \
  --tile-w 16 --tile-h 16 \
  --tiny-weight 0.4 --lbp-weight 0.3

# 快速预览
mosaicraft mosaic -i photo.jpg -d lib.db \
  --out-w 640 --out-h 480 --tile-w 64 --tile-h 64
```

## 项目结构

```
Mosaicraft/
├── core/
│   ├── ImageNormalizer     归一化 180×320
│   ├── BatchProcessor      多线程批处理
│   ├── Database            SQLite 索引
│   ├── FeatureExtractor    V1~V4 特征提取
│   ├── MosaicEngine        马赛克生成引擎
│   └── UnicodeIO           Unicode 路径支持
├── compute/
│   ├── CudaBackend.h       CUDA 接口
│   └── CudaBackend.cu      GPU kernel
├── src/
│   └── main.cpp            CLI 入口
└── CMakeLists.txt
```

## 许可证

GPL — 详见 [LICENSE](LICENSE)
