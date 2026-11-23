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

模型准备会验证大小和 SHA-256；模型准备与运行均不需要 Python 或 ONNX Runtime。可执行文件在自己的 `models/` 子目录查找文件，不从当前工作目录找模型，也不下载。Windows 私人测试包可以切换到其他目录后用绝对路径运行：

```text
<local-install>/mosaicraft library normalize <input> -o <new-output> --size 180x320 --crop smart-real --threads 1 --json
<local-install>/mosaicraft library normalize <input> -o <new-output> --size 180x320 --crop smart-anime --threads 1 --json
```

运行摘要中的 `smartCropFacesFound`、`smartCropMoved`、`smartCropCentered` 和 `smartCropMainFaceClipped` 按图片计数。无需裁边的图片不计入；`smartCropInferenceWorkerMs` 是逐图推理耗时之和，不包括启动时的模型校验和探测，也不等于批处理的墙钟时间。同尺寸特快路径保留旧归一化的行为，包括已有的 PNG 紧凑化规则。

## 人脸检测输入

- `smart-real` 将原图直接交给 YuNet，不缩放，不限制最长边。YuNet 在推理内部向右、向下补齐到 32 的倍数，检测框仍位于原图坐标系。
- `smart-anime` 同样不缩放、不限制最长边。仅在推理时用灰色像素将宽高补齐到 32 的倍数；检测框撤销补边偏移后映射回原图。OpenCV 5.1.0 DNN 直接加载锁定的 DeepGHS 原版动态 ONNX，按对齐后的尺寸校验 `output0` 为 `1×5×N`，其中 `N` 是步长 8、16、32 的特征图网格数之和。此前 OpenCV 4.12 实验使用固定形状转换产物，模型哈希不同。
- U²-NetP 的 `320×320` 主体图输入不变；检测用补边不会进入输出。两种智能模式各用一套模型逐张处理，避免多份原尺寸张量同时占用内存；`--threads` 仍可控制中心裁剪的并行度。大图耗时和内存会上升，推理失败时明确报错并保留已完成的输出，不降分辨率重试。

## 模型锁与发布条件

- `cmake/smart-crop-models.json` 锁定三个权重的来源、大小和哈希。YuNet `2023mar` 有对应的 MIT 声明。原 U²-Net 权重的授权说明不能单独证明 rembg 发布附件的导出来源；DeepGHS 仓库模型卡标为 MIT，但所选权重的上游权利链还需核对。当前两项均未获准再分发。禁止把私人测试安装目录当作可发布附件。
- 三份权重的再分发依据和许可证齐备、独立人工标注的质量审核文档及两份可再分发的正例通过后，才可设置仓库变量 `MOSAICRAFT_SMART_CROP_RELEASE=1`（本地发布入口对应 `-DSMART_CROP=ON`）。归档验收用 `scripts/requirements-smart-crop-verification.txt` 的 Pillow 对照像素与人工标注，不属于解包后的运行依赖。
- 初审须覆盖偏侧真人、偏侧动漫、多人、主脸比窗口宽、无脸主体及无明显主体。分别统计两个模式在可保全主脸的样本上是否优于中心裁剪，以及中心裁剪已保全的主脸是否有新切断；另记录无脸主体效果、误移动率、批耗时及峰值内存。把批准的审核文档路径、大小和 SHA-256 写入 `qualityReview`；不得用两个解包正例代替总体质量审核。
- 解包正例锁定 PNG 和人工标注的 `mainFace: [left, top, right, bottom]`（半开坐标）。每张图高 320、宽大于 180，主脸能完整落入 `180×320`，但中心裁剪切脸。发布和独立验包从其他工作目录运行，以输出像素核对实际裁剪位置和完整主脸，同时检查检测和移动指标。任何目标平台失败都不能交付宣称完整支持的包。复验已有智能裁剪版本时，使用该版本的源码锁及标注。

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

### CUDA 智能裁剪（开发构建，不进入发布包）

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

Windows 和 Linux 必须分别在真实 GPU、无 Toolkit/cuDNN 的清洁环境断网解包验收。现有权重授权和人工质量门禁仍生效；在批准 NVIDIA 运行时文件清单（包括延迟加载的依赖）并取得实测结果前，`Release.cmake` 和 runtime bundle 安装均拒绝智能裁剪 CUDA，不套用马赛克 CUDA 的 `BuildOnly` 放行逻辑。

