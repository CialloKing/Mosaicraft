# 智能裁剪：本地构建和发布验收

## 本地构建

普通源码构建默认不开启智能裁剪，只需要原有 OpenCV 组件。Windows x64 可按以下步骤用锁定的 vcpkg 和 OpenCV 5.1.0 构建私人测试版本。先按[源码构建](building.md#windows)准备锁定的 `build/tools/vcpkg`，然后在仓库根目录运行：

```powershell
cmake -DMODE=Prepare -DSMART_CROP=ON -DVCPKG_ROOT=build/tools/vcpkg -P cmake/PrepareRelease.cmake
cmake -DMODE=Install -DSMART_CROP=ON -DVCPKG_ROOT=build/tools/vcpkg -P cmake/PrepareRelease.cmake
cmake -DMODE=Models -P cmake/PrepareRelease.cmake
$env:VCPKG_ROOT = "$PWD/build/tools/vcpkg"
cmake --preset windows-x64 -B build/windows-smart-crop -DMOSAICRAFT_SMART_CROP=ON `
  "-DVCPKG_INSTALLED_DIR=$PWD/third_party/vcpkg_installed/smart-crop" `
  "-DMOSAICRAFT_SMART_CROP_MODELS_DIR=$PWD/build/smart-crop-models/prepared"
cmake --build build/windows-smart-crop --config Release --target mosaicraft_test_targets
cmake --install build/windows-smart-crop --config Release --prefix build/smart-crop-local
```

模型准备会验证大小和 SHA-256。ONNX 模型准备和推理运行不依赖 Python 或 ONNX Runtime。Windows/Linux 双后端包另运行 `cmake -DMODE=SmartCropVulkanModels -P cmake/PrepareRelease.cmake`，获取已验证的 Vulkan 模型。可执行文件在自己的 `models/` 子目录查找模型。Windows 私人测试包可以切换到其他目录后用绝对路径运行：

```text
<local-install>/mosaicraft library normalize <input> -o <new-output> --size 180x320 --crop smart-real --threads 1 --json
<local-install>/mosaicraft library normalize <input> -o <new-output> --size 180x320 --crop smart-anime --threads 1 --json
```

运行摘要中的 `smartCropFacesFound`、`smartCropMoved`、`smartCropCentered` 和 `smartCropMainFaceClipped` 按图片计数。无需裁边的图片不计入；`smartCropInferenceWorkerMs` 是逐图推理耗时之和，不包括启动时的模型校验和探测，也不等于批处理的墙钟时间。同尺寸特快路径保留旧归一化的行为，包括已有的 PNG 紧凑化规则。

## 更新 Vulkan 模型

模型或转换工具变更时，在 Windows 使用 Python ONNX 和锁定的 pnnx 重新转换，按真人、动漫和显著性模型复验。`cmake/smart-crop-vulkan.json` 记录逐文件哈希，`cmake/smart-crop-vulkan-prebuilt.json` 单独记录获准分发的版本化 ZIP 地址和 SHA-256。ZIP 内的六个模型位于 `models/vulkan/`。当前来源为 v1.15.19 的 Windows 智能裁剪正式包。更新发布输入时同步这两份锁，并从空目录运行 `SmartCropVulkanModels` 验证下载结果。[转换与探针复现](../SMART_CROP_VULKAN.md#如何复现)

## 设备策略

中心裁剪仍为默认，且不接受设备选项。选择 `smart-real` 或 `smart-anime` 后，`--crop-device`（HTTP 请求中的 `cropDevice`）可取 `auto`、`cpu`、`cuda`，省略等同于 `auto`。

[CUDA + Vulkan 智能裁剪包](../SMART_CROP_VULKAN.md)的 WebUI 会按操作与模型启用 `vulkan` 和 `cuda+vulkan`：仅用于独立归一化，不一定比单 CUDA 快。

| 策略 | 行为 |
|---|---|
| `auto` | CUDA 智能版先用 CUDA；可捕获的 CUDA 故障会释放 GPU 会话，当前图用 CPU 重试。CPU 版直接使用 CPU，不探测。 |
| `cpu` | 全程只用 CPU，不探测 CUDA。 |
| `cuda` | 预检失败不创建输出；批中失败停止并报告保留的部分输出，不回退。 |

`auto` 在 CPU 续跑时由同一个 GPU worker 串行探测，最短重试间隔为 5、10、20、40、60 秒，之后最多每 60 秒重试一次。探测通过后只在下一张需要推理的图片试用，真实图片成功才计为恢复。模型与输出契约错误、CPU 推理错误仍终止；已知需要重启进程的 CUDA 错误不再探测。Windows 加载器拒绝启动或进程崩溃也不属于自动回退。

摘要中的 `smartCropCpuImages`、`smartCropCudaImages` 只计成功推理的正式图片，不含探测和跳过推理的图片；两边都用过时，实际设备为 `mixed`。`smartCropCudaFallbacks` 和 `smartCropCudaRecoveries` 统计设备切换，日志记录原因。Web 根据 `smart-crop-device-auto` 和 `smart-crop-cuda` 两个功能标记展示选项，恢复任务保留请求策略，不用实际后端覆盖它。

## 人脸检测输入

- `smart-real` 将原图直接交给 YuNet，不缩放，不限制最长边。YuNet 在推理内部向右、向下补齐到 32 的倍数，检测框仍位于原图坐标系。
- `smart-anime` 先按原图尺寸检测，不限制原尺寸首检的最长边。用灰色像素把输入补齐到 32 的倍数；检测框撤销补边后映射回原图。OpenCV 5.1.0 DNN 加载锁定的 DeepGHS 原版动态 ONNX，按对齐尺寸校验 `output0` 为 `1×5×N`，其中 `N` 是步长 8、16、32 的网格数之和。
- 动漫原图长边至少 2048 且首检没有置信度达到 0.50 的有效人脸时，用同一个 DeepGHS 模型对长边 1024 的等比缩略图补检一次。只有补检得到更可靠的人脸才替换较弱的结果，未命中仍用原结果。缩略图只用于补检，不是原图尺寸上限，也不用于生成输出。
- 动漫先保留主脸和尽可能多的其他完整人脸，再比较主脸向上、向两侧扩出的头部估计范围。保留该范围至少 **75%** 后，不再为额外的头部面积移动裁剪框，允许按前景热图和居中规则兼顾构图；也不强制裁掉能留下的头部。若保脸约束下无法达到 75%，尽量保留最大面积。该范围不是头发分割，真人仍用原规则。
- U²-NetP 的 `320×320` 主体图输入不变；检测用补边不会进入输出。两种智能模式各用一套模型逐张处理，避免多份原尺寸张量同时占用内存；`--threads` 仍可控制中心裁剪的并行度。大图耗时和内存会上升。自动策略只允许整张原图改由 CPU 推理，不降分辨率重试；终止任务时会报告已完成的输出。

用同一目录的两张偏侧插图按 `180×320` 复测 75% 策略。神里绫华样本的人工标记头部横向覆盖由最大化策略的 100% 放宽到约 70.9%（`1: scale` 对照为约 66.3%）；第二张为约 75.7%。这些读数是人工标记的横向比例，不等同于按检测框扩展范围计算的面积目标。两张的检测主脸均完整保留，CPU/CUDA 输出逐字节相同，原图未变。

### 旧版最大化头部覆盖策略复验（2026-11-24）

- 两张约 6K 的 HighLikes 偏侧插图按 `180×320` 复测：一张原尺寸漏检，另一张只有低置信度误检。补检找回相应人脸，输出按与原图像素匹配复核，两个裁剪框分别覆盖了人工标记头部横向范围的 100% 和约 99.5%。CPU/CUDA 两路输出逐字节相同，原图未变。
- 同一 HighLikes 样本 97/97 成功，需推理的 85 张均使用 CUDA，另外 12 张按旧规则不推理；检测到脸的图片从 68 增至 76。这是运行和覆盖抽查，不是整库人工质量评审。另抽验中心与真人输出，和旧版逐字节一致；两种智能构建的 CTest 各 7/7 通过。

## 模型锁与发布条件

- `cmake/smart-crop-models.json` 锁定三个权重的来源、大小和哈希。YuNet `2023mar` 有对应的 MIT 声明。原 U²-Net 权重的授权说明不能单独证明 rembg 发布附件的导出来源；DeepGHS 仓库模型卡标为 MIT，但所选权重的上游权利链还需核对。维护者接受范围已扩展至现行三个平台的智能裁剪包，包括 Windows/Linux 使用的 Vulkan 转换权重。具体范围见锁定文件。
- 三份权重及审核记录按 `cmake/smart-crop-models.json` 校验。发布矩阵包含 Windows/Linux 的 CUDA + Vulkan 智能裁剪包和 macOS 的 CPU 智能裁剪包，GPU 包还支持 CPU 推理回退。验包默认用生成的测试图核对实际推理设备和输出尺寸。可在本机提供锁定的标注正例并安装 `scripts/requirements-smart-crop-verification.txt`，额外检查主脸保全。
- 初审须覆盖偏侧真人、偏侧动漫、多人、主脸比窗口宽、无脸主体及无明显主体。分别统计两个模式在可保全主脸的样本上是否优于中心裁剪，以及中心裁剪已保全的主脸是否有新切断；另记录无脸主体效果、误移动率、批耗时及峰值内存。把批准的审核文档路径、大小和 SHA-256 写入 `qualityReview`；不得用两个解包正例代替总体质量审核。
- 解包正例锁定 PNG 和人工标注的 `mainFace: [left, top, right, bottom]`（半开坐标）。每张图高 320、宽大于 180，主脸能完整落入 `180×320`，但中心裁剪切脸。开启标注验收时，从其他工作目录运行，以输出像素核对裁剪位置和主脸覆盖，同时检查检测和移动指标。复验已有智能裁剪版本时，使用该版本的源码锁及标注。

## 2026-11-23 本地验收记录

- Windows x64 OpenCV 4.12：默认构建和启用构建均通过 7 项 CTest；命令转义测试通过。CLI 两路分别移走未选模型仍可运行；移走或篡改所选模型时失败且不创建输出目录。同尺寸、隐式和显式中心裁剪与 v1.15.14 对应输出逐字节一致，原图哈希不变。
- Windows x64 私人 CPU ZIP：两张人工画框的偏侧正例均检测到脸并移动裁剪框，复验确认输出像素完整覆盖标注主脸。HTTP API 两路成功并保留模式，非法值及非归一化用途返回 `400`；Chromium、Firefox 检查了选择器、生成的 CLI、参数恢复和旧后端的禁用保护。未做操作系统级断网验证或跨平台公开包验收。
- 同一台机器、每次输入一张、`--threads 1`：中心约 0.16–0.18 秒、进程峰值工作集约 12–13 MiB；真人约 1.67 秒、306.5 MiB；动漫约 2.05 秒、398.7 MiB。两份私人安装树用同样的 ZIP 工具压缩，开启后由 6,985,686 字节增至 27,880,433 字节（约 +19.93 MiB）。这不是批量基准或正式发布包体积。
- 隔离的 OpenCV **5.1.0** Windows CPU 实验：直接加载 DeepGHS 原版动态 ONNX（未改文件），用同一个网络实例交替输入 `640×640`、`320×320`、`640×384` 和 `960×960`，`output0` 分别为 `1×5×8400`、`1×5×2100`、`1×5×5040` 和 `1×5×18900`。同一动漫正例与原版 ONNX Runtime 相比，最大框坐标差约 0.00121 像素、置信度差约 2.81×10⁻⁶。YuNet 和 U²-NetP 也做了接口探测。5.1 新图执行器会提示不支持显式设置 target；这次单独测试使用 CPU wheel，不替代原生产品构建验收。

### 升级至 OpenCV 5.1.0 后的原生复验

- Windows x64 普通版和智能裁剪版分别通过全部 7 项 CTest。普通 CPU ZIP 通过 `Release.cmake` 的解包验收。智能裁剪私人 ZIP 在清空依赖搜索路径、把代理指向不可用地址后，从另一工作目录复验两张人工标注偏侧正例，真人和动漫均保全主脸；这不是操作系统级断网测试。HTTP API 两种模式均实际完成推理，非法用法仍被拒绝。
- 原生 C++ 探测用同一个 DNN 实例直接读 DeepGHS 原版 ONNX，再次确认 `640×640`、`320×320`、`640×384`、`960×960` 分别产生 `1×5×8400`、`1×5×2100`、`1×5×5040`、`1×5×18900`。该轮复验的动漫 CLI 仍用 `640×640`。两个模式只需要本模式的人脸模型；所选模型丢失或 SHA-256 不符时，在创建输出目录前失败。同尺寸路径和隐式／显式中心裁剪在两张正例中与旧版逐字节一致。
- 同批 HighLikes 分层抽样 96 张，`180×320`、4 线程：中心和动漫智能裁剪各有 96/96 张与 4.12 对应输出逐字节一致，96 张样本原图哈希未变；同批普通建库的 `grid_lab`、`tiny`、`lbp` 特征也逐条一致。动漫智能模式检测到 77 张、移动 34 张。按检测框复算，能完整放进裁剪窗口的 73 个主脸，中心保全 58 个、智能裁剪保全 73 个；未新切断中心已保全的主脸。另有 4 个主脸大于裁剪窗口。这是同模型复算，不是独立人工质量标注，也不代表全库。
- 样本单次动漫批处理约 35.49 秒，进程峰值工作集约 1.29 GiB；当时有其他构建活动，不作为版本性能比较。相同工具压缩的两份 Windows x64 私人安装树分别为 9,863,890 和 31,722,528 字节，智能版本增加约 20.85 MiB。Linux、macOS、Windows ARM64、GPU 和 Qt 变体未在这次复验中运行。

### 使用 cuDNN 的历史 CUDA 对照构建

只针对 Windows/Linux x64。`MOSAICRAFT_SMART_CROP_CUDA` 默认关闭，普通智能裁剪仍用 CPU。需要带 `dnn-cuda` 的 `opencv5`，以及锁定的 CUDA 13.5.37 SDK（含 cuBLAS）和 cuDNN 9.28.1.4 CUDA 13。cuDNN 用 `cmake -DMODE=CuDNN -P cmake/PrepareRelease.cmake` 下载、校验并解包；普通依赖安装不会下载 cuDNN。以下示例假定已准备锁定的 vcpkg 和模型：

```powershell
$env:MOSAICRAFT_CUDA_SDK = $env:CUDA_PATH.Replace('\','/')
$env:MOSAICRAFT_CUDNN_SDK = "$($PWD.Path.Replace('\','/'))/build/tools/cudnn-9.28.1.4"
$env:VCPKG_ROOT = "$PWD/build/tools/vcpkg"
$deps = "$PWD/third_party/vcpkg_installed/smart-crop-cuda"
cmake -DMODE=Install -DSMART_CROP=ON -DSMART_CROP_CUDA=ON -P cmake/PrepareRelease.cmake
# 独立构建，不复用 CPU 构建缓存。
cmake --preset windows-x64 -B build/windows-smart-crop-cuda -DVCPKG_MANIFEST_INSTALL=OFF `
  "-DVCPKG_INSTALLED_DIR=$deps" -DMOSAICRAFT_CUDA=ON -DMOSAICRAFT_SMART_CROP=ON `
  -DMOSAICRAFT_SMART_CROP_CUDA=ON -DMOSAICRAFT_BUNDLE_RUNTIME=OFF `
  "-DCUDAToolkit_ROOT=$env:MOSAICRAFT_CUDA_SDK" `
  "-DMOSAICRAFT_SMART_CROP_MODELS_DIR=$PWD/build/smart-crop-models/prepared"
cmake --build build/windows-smart-crop-cuda --config Release --target mosaicraft MosaicraftWebUI
cmake --install build/windows-smart-crop-cuda --config Release --prefix build/windows-smart-crop-cuda/local
```

从本地安装运行前，Windows 在 `PATH` 加入 `$deps/x64-windows-release/bin`、SDK 的 `bin/x64`、cuDNN 的 `bin/x64`；Linux 使用 `linux-x64` preset，并按 SDK 布局设置 `LD_LIBRARY_PATH`。CUDA 版是本地验证构建，不是可再分发的安装包。指定 `--crop-device cuda --json` 后，只有图中卷积已选择 CUDA 执行器才接收输出。排查回退时，可设置 `OPENCV_LOG_LEVEL=INFO` 查第一个不支持的算子。

**性能与发布门槛：**先复验两类正例，逐张核对 CPU 已保全的标注主脸没有被 CUDA 新切断，再测试不同尺寸连续切换、无卡、缺库、显存不足和部分输出。按模式分别运行 `python scripts/bench_smart_crop_cuda.py <cli> <input> <new-work-dir> --mode smart-anime`（真人替换为 `smart-real`，Python 3.11+ 仅用于验收）。脚本交错三轮，比较整条流程的中位吞吐，要求实际后端正确、零失败且比值至少 1.5；记录逐次模型耗时、设备级采样显存、输入 SHA-256 和完整 CLI 日志。显存采样含桌面与其它进程，可能漏掉瞬时峰值，不等同于进程精确峰值。正式比较时应排除后台负载，动漫输入包含 HighLikes 的 96 张样本与 `6534×8802` 最大图；真人用独立标注样本与同等级大图。开发机短测通过后再隔离复跑 HighLikes 全量；已有 CPU 全量任务不停止。

cuDNN 分发和独立验收的说明适用于早期 Windows CUDA 包。现行 Windows/Linux 正式包的 CUDA 路径使用 CUTLASS，与 Vulkan 推理同包分发。[当前打包入口](packaging.md#cmake-发布入口) · [早期 cuDNN 发布证据](smart-crop-release-evidence.md)

#### 自动策略变更前的 Windows 开发基线（2026-11-24）

RTX 4060 Laptop 8188 MiB、驱动 621.14、CUDA 13.5.37、cuDNN 9.28.1.4。原生 OpenCV 5.1.0 未补丁时三个图都因 `Resize`/`Resize2` 整图回落；补入设备 Resize 和主机生成的动态坐标轴后，前向结束时 U²-NetP 119/119、YuNet 53/53、DeepGHS 64/64 个卷积在 CUDA。真人与动漫分别在同一会话正反切换形状，DeepGHS 的动态网格随输入变化。两份独立标注的偏侧正例均以输出像素复核保全主脸。真人三个衍生尺寸也与原尺寸 CPU 结果逐字节相同。

| 输入 | CPU 中位墙钟 / 张每秒 | CUDA 中位墙钟 / 张每秒 | 中位吞吐比 | CUDA 设备级采样峰值 | 失败 |
|---|---:|---:|---:|---:|---:|
| 动漫：HighLikes 96 张 + `6534×8802` 原图 | 316.02 s / 0.307 | 27.75 s / 3.495 | 11.39× | 7617 MiB | 0 |
| 真人：同一照片和四张衍生图（最大 `6534×8802`） | 13.36 s / 0.374 | 2.87 s / 1.741 | 4.65× | 5662 MiB | 0 |

每个配置各三轮、`180×320`、一个会话。两个模式每轮 CPU/CUDA 输出逐字节相同，参加测试的输入 SHA-256 未变。U²-NetP / 人脸适配器耗时中位数（整批累计，含预处理和解析）：动漫 CPU 92.81/211.14 s、CUDA 2.83/14.61 s；真人 CPU 5.04/6.25 s、CUDA 0.11/1.17 s。采样值含桌面与其他进程；既有 HighLikes CPU 批次未停止，因此结果不是独占机器的正式性能报告。真人样本同源，不能替代独立真人质量评审。

隐藏 GPU 和使用未补算子的 DLL 都在创建输出前明确失败，未降级。移除开发运行环境中的 cuDNN 路径时，Windows 加载器以 `0xC0000135` 拒绝启动，未生成输出；此情况下程序本身无法返回 JSON。真实显存耗尽后的部分输出、Linux GPU 和无 Toolkit/cuDNN 的离线解包尚未验收。

### 真人与动漫原尺寸推理复验

- Windows x64 OpenCV 5.1.0 智能版原生构建通过 7/7 项 CTest，安装目录的 CLI 从另一工作目录运行，两种模式都保全了各自偏侧正例的人工标注主脸。HTTP API 两路实际完成推理，三种非法用法仍返回 `400`。
- 从同一人工标注真人正例制作三张不同尺寸的偏侧图（`1603×541`、`4299×2069`、`1327×639`），同批提交以验证尺寸切换。原尺寸 YuNet 检测到 3/3 张，输出像素与包含完整标注的裁剪窗口一致；旧版最长边 640 检测到 0/3 张。隐式中心、显式中心与旧版中心输出逐字节一致，原图未变。样本源自同一正例，不代表三张独立照片。新真人批次约 4 秒，峰值工作集约 998 MiB。
- 动漫原尺寸实现复跑同一 HighLikes 分层抽样 96 张，`180×320`：96/96 张都生成输出，中心裁剪与旧版逐字节一致，原图哈希未变。动漫智能模式检测到 67 张、移动 33 张；按原尺寸模型框复算，能放进窗口的 63 个主脸中，中心保全 51 个，智能保全 63 个。改用旧版固定输入的检测框作为参照，旧版保全 73 个，新版保全 69 个；说明两种输入会漏检或选择不同人脸，不能据此宣称原尺寸全面胜出。这些数据不是独立人工标注，也不代表全库。
- 该动漫批次约 168 秒，峰值工作集约 3.12 GiB，运行时有其他构建和测试任务，不作为独立性能基准。合并两种原尺寸方案后，在最终安装树复测该批最大原图 `4299×6068`（对齐至 `4320×6080`），输出与动漫批次逐字节一致；该图未达可靠人脸阈值。

本轮自动设备策略的复验、缺失证据及实际放行状态见[发布证据](smart-crop-release-evidence.md)。


### CUDA I/O 流水线：开发机基准

Windows x64 CUDA 智能版的 `smart-real` 和 `smart-anime` 已分别通过门槛，默认使用流水线。设 `MOSAICRAFT_SMART_CROP_PIPELINE=1` 可显式请求流水线，`=0` 为两类强制串行。只有启动时预检选中 CUDA、至少 8 个输入且应用线程预算至少为 3 才使用流水线。读图解码和输出保存各使用一条线程，原图识别仍按输入顺序由同一个会话处理；`auto` 仍只在 CUDA 故障时改用 CPU。读、写接力槽各保留一张、各有 128 MiB 缓冲额度；超额度图片不再并发推理和编解码，直接消化已解码图。额度不含编解码临时内存、DNN 工作区或显存。

```text
python scripts/bench_smart_crop_cuda.py <cli> <input> <new-work-dir> --mode smart-anime --study pipeline
python scripts/bench_smart_crop_cuda.py <cli> <input> <new-work-dir> --mode smart-anime --study hybrid
```

`pipeline` 对比同一 CLI 的串行／流水线；全量成对复验用 `--rounds 1 --timeout 5400`，不能用单轮代替样本三轮。`hybrid` 额外需要 Pillow，把 5%／10% 的输入像素工作量分给 CPU（不分配最大档），限制 CPU 子进程的 OpenCV 并行线程数并把较慢子进程的尾部也计入墙钟。两套试验都逐文件对比 SHA-256，记录 Windows 进程峰值工作集和设备级采样显存；混合工作集使用两个子进程各自峰值之和作为上界。混合试验不改变生产调度。

运行流水线或显式设置开发开关时，`smartCropPipeline` 报告实际路径（0 或 1）。`smartCropReadDecodeMs`、`smartCropCropMs`（含推理）、`smartCropEncodeWriteMs` 是各阶段累计时间，`smartCropWaitInputMs`／`smartCropWaitOutputMs` 是协调线程的等待时间；阶段时间有重叠，不能相加作为墙钟。


2026-11-24，Ryzen 9 7940H／RTX 4060 Laptop 8 GiB（驱动 621.14），同一个 Windows x64 CLI、`180×320`、3 个应用线程，既有 CPU 全量任务结束后进行开发机对照：

| 输入 | 串行 CUDA 墙钟 | 流水线墙钟 | 缩短 | 流水线最大新增工作集 |
| --- | ---: | ---: | ---: | ---: |
| 真人 32 张独立照片 | 7.93 s | 6.62 s | 16.5% | 1.05 MiB |
| HighLikes 97 张（含 6534×8802 原图） | 19.39 s | 15.58 s | 19.6% | 0.66 MiB |
| HighLikes 200 张分层复验（含最大原图） | 34.57 s | 26.29 s | 24.0% | 0.63 MiB |

真人 32 张和动漫 97 张各跑三轮，表中取中位数。动漫 200 张复验替代原定的 12,340 张全量成对验证：按原图像素数排序（相同像素数用原文件顺序），从首尾之间等距取 200 个秩，按原文件顺序跑一组串行／流水线。两路各输出 200 张，输入 SHA-256 不变、文件名和输出 SHA-256 完全一致、零失败，`6534×8802` 最大图未出现 OOM。设备级采样显存峰值为 6132／6110 MiB（含桌面占用）。流水线推理累计 23.24 s，等待输入 0.45 s；串行推理累计 23.41 s，提速主要来自把读图、写盘与推理重叠，不是推理本身变快。未完成全量成对验证，不将样本提速外推为全库实测。

其他对照也保持输入不变、输出 SHA-256 一致、零失败。另用真人 8 张（含 6534×8802 压力图）对照，未出现 OOM。32 张真人输入来自 Wikimedia Commons 标注为美国国会作品公有领域的肖像，一人一张；这是性能集，不是独立的主脸质量审核。图片、来源清单及原始报告只存于本地 `build/smart-crop-cuda/pipeline-study-20261124-163502/`。

同批 97 张再做三轮分工：纯 CUDA 流水线 16.29 s，CPU 分得 4.94% 像素（24 张）时为 22.54 s，分得 9.85% 像素（36 张）时为 34.91 s。两档结果均保持输出字节一致，但都因为总时间变长、工作集上界增加超过 512 MiB 而不采纳。

### 同卡 CUDA 双会话

Windows x64 CUTLASS 构建可用 `MOSAICRAFT_SMART_CROP_CUDA_LANES=2` 试用同卡双会话。独立动漫归一化须选择 `--crop-device auto`，至少有 32 张输入、4 个应用线程，且 CUDA 报告的总显存至少为 7.5 GiB。两张图补齐到 32 的倍数后均不超过 400 万像素、可用显存至少为 3 GiB 时允许并发。大图独占推理，显存余量不足时先释放第二会话。`=1` 选择默认的单会话。

同卡调度沿用原始输出编号。第二会话遇到可恢复的 CUDA 故障时退出本批，由主会话重试该图。`smartCropCudaLanes` 和 `smartCropCudaSecondLaneImages` 记录实际使用情况。`smartCropCudaLaneOverlapMs` 计量两条推理工作线程重叠的时间。

```text
python scripts/bench_smart_crop_cuda.py <cli> <混合尺寸输入> <新报告目录> --mode smart-anime --study same-cuda --rounds 3
python scripts/bench_smart_crop_cuda.py <cli> library/HighLikes <新全量报告目录> --mode smart-anime --study same-cuda --rounds 1 --timeout 5400
```

两路需逐图输出 SHA-256 和裁剪框一致，且零 CPU 回退。准入阈值为耗时至少缩短 10%、混合及全量的整卡采样显存增量不超过 512 MiB。默认开启还需第二种 CUDA 架构的 Windows 实卡验收。

### DeepGHS CUDA 输入试验

带 `smart-crop-cuda-input.patch` 的 CUTLASS 构建支持 `MOSAICRAFT_SMART_CROP_CUDA_INPUT=legacy|pinned|fused`。`pinned` 沿用 OpenCV 的 CPU 张量转换，复用页锁定的 FP32 上传缓冲。`fused` 上传 8 位 BGR，在 DNN 的同一条 CUDA 流上生成 FP32 RGB 张量。两路都限制在补齐后 400 万像素以内，可在 `smartCropOptimizedCudaInputs` 中核对使用次数。默认 `legacy`。

```text
python scripts/bench_smart_crop_cuda.py <cli> <混合尺寸输入> <新报告目录> --mode smart-anime --study cuda-input --rounds 3
python scripts/bench_smart_crop_cuda.py <cli> library/HighLikes <新全量报告目录> --mode smart-anime --study cuda-input --cuda-input-variant fused --rounds 1 --timeout 5400
```

`--cuda-input-lanes 1` 可固定使用单会话做独立对照。切换默认输入路径前，CUDA 输入张量须逐位一致，整批输出和裁剪框须一致，全量耗时至少缩短 3%。

### CUDA 卷积权重复用（2026-11-24）

OpenCV 5 新图引擎在尺寸切换时会重建 `Conv2` 节点；尺寸相关的 cuDNN 描述符需要重建，固定卷积权重和偏置不需要再次上传。补丁让每个 FP32 cuDNN 卷积执行器各持有一份设备权重，跨尺寸复用；权重版本变化或会话销毁会清空。不缓存旧尺寸节点，不改卷积算法选择、模型、尺寸、裁剪顺序或恢复策略。

使用此补丁构建的新版本中，真人和动漫只要选中 CUDA FP32，就默认启用权重复用；不要求启用 I/O 流水线。设 `OPENCV_DNN_CUDA_REUSE_CONV2_WEIGHTS=0` 可回退，设为 `1` 可显式开启。CPU 和中心裁剪路径不变。已发布的 v1.15.16 不含此补丁。

```text
python scripts/bench_smart_crop_cuda.py <cli> <input> <new-work-dir> --mode smart-anime --study weights
```

用包含此补丁的 Windows x64 构建，两路都强制使用 CUDA 流水线。脚本显式设置开关，交错三轮，记录同一 CLI 和 DLL 的 SHA-256，对照墙钟、输出 SHA-256、工作集以及设备级采样显存。脚本仍用原试验判据：至少 5% 墙钟收益、逐文件字节一致、零新增失败、峰值工作集最多增加 512 MiB；它不决定程序默认值。

RTX 4060 Laptop 上，同一批 HighLikes 24 张混合尺寸图做了两组三轮对比，中位墙钟分别从 **6.71 s 降至 5.03 s（−25.0%）**、从 **6.28 s 降至 5.14 s（−18.1%）**。24 张输出 SHA-256 与基线完全相同，输入未变、零失败，最大新增进程工作集约 0.30 MiB。真人另取 8 张做一轮回归，两路输出逐字节一致，不作为真人性能验收。启用实验开关的 CUDA 构建通过 7/7 项 CTest。

单次同输入 Nsight 对照中，`cudaMalloc` 从 3,773 次降至 598 次，`cudaFree` 从 3,777 次降至 602 次，主机到设备上传从 1,132 MiB 降至 845 MiB。内核次数未变；CUDA API 累计时间可能含同步，不能直接换算为墙钟收益。

上述短测时有其他 GPU 负载。另一项把动漫补边和张量转换移到读线程的实验，在同批三轮中位由 6.43 s 变为 6.48 s，未保留实现。

后台 GPU 重负载退出后，继续使用同一候选 CLI/DLL、`180×320`、3 个应用线程，在同一输入上交错三轮。两路都是严格 CUDA 流水线：

| 样本 | 现有流水线中位 | 权重复用中位 | 墙钟缩短 | 最大配对新增工作集 | 设备级采样显存峰值（现有 → 复用） |
| --- | ---: | ---: | ---: | ---: | ---: |
| HighLikes 200 张，含 `6534×8802` 最大原图 | 31.80 s | 26.05 s | **18.1%** | 0.34 MiB | 6205 → 6258 MiB |
| 32 张独立真人照片 | 6.53 s | 6.25 s | **4.3%** | 1.20 MiB | 3400 → 3313 MiB |

动漫三轮配对分别为 `26.55 → 26.11 s`、`31.80 → 26.05 s`、`32.11 → 23.54 s`，每轮均更快，但幅度有波动。人脸阶段累计中位由 22.81 s 降至 17.02 s。两种模式所有轮次均为零失败、零 CPU 回退，输入 SHA-256 不变，输出文件名和 SHA-256 也与此前流水线报告一致。最大动漫图每轮均成功、无 OOM；未跑全库，显存采样也不等同于进程瞬时峰值。

启用候选开关再做真人 8 张回归：可见 GPU 时 `auto` 与严格 CUDA 输出一致；隐藏 GPU 时严格 CUDA 失败且不创建输出目录，`auto` 退到 CPU，输出与显式 CPU 一致。这验证的是启动阶段回退，不是真实运行中 OOM。动漫达到原试验门槛；真人的 4.3% 低于原门槛，但复验输出和内存均通过，仍采用该优化。报告和时间线留在 `build/cuda-crop-profile-20261124/`。

切换默认值后，不设置开关重跑动漫 200 张、真人 32 张，输出 SHA-256 均与旧路径相同，没有新增失败。Nsight 短测中，未设置开关与显式 `=1` 的 `cudaMalloc` 次数一致，动漫 24 张均为 598 次、真人 8 张均为 480 次；显式 `=0` 分别恢复为 3,773 和 1,328 次。三种设置的短测输出逐字节一致。默认行为的报告位于本地 `build/cuda-crop-profile-20261124/default-enabled/`。
