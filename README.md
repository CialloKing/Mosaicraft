# Mosaicraft v1.14.11

> GPU 加速的照片马赛克拼贴生成器 — 本地创建图库图片精准匹配，输出超大分辨率马赛克拼图

**Windows x64** · **Linux x64** · **macOS ARM64** · **Windows/Linux CUDA 加速**

（本项目纯AI生成，绝无手写代码）

---

## 快速开始

### 🖥️ Web UI（推荐）

```powershell
# 双击或命令行启动，浏览器自动打开 http://localhost:8080
MosaicraftWebUI.exe
```

可视化操作：建库 → 输入或粘贴目标图片路径 → 生成。页面显示服务工作目录，相对路径在该目录下解析；粘贴“复制为路径”的外层双引号会自动去掉。

顶部“使用帮助”提供快速开始、路径填写、尺寸与格式、匹配权重、开关、建库和任务结果说明，可在离线页面阅读。预览选项旁的帮助链接可直接定位到对应说明；切换帮助页会保留表单并继续跟踪任务。

任务区持续显示列表、当前阶段、计数、后端回退和具体错误；切换功能页或刷新后可恢复同一服务会话的任务，并可恢复参数后自行提交。断线保留最后状态并重试；提交响应丢失时不会自动重复提交。仅排队任务可以取消，运行中任务需等待结束。任务和诊断仅保留在当前服务内存，最多保留最近 50 个完成任务，清理记录不删除文件。

结果区显示实际输出路径、格式、尺寸和下载入口；可选“生成结果预览”默认关闭，开启后只自动加载有界 JPEG 预览。主图不会自动加载；后续阶段失败时，已保存的主图仍会显示。HTML 报告以附件下载，分块与 DeepZoom 提供实际目录或清单路径。

表单草稿与按用途分类的最近 10 个路径保存在浏览器本地，可清空最近路径。命令和 API 提交共用参数，PowerShell 路径使用单引号引用；缺少必填参数时禁止复制无效命令。完整接口与诊断字段见 [API 文档](docs/API.md)，本轮检查见 [运行诊断验收](docs/RUNTIME_DIAGNOSTICS_VERIFICATION.md)。

Linux/macOS 解压 tar.gz 后运行 `./MosaicraftWebUI`；命令行入口为 `./mosaicraft --help`。macOS 发布包仅支持 ARM64。

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
- **匹配精排** — 四种模板沿用原 CUDA 评分；自定义尺寸默认使用 CPU 兼容 CUDA 评分，保留原 CPU 排序、邻域处理和选图
- **多分辨率** — 180×320 / 320×180 / 360×640 / 640×360 沿用模板内核；其他合法尺寸使用与 CPU 精确兼容的通用 CUDA 特征统计，宽高各支持 1～8192，资源不足时回退 CPU
- **纯 CPU 模式** — `-D MOSAICRAFT_CUDA=OFF` 即可无 NVIDIA 平台编译