#### Windows 开发机实测（2026-11-24）

RTX 4060 Laptop 8188 MiB、驱动 621.14、CUDA 13.5.37、cuDNN 9.28.1.4。原生 OpenCV 5.1.0 未补丁时三个图都因 `Resize`/`Resize2` 整图回落；补入设备 Resize 和主机生成的动态坐标轴后，前向结束时 U²-NetP 119/119、YuNet 53/53、DeepGHS 64/64 个卷积在 CUDA。真人与动漫分别在同一会话正反切换形状，DeepGHS 的动态网格随输入变化。两份独立标注的偏侧正例均以输出像素复核保全主脸。真人三个衍生尺寸也与原尺寸 CPU 结果逐字节相同。

| 输入 | CPU 中位墙钟 / 张每秒 | CUDA 中位墙钟 / 张每秒 | 中位吞吐比 | CUDA 设备级采样峰值 | 失败 |
|---|---:|---:|---:|---:|---:|
| 动漫：HighLikes 96 张 + `6534×8802` 原图 | 316.02 s / 0.307 | 27.75 s / 3.495 | 11.39× | 7617 MiB | 0 |
| 真人：同一照片和四张衍生图（最大 `6534×8802`） | 13.36 s / 0.374 | 2.87 s / 1.741 | 4.65× | 5662 MiB | 0 |

每个配置各三轮、`180×320`、一个会话。两个模式每轮 CPU/CUDA 输出逐字节相同，参加测试的输入 SHA-256 未变。U²-NetP / 人脸适配器耗时中位数（整批累计，含预处理和解析）：动漫 CPU 92.81/211.14 s、CUDA 2.83/14.61 s；真人 CPU 5.04/6.25 s、CUDA 0.11/1.17 s。采样值含桌面与其他进程；既有 HighLikes CPU 批次未停止，因此结果不是独占机器的正式性能报告。真人样本同源，不能替代独立真人质量评审。

隐藏 GPU 和使用未补算子的 DLL 都在创建输出前明确失败，未降级。移除开发运行环境中的 cuDNN 路径时，Windows 加载器以 `0xC0000135` 拒绝启动，未生成输出；此情况下程序本身无法返回 JSON。真实显存耗尽后的部分输出、Linux GPU 和无 Toolkit/cuDNN 的离线解包尚未验收，官方包保持禁发。

### 真人与动漫原尺寸推理复验

- Windows x64 OpenCV 5.1.0 智能版原生构建通过 7/7 项 CTest，安装目录的 CLI 从另一工作目录运行，两种模式都保全了各自偏侧正例的人工标注主脸。HTTP API 两路实际完成推理，三种非法用法仍返回 `400`。
- 从同一人工标注真人正例制作三张不同尺寸的偏侧图（`1603×541`、`4299×2069`、`1327×639`），同批提交以验证尺寸切换。原尺寸 YuNet 检测到 3/3 张，输出像素与包含完整标注的裁剪窗口一致；旧版最长边 640 检测到 0/3 张。隐式中心、显式中心与旧版中心输出逐字节一致，原图未变。样本源自同一正例，不代表三张独立照片。新真人批次约 4 秒，峰值工作集约 998 MiB。
- 动漫原尺寸实现复跑同一 HighLikes 分层抽样 96 张，`180×320`：96/96 张都生成输出，中心裁剪与旧版逐字节一致，原图哈希未变。动漫智能模式检测到 67 张、移动 33 张；按原尺寸模型框复算，能放进窗口的 63 个主脸中，中心保全 51 个，智能保全 63 个。改用旧版固定输入的检测框作为参照，旧版保全 73 个，新版保全 69 个；说明两种输入会漏检或选择不同人脸，不能据此宣称原尺寸全面胜出。这些数据不是独立人工标注，也不代表全库。
- 该动漫批次约 168 秒，峰值工作集约 3.12 GiB，运行时有其他构建和测试任务，不作为独立性能基准。合并两种原尺寸方案后，在最终安装树复测该批最大原图 `4299×6068`（对齐至 `4320×6080`），输出与动漫批次逐字节一致；该图未达可靠人脸阈值。

正式质量审核、三平台发布验收以及两份权重的再分发核对仍未完成。当前启用官方智能裁剪发布会被门禁拒绝。
