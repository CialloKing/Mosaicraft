# Mosaicraft v1.13.9

> GPU 加速的照片马赛克拼贴生成器 — 本地创建图库图片精准匹配，输出超大分辨率马赛克拼图

**Windows** · Linux (预览) · **CUDA GPU 加速** 

（本项目纯AI生成，绝无手写代码）

---

## 快速开始

### 🖥️ Web UI（推荐）

```powershell
# 双击或命令行启动，浏览器自动打开 http://localhost:8080
MosaicraftWebUI.exe
```

可视化操作：建库 → 选图 → 一键生成，无需记忆命令。

### ⌨️ 命令行

```powershell
# 1. 建库（一次，图库 → 特征数据库）
mosaicraft build -i ./photos -r

# 2. 生成马赛克
mosaicraft mosaic -i target.jpg -d library/mosaicraft.db -o output.jpg
```

---

## 特性

### 🎯 匹配引擎
- **五层特征融合** — AvgLAB + 8×8 Grid + TinyImage + 边缘密度 + LBP
- **Grid-first 策略** — 8×8 Grid（192维）贡献 58%，空间加权提升 7.4%
- **ANN → GPU 四层检索** — hnswlib 粗筛 → GPU 精排 → 邻域去重 → 多线程贴图
- **自适应参数** — candidates 默认 150，neighborWindow O(√N) 动态缩放

### ⚡ GPU 加速
- **建库特征提取** — GPU 批量处理，96s → 2.2s（43×）
- **匹配精排** — CUDA 五特征并行评分
- **多分辨率** — 模板化内核，支持 180×320 / 320×180 / 360×640 / 640×360
- **纯 CPU 模式** — `-D MOSAICRAFT_CUDA=OFF` 即可无 NVIDIA 平台编译

### 🖼️ 输出格式

| 格式 | 最大分辨率 | 说明 |
|------|-----------|------|
| **JPG** | 65,500px | 默认格式，q1-100 可调 |
| **PNG** | **无限制** | 流式写入，内存恒定 ~162KB，`--png-level 1-9` |
| **TIFF** | **无限制** | BigTIFF 直写，实测 50040×75200 |
| **WebP** | 16,383px | 体积最小，自动等比缩放 |

- `--write-mode auto/stream/batch` — 统一控制流式/全量写入

### 🛠️ 工具链
- **Web UI** — `MosaicraftWebUI.exe` 本地 HTTP 服务，浏览器一键运行
- **REST API** — 17 个端点（`/api/jobs/*`, `/api/db/*`, `/api/mosaic`），JSON 交互，异步任务管理
- **Web 命令生成器** — `tools/command-builder/index.html` 可视化拼装 CLI 命令
- **图库诊断** — `db-stats` 亮度直方图 + 覆盖缺口；`db-usage` 全局热点图；`db-health` 健康检查
- **数据库管理** — `db-purge` 减量清理；`build --append` 增量建库；`build --recursive` 递归扫描
- **质量分析** — `--analyze` 量化报告 + 热力图 + HTML 报告 + 最差 tile 导出
- **异步任务** — `POST /api/jobs/*` 提交长任务 → `GET /api/jobs/{id}` 轮询进度
- **API 文档** — `docs/API.md` 完整端点目录

### 📦 工程优化
- **FeaturePack** — 50K 文件 → 2 次 fread，加载 < 300ms
- **建库流水线** — CPU 归一化与 GPU 特征提取并行；GPU 批量 32→256
- **ImageCache** — LRU 缓存，16 分片并发
- **Unicode 路径** — 中文/日文等文件名全链路支持

---

## 安装与构建

### 依赖

```powershell
vcpkg install opencv4 sqlite3 tiff --triplet x64-windows
```

### 编译

```powershell
git clone https://github.com/CialloKing/Mosaicraft.git
cd Mosaicraft
cmake -B build -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

### 编译选项

| 选项 | 默认 | 说明 |
|:---|:---|:---|
| `MOSAICRAFT_CUDA=ON` | ✅ | CUDA GPU 加速（需 NVIDIA GPU + CUDA Toolkit） |
| `MOSAICRAFT_CUDA=OFF` | — | 纯 CPU 编译（Linux / 无 GPU Windows） |

### 验证

```powershell
# 核心单元/API 合约测试 + 服务层回归测试
cmake --build build --config Release --target mosaicraft_tests mosaicraft_regression_tests
ctest --test-dir build -C Release --output-on-failure

