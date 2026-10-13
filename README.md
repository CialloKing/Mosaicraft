# Mosaicraft v1.14.2

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
- **ANN → GPU 四层检索** — 196 维并行粗筛 → 五特征 GPU 分批精排 → 邻域去重 → 多线程贴图
- **自适应参数** — candidates 默认 150，neighborWindow O(√N) 动态缩放

### ⚡ GPU 加速
- **建库特征提取** — GPU 批量计算；CPU 归一化、Canny 和落盘校验保证 v6 特征语义一致
- **匹配精排** — CUDA 五特征并行评分
- **多分辨率** — 模板化内核，支持 180×320 / 320×180 / 360×640 / 640×360
- **纯 CPU 模式** — `-D MOSAICRAFT_CUDA=OFF` 即可无 NVIDIA 平台编译

### 🖼️ 输出格式

| 格式 | 最大分辨率 | 说明 |
|------|-----------|------|
| **JPG** | 65,500px | 默认格式，q1-100 可调 |
| **PNG** | 受整数及编码器边界限制 | 流式写入，缓冲随输出宽度增长，`--png-level 1-9` |
| **TIFF** | 受整数及编码器边界限制 | BigTIFF 直写，实测 50040×75200 |
| **WebP** | 16,383px | 超过编码器尺寸限制时明确报错 |

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
- **FeaturePack** — 特征合并读取，CPU/GPU 共用来源校验，完整同代次缓存通过单一清单发布
- **建库流水线** — CPU 归一化与 GPU 特征提取并行；GPU 批量 256；CPU 特征按 64 MiB 像素预算、最多 256 张分批并行处理，顺序提交
- **ImageCache** — 512 MiB 像素字节预算 LRU，共享只读图片；PNG/JPG/TIFF 固定线程池、双行流式输出
- **有界匹配** — 默认主机批次缓冲 256 MiB、CPU 特征缓存 256 MiB、每批最多 1024 块；任务内复用 ANN、线程池与 CUDA 工作区
- **Unicode 路径** — 图片及特征文件使用 Unicode 路径读取

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

# 发布后验收 GitHub Release 上真实可下载的 zip
.\scripts\verify-release-asset.ps1 -Tag v<version>
```

`release.ps1` 会生成 `Mosaicraft_v<version>_<platform>-<arch>_<runtime>.zip`。当前正式 Windows CUDA 包名为 `Mosaicraft_v<version>_windows-x64_cuda.zip`；CI CPU-only 候选包名为 `Mosaicraft_v<version>_windows-x64_cpu-only_ci-cpu.zip`。正式打包要求 git 工作区干净。脚本会写入包内 `BUILD_INFO.txt`，记录版本、包名、运行时和 git commit，并验证包内 `mosaicraft --version` 与 Web UI/API smoke。CI 使用同一脚本跑 Windows CPU-only 门禁，避免本地发布步骤和远端检查不一致。

`verify-release-asset.ps1` 用于发布后验收真实 GitHub Release 附件：下载 zip、校验 SHA256、检查包内 commit 是否匹配 Git tag、解压检查包结构和文档策略，并使用解压后的 `MosaicraftWebUI.exe` 跑 Web UI/API smoke。

正式发布准入以 [Release Checklist](docs/RELEASE_CHECKLIST.md) 为准：本机 CUDA 发布包和远端 CPU-only CI 都通过后，才创建 GitHub Release。

---

## 使用指南

### 命令一览

```
mosaicraft build     -i <dir> [-d <db>] [--append] [-r] [--normalize-size WxH]
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
mosaicraft build -i ./photos --normalize-size 360x640 -r