v1.14.11 统一单图输出格式来源与超限适配，并在 Web UI 中展示输出计划和实际产物。API 合约为 2.2、主兼容版本保持 2，算法、特征 v6 和图库格式不变，已有图库无需重建。Web UI 继续提供离线使用帮助、参数恢复和可选结果预览。完整变更见 [百科版本记录](https://github.com/CialloKing/Mosaicraft/blob/v1.14.11/docs/ENCYCLOPEDIA.md#v11411-输出格式来源与超限适配-2026-11-10)。

当前单图输出按“显式格式 → 输出后缀 → 输入后缀 → PNG”确定格式；自动 JPG/WebP 超限改存完整尺寸 PNG，显式选择 JPG/WebP 则缩小输出 Tile。页面展示适配前后尺寸与实际文件，验收见 [输出格式与超限适配](docs/OUTPUT_FORMAT_VERIFICATION.md)。

继续提供 sm_75/80/86/89/90/100/120 七架构原生 CUDA 内核及对应 PTX JIT，任务开始 GPU 工作前预检，不兼容时使用 CPU。正式发布包含 Windows/Linux CPU、macOS ARM64 CPU 以及 Windows/Linux CUDA 五种包；Windows CUDA 经过本机 GPU 必需验收，Linux CUDA 保留 GPU 未实机验收标记。验证边界见 [CUDA 多架构验证](docs/CUDA_ARCHITECTURES.md)。

### 🖼️ 输出格式

| 格式 | 最大分辨率 | 说明 |
|------|-----------|------|
| **JPG** | 65,500px | q1-100 可调；推导格式超限转 PNG，显式选择时缩小 Tile |
| **PNG** | 受整数及编码器边界限制 | 流式写入，缓冲随输出宽度增长，`--png-level 1-9` |
| **TIFF** | 受整数及编码器边界限制 | BigTIFF 直写，实测 50040×75200 |
| **WebP** | 16,383px | 推导格式超限转 PNG，显式选择时缩小 Tile |

- `--write-mode auto/stream/batch` — 统一控制流式/全量写入
- `--overlay-strength 0..1` — 根据基础图对应位置透明叠加，默认关闭，普通、流式和 DeepZoom 输出共用。

### 🛠️ 工具链
- **Web UI** — `MosaicraftWebUI.exe` 本地 HTTP 服务，浏览器一键运行
- **REST API** — 19 个端点（默认启用 18 个）（`/api/jobs/*`, `/api/db/*`, `/api/mosaic`），JSON 交互，异步任务管理
- **Web 命令生成器** — `tools/command-builder/index.html` 可视化拼装 CLI 命令
- **图库诊断** — `db-stats` 亮度直方图 + 覆盖缺口；`db-usage` 全局热点图；`db-health` 健康检查
- **数据库管理** — `db-purge` 减量清理；`build --append` 增量建库；`build --recursive` 递归扫描
- **质量分析** — `--analyze` 量化报告 + 热力图 + HTML 报告 + 最差 tile 导出
- **异步任务** — `POST /api/jobs/*` 提交长任务 → `GET /api/jobs/{id}` 轮询进度
- **API 文档** — `docs/API.md` 完整端点目录

### 📦 工程优化
- **FeaturePack** — 特征合并读取，CPU/GPU 共用来源校验，完整同代次缓存通过单一清单发布
- **建库流水线** — 模板路径保留归一化与 GPU 提取并行；通用 CUDA 按 64 MiB 工作缓冲预算、最多 256 张顺序分批，保留 CPU 的去重、记录 ID 和事务顺序
- **ImageCache** — 512 MiB 像素字节预算 LRU，共享只读图片；PNG/JPG/TIFF 固定线程池、双行流式输出
- **有界匹配** — 默认主机批次缓冲 256 MiB、CPU 特征缓存 256 MiB、每批最多 1024 块；任务内复用 ANN、线程池与 CUDA 工作区
- **Unicode 路径** — 图片及特征文件使用 Unicode 路径读取

---

## 安装与构建

### 工具与依赖

主构建入口要求 CMake 4.2+、PowerShell 7 和 Git。Windows x64 使用 Visual Studio 2026 的 C++ 工具；Linux x64、macOS ARM64 使用 Ninja 和系统 C++ 编译器。CPU-only 构建无需 CUDA。

在 PowerShell 7 中执行以下命令。依赖由仓库的 `vcpkg.json` 安装，工具仓库也须位于同一固定提交；已有符合版本的 vcpkg 可直接设置 `VCPKG_ROOT`。

```powershell
git clone https://github.com/CialloKing/Mosaicraft.git
cd Mosaicraft
git clone https://github.com/microsoft/vcpkg.git build/tools/vcpkg
git -C build/tools/vcpkg checkout a0400024711b283056538ac19ced80b91a83c24c
$env:VCPKG_ROOT = (Resolve-Path build/tools/vcpkg).Path
./scripts/setup-dependencies.ps1 -Mode Prepare
./scripts/setup-dependencies.ps1 -Mode Install
```

脚本按当前平台选择 Release-only triplet，依赖共用 `build/dependencies`。不要在 manifest 模式下给 `vcpkg install` 传独立包名。Linux 构建依赖需要系统的编译器、pkg-config、zip/unzip 等基础工具；打包另需 `patchelf`。从 Bash 使用时，先进入 `pwsh` 再执行整组命令，使脚本设置的环境变量保留在当前会话。

### 编译

| 平台 | CPU-only Preset | CUDA Preset |
| --- | --- | --- |
| Windows x64 | `windows-cpu` | `windows-cuda` |
| Linux x64 | `linux-cpu` | `linux-cuda` |
| macOS ARM64 | `macos-arm64-cpu` | 不支持 |

```powershell
# 将 windows-cpu 替换为当前平台所需的 Preset
cmake --preset windows-cpu
cmake --build --preset windows-cpu --target mosaicraft MosaicraftWebUI mosaicraft_tests mosaicraft_regression_tests --parallel
ctest --preset windows-cpu

# CUDA：显式选择已安装的 SDK，再使用对应 CUDA Preset
$env:CUDAToolkit_ROOT = 'C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v13.5'
cmake --preset windows-cuda
cmake --build --preset windows-cuda --target mosaicraft MosaicraftWebUI mosaicraft_tests mosaicraft_regression_tests --parallel
ctest --preset windows-cuda
```

每个 Preset 使用独立的 `build/<preset>`，程序位于其 `Release/bin`。Windows 旧入口 `cmake --preset default`、`cmake --build --preset release` 继续映射到 `windows-cuda`；`relwithdebinfo` 配置与构建入口使用独立目录。项目内核仍允许直接 CMake 配置，维护和 CI 使用上述入口；个人覆盖配置可写入不提交的 `CMakeUserPresets.json`。

测试程序不参与默认构建，因此上面的命令显式列出两个测试目标；仅执行不带 `--target` 的默认构建后，不能直接运行完整 CTest。

### 编译选项

| 选项 | 默认 | 说明 |
|:---|:---|:---|
| `MOSAICRAFT_CUDA=ON` | ✅ | CUDA GPU 加速（需 NVIDIA GPU + CUDA Toolkit） |
| `MOSAICRAFT_CUDA=OFF` | — | 纯 CPU 编译（Linux / 无 GPU Windows） |
| `CUDA_ARCH` | `sm_75;sm_80;sm_86;sm_89;sm_90;sm_100;sm_120` | CUDA 原生架构列表；保留单值覆盖 |

CUDA 构建固定 Toolkit 13.5.0 / nvcc 13.5.37，必须显式设置 `CUDAToolkit_ROOT`；编译器、头文件和运行库须来自同一 SDK，13.3 旧构建目录应保留并另建新目录。云端最小 SDK 可通过 `./scripts/setup-cuda.ps1` 安装，组件地址和 SHA256 由锁文件固定。Windows 使用 SDK 的 hybrid 加载方式，不分发显示驱动；Linux 随包部署共享运行库和相对 RPATH。

默认 CUDA 包覆盖 RTX 20/GTX 16、RTX 30、RTX 40、A100/A30、H100/H200、B200/GB200、RTX 50，同时携带与七种原生目标对应的 `compute_75/80/86/89/90/100/120` PTX，由驱动选择原生代码或 PTX JIT。任务开始 GPU 工作前检查实际内核兼容性，不可用时记录原因并使用 CPU；运行中 CUDA 错误仍报错。驱动须满足 CUDA 13.5 及所用 PTX 的要求。当前仅 Ada 实机验收，其他架构已编译覆盖、尚未实机验证。单卡构建可使用 `-DCUDA_ARCH=sm_86`；同版本构建目录保留架构缓存，正式通用包拒绝遗漏架构的产物。详见 [CUDA 多架构与验证](docs/CUDA_ARCHITECTURES.md)。

### 验证

```powershell
# 核心单元/API 合约测试 + 服务层回归测试
cmake --build --preset windows-cuda --target mosaicraft_tests mosaicraft_regression_tests
ctest --preset windows-cuda

# Web UI/API 真实 smoke：启动 MosaicraftWebUI，提交 build/mosaic job 并验证输出
cmake --build --preset windows-cuda --target mosaicraft_webui_smoke
```

### 发布自动化

```powershell
# 发布前环境检查，不构建、不打包
.\scripts\release.ps1 -InspectOnly

# 默认 CUDA Release 发布：配置/构建/CTest/WebUI smoke/打包/解压验证/SHA256
.\scripts\release.ps1

# CPU-only 原生包，无需 GPU
.\scripts\release.ps1 -BuildDir build/package-cpu -NoCuda -OutputDir build/archives

# 无 GPU 的 CUDA 候选包
.\scripts\release.ps1 -BuildDir build/package-cuda -CudaValidation BuildOnly -OutputDir build/archives

# Linux x64 正式附件，保留 GPU 未验收状态，仍完整测试和独立验收
./scripts/release.ps1 -BuildDir build/package-linux-cuda -CudaValidation BuildOnly -AllowUnvalidatedCudaRelease -OutputDir build/archives

# 发布后验收实际附件；多个附件必须明确指定名称
.\scripts\verify-release-asset.ps1 -Tag v<version> -AssetName <archive-name>
```

`release.ps1` 默认生成 Windows ZIP 或 Linux/macOS tar.gz，可用 `-ArchiveFormat` 选择格式。要求工作区干净；包内 `BUILD_INFO.txt` 记录版本、平台、架构、运行时、提交及 GPU 验收状态，包旁生成 SHA256 文件。CUDA 默认要求可用 GPU；`BuildOnly` 包默认带 `_candidate` 后缀。Linux x64 可显式使用 `-AllowUnvalidatedCudaRelease` 生成正式 tar.gz，GPU 状态仍为 `not-run`；该模式不能跳过测试和验收，不能用于 Windows。

`verify-release-asset.ps1 -ArchivePath <path> -RequireStandalone` 可独立验收 ZIP/tar.gz，`-ZipPath` 仍可使用。脚本检查哈希、元数据、文档、CLI 和包内 Web UI/API；远端附件还校验 tag 对应提交。

日常 CI 运行 Windows/Linux CPU 检查；手动或标签触发 Windows x64、Linux x64、macOS ARM64 三平台 CPU 打包、Windows CUDA 候选及 Linux CUDA 正式包，并在独立 job 验收。工作流不自动发布 Release。正式发布需三平台 CPU 包和 Linux CUDA 包通过独立验收，以及同一提交的本机 Windows CUDA GPU 验收。Linux CUDA 已通过编译和无 GPU 运行验收，尚未做 Linux 实机 GPU 验收。详见 [CI 与打包](docs/CI_PACKAGING.md)、[Release Checklist](docs/RELEASE_CHECKLIST.md) 和 [CUDA 13.5 迁移报告](docs/CUDA_13_5_MIGRATION.md)。

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

素材按现有规则解码后的宽高与图库规格完全一致时，默认直接将原文件字节保存为独立副本，跳过缩放、裁剪和重新编码，保留原始压缩数据及内嵌元数据；`--normalize-only` 同样适用，无需新增选项。判定比较两轴的精确像素尺寸，仅宽高比相同不算。同一文件不做自复制；显式转换文件格式时仍按目标格式编码。尺寸不同时继续等比覆盖、中心裁剪，再以 JPEG 质量 100（最高质量）、PNG 压缩等级 9 编码，扩展名不区分大小写。

模板 CUDA 流水线复用原字节副本的解码像素；重新编码的图片仍以落盘结果计算特征，CPU 路径保持有界分批读取。特征算法保持 v6，已有素材和有效特征不主动重写。去重仍依据落盘图片的解码像素；旧版经过重新压缩的 JPEG 与再次导入的原文件可能具有不同像素，因此可能作为不同素材保留。实现、兼容性及性能记录见 [同尺寸免重编码验证](docs/SAME_SIZE_NORMALIZATION.md)。

建库默认复用数据库中的平均 LAB，按整张图片的综合色调排列全库。平均 LAB 色度 `C < 0.5` 时归入灰度区，按黑到白排列；其他图片转换为 HSV，以 345° 为起点，每 15° 划分一个色相区间，相邻区间交替采用亮到暗、暗到亮，再按精细色相和色度排序。文件名从 `000000` 连续编号并保留扩展名，超过六位时统一扩展位数。在 Windows 资源管理器中选择大图标并按名称升序，即可按颜色查找。白底会稀释主体颜色，多色图片按综合色归类；近灰图片的色相可能受微小数值差异影响，图库缺少中间色时允许出现跳跃。

普通、重复和追加建库都会重新排列全库，旧数字文件名可能改变；数据库图片 ID、使用统计、图片字节和特征对应关系保持不变。独立输入目录中的原图不改名；输入与输出同目录时，已索引原图也会重新编号，递归目录中的已索引图片收拢到输出目录根目录。`--normalize-only` 只归一化，不重排已有图库。同一图片在 CPU/CUDA 间追加也会去重；颜色排序阶段直接读取数据库均值，不解码图片；旧图库首次补算排序键，后续复用缓存，仅新增、内容哈希或平均 LAB 变化时重算。模板 CUDA 与 CPU 的平均颜色提取仍有精度差异，独立建库不保证逐张编号完全相同；自定义尺寸的通用 CUDA 使用原 CPU 舍入规则，保持特征和排序一致。

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

# 同步生成小尺寸 JPEG 预览
mosaicraft mosaic -i target.jpg -d library/180x320/mosaicraft.db -o output/result.png --preview
```

### 主要参数

| 参数 | 默认 | 说明 |
|------|------|------|
| `-i --input` | 必填 | 目标图片路径 |
| `-d --db` | library/mosaicraft.db | 特征数据库 |
| `-o --output` | output/output（无后缀） | 输出文件名；最终后缀自动确定 |
| `--tile-w/h` | 9 / 16 | tile 网格尺寸 |
| `--output-tile <WxH>` | 180x320 | 单 tile 输出尺寸；横屏图库默认使用 320x180 |
| `--out-w/h` | 0=auto | 目标图缩放；只指定一轴时另一轴按比例推导 |
| `--upscale <n>` | 关闭 | 放大原图 n× 再分块，最大 64 |
| `--format` | 自动推导 | jpg / png / webp / tiff；显式选择优先于文件后缀 |
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
| `--overlay-strength` | 0 | 基础图透明叠加强度，范围 `0..1`；`0` 关闭 |
| `--cpu` | — | 强制 CPU 模式 |
| `--benchmark` | — | 分阶段耗时统计 |
| `--analyze` | — | 质量报告 + 热力图 |
| `--preview` | 关闭 | 同步生成 `<主图名（不含扩展名）>_preview.jpg`，仅支持单图输出 |
| `--deepzoom` | — | Deep Zoom 金字塔 + HTML viewer |

`--preview`、API 的 `generatePreview` 和 Web UI 的“生成结果预览”均为默认关闭的可选功能。开启后，在实际主图路径旁保存去掉扩展名并加 `_preview.jpg` 的预览，例如 `result.png` 对应 `result_preview.jpg`；显式 `--format` 调整主图后缀时，预览沿用最终文件名。预览保持比例、不放大，宽度不超过 1400、总像素不超过 400 万，极端长图同时遵守 JPEG 单轴 65500 上限，JPEG 质量固定为 92，与主图 `--quality` 独立。

预览从已完成亮度调整和基础图叠加的合成像素做面积平均缩小，普通与流式输出使用相同算法，生成时同步处理，无需重新读取主图。主图关闭成功后才保存预览；预览写入失败返回非零退出码并报告已保存的主图路径，保留主图。关闭时不创建、删除或更新已有预览；开启时同名普通预览文件按现有输出方式覆盖。`--preview` 不能与 `--tiled` 或 `--deepzoom` 同用，组合会在生成前报错。REST API 与 Web UI 使用相同预览语义。`--benchmark` 中的预览耗时已包含在 Encoding 中。

功能与性能验收见 [可选预览图验证](docs/PREVIEW_VERIFICATION.md)。

亮度调整需要显式启用 `--color-adjust`，单独设置 `--color-strength` 不会启用。每个输出小块分别抽取一个等概率随机档位（共 1001 档），以 `1±strength` 的乘数统一缩放 LAB L 通道，A、B 通道保持不变。默认强度 `0.04` 的乘数范围为 `0.96～1.04`，平均值为 1；强度为 0 时跳过颜色转换，保持原像素。调整在选图后执行，不改变匹配特征；像素取整、高亮截断和颜色转换仍可能影响最终图像的平均亮度。

单图格式优先级为 **显式 `--format` → 输出文件后缀 → 输入文件后缀 → PNG**。后缀大小写不敏感，支持 JPG/JPEG、PNG、WebP、TIFF/TIF。无后缀输出会追加实际后缀；未指定 `-o` 时使用 `output/output.<实际格式>`。输入后缀无法推导时回退 PNG，输入仍须能够解码；不支持的输出后缀会报错，除非使用有效的显式格式覆盖。显式格式优先于冲突后缀，兼容的别名和大小写保留原拼写。单图输出的父目录需已存在。

| 单图格式 | 推导格式时超限 | 显式选择格式时超限 |
| --- | --- | --- |
| JPG，单轴 65,500 | 改为 PNG，保持尺寸 | 缩小输出 Tile，保持 JPG |
| WebP，单轴 16,383 | 改为 PNG，保持尺寸 | 缩小输出 Tile，保持 WebP |
| PNG/TIFF | 保持尺寸，继续检查整数和资源上限 | 同左 |

例如 `-i target.jpg -o result.webp` 超限时保存完整尺寸的 `result.png`；`-o result.png --format jpg` 超限时保存缩小后的 `result.jpg`。缩小只影响输出 Tile，网格与匹配特征不变；整数取整可能略微改变比例。网格任一轴的块数已超过对应编码器上限时，无法靠缩小 Tile 适配，需要 PNG/TIFF 或分块。自动改存 PNG 使用现有 `--png-level`，不使用 JPEG/WebP 质量。预览、报告及下载均沿用实际主图路径；任务的“输出计划”不代表文件已保存。

参数中的尺寸、权重和惩罚会统一校验，未知选项、未知显式格式以及 NaN/Infinity 会报错。工作图像（含放大后目标、单个 tile）最多 1 亿像素、单轴最多 100 万，建库归一化单轴最多 8192，最终单图每轴不得超过 `INT_MAX`。超大输出默认允许，无需额外开关；PNG/TIFF 可使用 `--write-mode stream` 降低输出内存占用。流式输出仍持有两行 tile 缓冲，内存随宽度和 tile 高度增长；512 MiB 图片缓存预算不代表进程总内存上限，匹配特征和候选也占用内存。分块和 DeepZoom 沿用现有分片规则，不因整图尺寸触发格式回退或缩小。

v6 统一了 CPU/CUDA 的 8×8 分区、Canny 边缘定义、统计列、评分尺度和复用惩罚，因此选图可能与旧版不同。CUDA 路径使用 CPU 的 OpenCV Canny 来保证同一语义，会增加特征提取时间。修复与验证记录见 [BUGFIX_VERIFICATION.md](docs/BUGFIX_VERIFICATION.md)。

基础图透明叠加在选图后执行：`结果 = 素材 × (1 − 强度) + 基础图 × 强度`。例如 `--overlay-strength 0.2` 保留 80% 素材，混合 20% 基础图；默认 `0` 跳过叠加，`1` 在成功加载的小块中完全显示基础图。参考图沿用匹配时的尺寸调整、`--upscale` 和镜像补边，缩小时丢失的原图细节不会恢复。强度越高，基础图轮廓越明显，小图细节越弱。

```powershell
mosaicraft mosaic -i target.jpg -d library/mosaicraft.db -o output.png --overlay-strength 0.2
```

叠加逐块双线性采样，不分配完整输出尺寸的基础图；开启时保留目标工作图，并额外使用每个工作线程的小块缓冲。若同时启用随机亮度调整，先调整素材亮度，再叠加基础图。图库、匹配结果和共享素材缓存不因叠加而改变，无需重建图库；缺失素材仍计为失败并沿用原背景。`--analyze` 分数描述选图质量，不衡量最终叠加画面。实现与实际样图验收见 [基础图叠加验证](docs/BASE_IMAGE_OVERLAY_VERIFICATION.md)。

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
│   ├── MosaicEngine.cpp/h      # 马赛克生成流程编排
│   ├── AnalysisReport.cpp/h    # 分析、素材导出、热力图与 HTML 报告
│   ├── MosaicMetrics.cpp/h     # 阶段统计与 benchmark 输出
│   ├── NeighborhoodState.cpp/h # 共用邻域惩罚与选图窗口
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
│   ├── FeatureExtractorCuda.cu # 模板 GPU 特征提取
│   ├── CompatibleFeatures.cu   # 动态尺寸 CPU 兼容 CUDA 统计
│   └── CompatibleScoring.cu    # 与图像尺寸无关的 CPU 兼容 CUDA 评分
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
│   ├── ci.yml                  # Windows/Linux CPU 日常检查
│   └── packages.yml            # 三平台 CPU 包及 CUDA 候选
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

这些预算约束对应缓冲或特征载荷，未包含完整目标图、图库记录、ANN、缓存元数据及 LRU 管理结构、OpenCV 算子内部工作区、512 MiB 图片缓存和输出缓冲，不能当作进程总内存上限。目标图仍完整解码，输出格式按前述单图规则适配。运行中取消、分析数据落盘和 PNG 编码优化留待后续更新。

自定义尺寸的通用 CUDA 提取纳入上述 256 MiB 主机预算，最多 256 块；建库使用 64 MiB 工作缓冲预算。BGR、LAB、灰度、锁页暂存及统计输出计入主机预算，像素、整数归约和兼容评分的设备暂存合计不超过 256 MiB，并受可用显存约束。评分常驻图库单独计量，按块校验和上传，不额外复制整库到主机数组。提取与评分独立准备资源：提取超预算仍可使用 GPU 评分，评分资源不足保留可用提取后端；已经提交 CUDA 工作后发生复制、内核或同步错误会结束任务并报告错误。

通用提取保留 OpenCV 颜色转换、TinyImage、Canny 和入库统计，加速 LAB、8×8 分区和 LBP 整数统计。自定义尺寸默认使用新的 CPU 兼容 CUDA 候选评分；ANN、邻域惩罚、平分排序、随机选择和分析继续复用原 CPU 路径。特征版本仍为 v6，旧自定义尺寸图库无需重建，生成 `--cpu` 和 CPU-only 构建保持纯 CPU。相同 OpenCV/构建环境、输入像素、图库快照、使用计数和随机种子下，通用路径与原 CPU 的特征和最终选图一致；不承诺跨 OpenCV 版本逐字节一致。验证和性能数据见 [动态尺寸特征验证](docs/DYNAMIC_CUDA_FEATURES.md)及 [独立 CUDA 评分验证](docs/INDEPENDENT_CUDA_SCORING.md)。

显式构建 `mosaicraft_match_benchmark` 后，可用 `scripts/benchmark_bounded.py` 对比原 64/64 MiB、256 块方案与默认预算，并记录墙钟、匹配阶段、源特征读取、缓存命中和主机峰值内存。此工具复用 API JSON，允许基准指定小输出 tile，不增加生产 CLI/API 参数。