# Web UI/API 真实 smoke：启动 MosaicraftWebUI，提交 build/mosaic job 并验证输出
cmake --build build --config Release --target mosaicraft_webui_smoke
```

### 发布自动化

```powershell
# 发布前环境检查，不构建、不打包
.\scripts\release.ps1 -InspectOnly -BuildDir build -Configuration Release

# 默认 CUDA Release 发布：配置/构建/CTest/WebUI smoke/打包/解压验证/SHA256
.\scripts\release.ps1 -BuildDir build -Configuration Release

# CPU-only 发布候选包，可用于无 CUDA runner
.\scripts\release.ps1 -BuildDir build-ci -Configuration Release -NoCuda -PackageSuffix ci-cpu
```

脚本会生成 `Mosaicraft_v<version>_<platform>-<arch>_<runtime>.zip`。当前正式 Windows CUDA 包名为 `Mosaicraft_v<version>_windows-x64_cuda.zip`；CI CPU-only 候选包名为 `Mosaicraft_v<version>_windows-x64_cpu-only_ci-cpu.zip`。脚本会验证包内 `mosaicraft --version` 与 Web UI/API smoke。CI 使用同一脚本跑 Windows CPU-only 门禁，避免本地发布步骤和远端检查不一致。

正式发布准入以 [Release Checklist](docs/RELEASE_CHECKLIST.md) 为准：本机 CUDA 发布包和远端 CPU-only CI 都通过后，才创建 GitHub Release。

---

## 使用指南

### 命令一览

```
mosaicraft build     -i <dir> [-d <db>] [--append] [-r] [--normalize-size W H]
mosaicraft mosaic    -i <img> -d <db> [-o <path>] [选项]
mosaicraft inspect   -i <img> [-d <db>]
mosaicraft db-stats  -d <db>
mosaicraft db-purge  -d <db>
mosaicraft db-usage  -d <db> [-n <N>] [--export <dir>]
mosaicraft db-health -d <db>
```

### 建库

```powershell
# 基本建库
mosaicraft build -i ./photos -d library/mosaicraft.db -r

# 自定义归一化分辨率
mosaicraft build -i ./photos --normalize-size 360 640 -r

# 增量追加新图片
mosaicraft build -i ./new_photos -d library/mosaicraft.db --append -r
```

归一化 → 特征提取 → SQLite + FeaturePack + ANN 索引，全自动完成。

### 生成马赛克

```powershell
# 默认参数（9×16 tile，180×320 输出，JPG）
mosaicraft mosaic -i target.jpg -d library/mosaicraft.db -o output.jpg

# 快速预览（缩小输出）
mosaicraft mosaic -i target.jpg -d library/mosaicraft.db --out-w 640 --out-h 480

# 高密度（4× tile 数量）
mosaicraft mosaic -i target.jpg -d library/mosaicraft.db --upscale 2 --output-tile 90 160

# 质量控制
mosaicraft mosaic -i target.jpg -d library/mosaicraft.db --png-level 9 --write-mode stream

# 质量分析 + 性能诊断
mosaicraft mosaic -i target.jpg -d library/mosaicraft.db --analyze --benchmark
```

### 主要参数

| 参数 | 默认 | 说明 |
|------|------|------|
| `-i --input` | 必填 | 目标图片路径 |
| `-d --db` | library/mosaicraft.db | 特征数据库 |
| `-o --output` | output/output.jpg | 输出路径（扩展名决定格式） |
| `--tile-w/h` | 9 / 16 | tile 网格尺寸 |
| `--output-tile <w> <h>` | 180 320 | 单 tile 输出尺寸 |
| `--out-w/h` | 原图 | 目标图缩放 |
| `--upscale <n>` | 关闭 | 放大原图 n× 再分块 |
| `--format` | 扩展名 | jpg / png / webp / tiff |
| `--quality` | 100 | JPEG/WebP 质量 1-100 |
| `--png-level` | 1 | PNG 压缩 1（快）~ 9（小） |
| `--write-mode` | auto | auto / stream / batch |
| `--candidates` | 150 | ANN 候选数 |
| `--lab/grid/tiny/edge/lbp-weight` | 0.20/0.45/0.25/0.05/0.05 | 特征权重 |
| `--penalty` | 0.01 | 图片复用惩罚 |
| `--color-adjust` | 关闭 | LAB L 通道微调 |
| `--cpu` | — | 强制 CPU 模式 |
| `--benchmark` | — | 分阶段耗时统计 |
| `--analyze` | — | 质量报告 + 热力图 |
| `--deepzoom` | — | Deep Zoom 金字塔 + HTML viewer |

### 图库管理

```powershell
# 图库诊断
mosaicraft db-stats -d library/mosaicraft.db     # 规模、LAB分布、亮度直方图