# 增量追加新图片
mosaicraft build -i ./new_photos -d library/mosaicraft.db --append -r
```

归一化 → 特征提取与去重 → 平均颜色 HSV 排序和连续编号 → SQLite + FeaturePack 缓存，全自动完成；生成时按需建立或加载 ANN 索引。

建库默认复用数据库中的平均 LAB，按整张图片的综合色调排列全库。平均 LAB 色度 `C < 0.5` 时归入灰度区，按黑到白排列；其他图片转换为 HSV，以 345° 为起点，每 15° 划分一个色相区间，相邻区间交替采用亮到暗、暗到亮，再按精细色相和色度排序。文件名从 `000000` 连续编号并保留扩展名，超过六位时统一扩展位数。在 Windows 资源管理器中选择大图标并按名称升序，即可按颜色查找。白底会稀释主体颜色，多色图片按综合色归类；近灰图片的色相可能受微小数值差异影响，图库缺少中间色时允许出现跳跃。

普通、重复和追加建库都会重新排列全库，旧数字文件名可能改变；数据库图片 ID、使用统计、图片字节和特征对应关系保持不变。独立输入目录中的原图不改名；输入与输出同目录时，已索引原图也会重新编号，递归目录中的已索引图片收拢到输出目录根目录。`--normalize-only` 只归一化，不重排已有图库。同一图片在 CPU/CUDA 间追加也会去重；颜色排序阶段直接读取数据库均值，不解码图片；旧图库首次补算排序键，后续复用缓存，仅新增、内容哈希或平均 LAB 变化时重算。CPU/CUDA 的平均颜色提取方式有精度差异，独立建库不保证逐张编号完全相同；同一数据库的重复排序保持稳定。

重编号前检查文件冲突，未索引的同名文件不会被覆盖；已索引图片不能跨卷移动到另一个输出目录。数据库旁的 `.library.lock` 用于协调建库、生成和维护。重命名被中断时会保留 `.color-order.json` 恢复清单，生成和维护拒绝读取未恢复的图库；恢复访问权限或关闭占用文件的程序后，用原建库命令加 `--append` 继续。恢复清单应交由建库命令处理。

特征缓存通过 `features.pack` 清单指向同一代次的数据与校验元数据，绑定图片 ID、特征版本、内容哈希和来源特征文件的路径、大小、修改时间及数据摘要。来源文件被修改、包损坏或清单不完整时，生成自动回退独立特征文件；缺失特征保留缺失状态。旧 v2 包不再未经校验直接参与匹配，下次普通或追加建库会自动生成新缓存；有效缓存不重复写入。清单仅在全部新文件完整落盘后切换，旧代次只在建库持有独占锁时清理。

旧版特征会在再次 `build --append` 时原地刷新为 v6，保留图片 ID 和使用统计；生成前会拒绝未刷新的旧特征。已有非空图库的归一化尺寸必须保持一致，改变尺寸请使用新的图库目录和数据库。若旧记录对应的文件已经丢失，请先恢复文件或执行 `db-purge` 清理悬空记录。颜色排序验证和单次耗时见 [验证记录](docs/COLOR_ORDER_VERIFICATION.md)。

### 生成马赛克

```powershell
# 默认参数（9×16 tile，180×320 输出，JPG）
mosaicraft mosaic -i target.jpg -d library/mosaicraft.db -o output.jpg

# 快速预览（缩小输出）
mosaicraft mosaic -i target.jpg -d library/mosaicraft.db --out-w 640 --out-h 480

# 高密度（4× tile 数量）
mosaicraft mosaic -i target.jpg -d library/mosaicraft.db --upscale 2 --output-tile 90x160

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
| `--output-tile <WxH>` | 180x320 | 单 tile 输出尺寸；横屏图库默认使用 320x180 |
| `--out-w/h` | 0=auto | 目标图缩放；只指定一轴时另一轴按比例推导 |
| `--upscale <n>` | 关闭 | 放大原图 n× 再分块，最大 64 |
| `--format` | 扩展名 | jpg / png / webp / tiff |
| `--quality` | 100 | JPEG/WebP 质量 1-100 |
| `--png-level` | 1 | PNG 压缩 1（快）~ 9（小） |
| `--write-mode` | auto | auto / stream / batch |
| `--candidates` | 150 | ANN 候选数 |
| `--topn-random` | 10 | Top-N 随机选取池 |
| `--neighbor-window` | 0=auto | 邻域去重窗口 |
| `--neighbor-penalty` | 100 | 邻域重复惩罚值 |
| `--lab/grid/tiny/edge/lbp-weight` | 0.20/0.45/0.25/0.05/0.05 | 特征权重 |
| `--penalty` | 0.01 | 图片复用惩罚 |
| `--l-range` | 20 | ANN 候选的 L 亮度差上限（0..255） |
| `--color-adjust` | 关闭 | 选图后对每个小块的 LAB L 通道施加对称随机扰动 |
| `--color-strength` | 0.04 | L 通道最大相对扰动，范围 `0..0.5`；`0.04` 对应 `±4%` |
| `--cpu` | — | 强制 CPU 模式 |
| `--benchmark` | — | 分阶段耗时统计 |
| `--analyze` | — | 质量报告 + 热力图 |
| `--deepzoom` | — | Deep Zoom 金字塔 + HTML viewer |

亮度调整需要显式启用 `--color-adjust`，单独设置 `--color-strength` 不会启用。每个输出小块分别抽取一个等概率随机档位（共 1001 档），以 `1±strength` 的乘数统一缩放 LAB L 通道，A、B 通道保持不变。默认强度 `0.04` 的乘数范围为 `0.96～1.04`，平均值为 1；强度为 0 时跳过颜色转换，保持原像素。调整在选图后执行，不改变匹配特征；像素取整、高亮截断和颜色转换仍可能影响最终图像的平均亮度。

参数中的尺寸、权重和惩罚会统一校验，未知选项、未知格式以及 NaN/Infinity 会报错。工作图像（含放大后目标、单个 tile）最多 1 亿像素、单轴最多 100 万，建库归一化单轴最多 8192。JPEG 单轴超过 65500、WebP 超过 16383 时会报错，请显式选择 PNG、TIFF 或分块输出。程序不会因编码器尺寸限制自动切换格式或缩小输出；显式 `--format` 与文件后缀不同时，后缀按所选格式调整。超大输出默认允许，无需额外开关；PNG/TIFF 可使用 `--write-mode stream` 降低输出内存占用。流式输出仍持有两行 tile 缓冲，内存随宽度和 tile 高度增长；512 MiB 图片缓存预算不代表进程总内存上限，匹配特征和候选也占用内存。

v6 统一了 CPU/CUDA 的 8×8 分区、Canny 边缘定义、统计列、评分尺度和复用惩罚，因此选图可能与旧版不同。CUDA 路径使用 CPU 的 OpenCV Canny 来保证同一语义，会增加特征提取时间。修复与验证记录见 [BUGFIX_VERIFICATION.md](docs/BUGFIX_VERIFICATION.md)。

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
│   └── RELEASE_CHECKLIST.md    # 正式发布准入清单
├── scripts/
│   └── release.ps1             # 发布构建、打包和解压验证
├── tests/
│   ├── test_core.cpp           # doctest 核心/API 合约测试
│   ├── test_regression.cpp     # 服务层建库/检查/出图回归测试
│   └── webui_smoke.ps1         # Web UI/API 真实 smoke 验证
├── .github/workflows/
│   └── ci.yml                  # Windows CPU-only CI 门禁
├── CMakeLists.txt
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

### v1.14.1 选图与内存优化

邻域去重复用图片对判断并提前结束超阈值计算，选图规则保持不变。图片按剩余使用次数缓存，最后一次使用后释放，同图并发请求共享解码。CPU 候选复用经过源文件校验的特征包；PNG 使用有界多行转换和写入，过滤、压缩和刷新策略保持不变。

ANN 使用 `features/lib.ann196` 和 `.meta` 缓存。缓存记录格式版本、维度、按图片 ID 排序的特征指纹及文件校验值；不匹配时自动重建，保留旧 `lib.ann`，无需重建图库。查询默认使用最多 8 个工作线程，选图和邻域去重仍按小块顺序执行。

五种特征及评分权重保持不变。TinyImage、LBP 的 512 维仍参与精排，粗筛不再存储其零值占位。GPU 评分临时显存上限为 256 MiB，任务内复用特征工作区。启动时 CUDA 不可用则使用 CPU；已开始的 CUDA 计算失败会返回服务错误。

使用 `--benchmark` 查看索引加载/构建、查询、特征提取、CUDA 分配/传输/评分、邻域惩罚、排序、分析、输出和缓存统计。基准脚本 `scripts/benchmark.py` 的 `--analysis-mode off|on|both` 默认 `both`，分别保存普通生成及分析模式结果。当前 v6 优化的耗时、峰值内存和回归结果见 [第二轮有界匹配报告](docs/PERFORMANCE_PHASE2.md) 与 [第一轮缓存及建库报告](docs/PERFORMANCE_PHASE1.md)；[v1.14.1 性能报告](docs/PERFORMANCE_v1.14.1.md) 保留历史样本结果。

### 速度优先的有界匹配

匹配按顺序分批完成目标特征、ANN 查询、亮度过滤、评分和选图。主机批次缓冲预算为 256 MiB，CPU 特征缓存载荷预算为 256 MiB，每批最多 1024 块；按特征尺寸、候选数量和缓存容量缩小批次，超预算单块独占一批。缓冲按需增长并复用，邻域去重、最近使用位置和随机数状态贯穿整个任务。CUDA 内部保留 256 张特征提取粒度；预算容得下两套缓冲时，下一批的特征和 ANN 与当前批的 CPU 选图重叠，最终选择仍按顺序执行。CUDA 评分临时显存继续受 256 MiB 及可用显存限制。

有效特征包的完整载荷能放入缓存预算时，CPU 顺序预载并在首次使用记录前校验来源；大库或无有效包使用按需读取的 LRU。缓存损坏、旧缓存和中文路径沿用统一读取及回退规则。选图只保存图库索引，分析保存标量及最差 20 块的详细特征。匹配完成后释放 ANN、缓存和工作区；关闭分析时同时释放目标图。

这些预算约束对应缓冲或特征载荷，未包含完整目标图、图库记录、ANN、缓存元数据及 LRU 管理结构、OpenCV/CUDA 工作区、512 MiB 图片缓存和输出缓冲，不能当作进程总内存上限。目标图仍完整解码，输出策略保持不变。运行中取消、分析数据落盘和 PNG 编码优化留待后续更新。

显式构建 `mosaicraft_match_benchmark` 后，可用 `scripts/benchmark_bounded.py` 对比原 64/64 MiB、256 块方案与默认预算，并记录墙钟、匹配阶段、源特征读取、缓存命中和主机峰值内存。此工具复用 API JSON，允许基准指定小输出 tile，不增加生产 CLI/API 参数。