# 使用统计
mosaicraft db-usage -d library/mosaicraft.db     # Top 50 热点图
mosaicraft db-usage -d library/mosaicraft.db --export ./hot  # 导出全部使用过的图

# 健康检查
mosaicraft db-health -d library/mosaicraft.db    # 完整性校验

# 清理瘦身
mosaicraft db-purge -d library/mosaicraft.db     # 移除未使用的特征数据
```

---

## 性能表现

> 46K 图库 · RTX 4060 · 默认 9×16 tile

| 目标图 | 原图分辨率 | Tiles | 输出分辨率 | 格式 | 耗时 |
|--------|-----------|-------|-----------|------|------|
| target5 | 1134×1712 | 13,482 | 22680×34240 | JPG | 19s |
| target5 | 1134×1712 | 13,482 | 22680×34240 | PNG | 72s |
| target10 | 1503×2286 | 22,879 | 30060×43840 | JPG | 36s |
| target10 | 1503×2286 | 22,879 | 30060×43840 | PNG | ~395s |

---

## 项目结构

```
Mosaicraft/
├── src/
│   ├── main.cpp                # CLI 入口（7 条子命令）
│   └── server.cpp              # MosaicraftWebUI HTTP 服务
├── core/                       # CPU 核心引擎
│   ├── MosaicEngine.cpp/h      # 马赛克生成 + 分析 + HTML 报告
│   ├── Database.cpp/h          # SQLite 读写 + 使用统计
│   ├── FeatureIndex.h          # hnswlib ANN 索引
│   ├── FeatureUtils.h          # Grid/Tiny/Edge/LBP 特征计算
│   ├── FeaturePack.h           # 二进制特征缓存
│   ├── ImageCache.h            # 贴图 LRU 缓存
│   ├── BigTiffWriter.h         # libtiff BigTIFF 写出
│   ├── DeepZoomWriter.h        # Deep Zoom 金字塔
│   └── ImageNormalizer.cpp/h   # 图片归一化
├── compute/                    # CUDA 加速
│   ├── CudaBackend.cu          # GPU 评分
│   └── FeatureExtractorCuda.cu # GPU 特征提取
├── tools/
│   └── command-builder/        # Web 命令生成器
├── docs/
│   ├── ENCYCLOPEDIA.md         # 项目百科全书
│   ├── RELEASE_CHECKLIST.md    # 正式发布准入清单
│   └── releases/               # 单版本详细发布说明
├── scripts/
│   └── release.ps1             # 发布构建、打包和解压验证
├── tests/
│   ├── test_core.cpp           # doctest 核心/API 合约测试
│   ├── test_regression.cpp     # 服务层建库/检查/出图回归测试
│   └── webui_smoke.ps1         # Web UI/API 真实 smoke 验证
├── .github/workflows/
│   └── ci.yml                  # Windows CPU-only CI 门禁
├── CMakeLists.txt
├── CHANGELOG.md
└── README.md
```

---

## 路线图

| 版本 | 主要内容 | 状态 |
|------|---------|------|
| v0.7 – v0.8 | FeaturePack · BigTIFF · ANN · ImageCache · Grid | ✅ |
| v1.0 – v1.1 | upscale · output-tile · WebP · --analyze | ✅ |
| v1.4 – v1.6 | Spatial Weight Map · db-stats · candidates sweep | ✅ |
| v1.7 – v1.9 | GPU Features (43×) · 增量建库 · db-health | ✅ |
| v1.10 | WorstTile · Unicode 全链路 | ✅ |
| v1.11 | Web 命令生成器 | ✅ |
| v1.12 | MosaicraftWebUI · 多分辨率 GPU · CUDA 开关 | ✅ |
| v1.13 | REST API · 异步任务 · 合约版本化 · 发布自动化 | ✅ |
| v2.0 | GUI 桌面应用 | 📋 计划中 |

---

## 许可证

[GPL v2](LICENSE)
