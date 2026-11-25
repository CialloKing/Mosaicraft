# Windows Vulkan 与双 GPU 智能裁剪实验

## 结论

在 Windows x64 上跑通了 Radeon 780M 的 FP32 真人裁剪，以及 RTX 4060 Laptop（CUDA）与 780M（Vulkan）按图分流。32 张真人样本的输出文件名、裁剪坐标和 SHA-256 均一致。**不建议启用这两个候选路径**：单 Vulkan 比 CPU 快 55.4%，但超出内存门槛；优化后双 GPU 比原双 GPU 快 6.5%，但仍比单 CUDA 慢 17.9%；扩大至 1,000 项复测后仍慢 12.7%。原生动漫模式现已支持显式双 GPU 实验：保留动态原图推理，将坐标网格移至后处理。HighLikes 的 1,000 张原图已验证输出一致；限制核显分流后，双 GPU 中位墙钟仍比单 CUDA 慢 2.8%。现有默认设备、CPU、自动回退和发布配置保持不变。

## 如何复现

- 工具链：MSVC 19.51.36257、CUDA 13.5.37、OpenCV 5.1.0 CUTLASS；应用的 CUDA 目标为 `sm_89` 和 `compute_89`。ncnn/pnnx 锁定为 `20261020`（ncnn 提交 `7e36d25cb153092ba87a60530455c7376fedf452`）。
- 使用 `scripts/smart_crop_vulkan_probe` 构建探针；`ncnn_DIR` 指向解压的 Windows x64 静态 Vulkan SDK 的 `x64/lib/cmake/ncnn`。不带参数启动探针可枚举 Vulkan 适配器；设备索引不是跨机器固定值。
- 将两个锁定的工具 ZIP 和解压的 pnnx 放在工具目录。用装有 Python `onnx` 的环境运行下列命令。工具及原始 ONNX 的哈希必须匹配；报告记录每个模型的转换产物哈希和在指定设备上的执行结果。

```powershell
python scripts/prepare_smart_crop_vulkan.py <锁定ONNX目录> <工具目录> <新转换目录> `
  --probe <探针EXE> --device 1 --max-real 6534 8802 --max-anime 1664 2496
```

U²-NetP（前景显著性）在固定的既有 320×320 输入上通过。YuNet 只放开 ONNX 的输入宽高和检测输出长度元数据，没有改算子或权重；在 352×256、640×384、352×256、6560×8832、352×256 的同会话序列上通过。动漫原始模型的 `Range` 等动态网格算子不能直接转换。现在从锁定 ONNX 中抽出网格解码前的子图，主干、分布回归（DFL）与置信分数仍在 GPU 执行；主机只对超过原置信阈值的候选恢复坐标，然后复用原来的边界过滤、NMS 和大图复检。未更改权重、FP32、输入分辨率或尺寸对齐规则。探针在同一会话验证非方形输入、尺寸往返及 1,664×2,496 压力输入。

在独立的 Windows x64 智能裁剪构建中设 `MOSAICRAFT_SMART_CROP_VULKAN=ON`（默认 OFF）和 `ncnn_DIR`。在 EXE 旁的 `models/vulkan` 放入通过探针、哈希匹配的 `saliency` 和所选模式的 `real` 或 `anime` 各一对 `.param`、`.bin` 文件（把转换得到的 `model.ncnn.*` 按模型名重命名）；保留原 `models` 中的 ONNX。

CLI 的 `library normalize --crop-device vulkan` 和 HTTP/WebUI 的同名策略仅用于真人归一化。`cuda+vulkan` 支持真人和动漫归一化，要求另一块 AMD/Intel Vulkan 适配器、至少 8 张图和 4 个应用线程。默认自动选适配器，也可用 `MOSAICRAFT_SMART_CROP_VULKAN_DEVICE` 指定 ncnn 枚举索引。实验选项不保证比单 CUDA 快；严格预检失败，不回退，也不用于创建或追加图库。

旧基准仍可用 `MOSAICRAFT_SMART_CROP_EXPERIMENT=vulkan|dual` 加显式 Vulkan 枚举索引，配合默认 `--crop-device auto`。该环境开关仅由 CLI 识别，不会为 Web 任务选设备。

双 GPU 至少需要 8 张图、4 个应用线程及另一块 AMD/Intel Vulkan 适配器；本次只实测 AMD。核显至多领取对齐后 4 Mi 像素的可裁剪图；动漫实验默认按实测收紧至 0.75 Mi 像素，超过上限由独显以原图尺寸处理。可以用 `MOSAICRAFT_SMART_CROP_DUAL_MAX_PIXELS` 指定 1,024–4,194,304 像素，不改变模型分辨率。最后三张不再派给核显。两个设备独立领取任务，已完成的结果在有界队列里等待按输入索引写出。读预取保留 64 MiB，活动、待写和正在写的缓冲共最多 192 MiB、最多 8 项；超限单图排空队列后由 CUDA 独占处理。这个预算不限制模型、驱动内存或阶段内部的临时分配。遇到推理错误，只写完最早失败索引之前的输出。

## 首轮性能与一致性（原顺序交接调度）

Windows 10 22H2（19045），Ryzen 9 7940H，独显 RTX 4060 Laptop（NVIDIA 621.14），核显 Radeon 780M（AMD 32.0.21030.2001）。用同一实验 CLI、32 张独立真人图、180×320 输出，四种路径各交错运行三轮，按进程启动至结束计时：

```powershell
python scripts/bench_smart_crop_cuda.py <实验CLI> <32张真人图目录> <新报告目录> `
  --study heterogeneous --mode smart-real --rounds 3 --vulkan-device 1 --with-dual
```

| 路径 | 三轮整批墙钟（秒） | 中位墙钟（秒） | 峰值工作集的中位值（MiB） |
| --- | --- | ---: | ---: |
| CPU | 35.984 / 36.528 / 36.126 | 36.126 | 2,911.0 |
| CUDA | 6.169 / 5.975 / 6.135 | 6.135 | 1,114.7 |
| Vulkan | 16.251 / 15.900 / 16.123 | 16.123 | 7,957.4 |
| CUDA + Vulkan | 7.705 / 7.762 / 7.721 | 7.721 | 1,479.9 |

- 12 次运行均完成 32/32；输入哈希不变，没有新增失败或隐式 CPU 推理回退。单 Vulkan 与 CPU、双 GPU 与 CUDA 的全部输出文件 SHA-256 和 ROI 完全一致。同轮 CUDA 和 CPU 的结果也相同。
- **单 Vulkan** 通过相对 CPU 至少快 10% 的门槛，但峰值工作集比 CPU 增加约 5,047 MiB，远超 512 MiB 上限。加入双 GPU 之前的独立三轮对照也重现这一结果（CPU 35.88 s、CUDA 6.12 s、Vulkan 16.10 s）。
- **双 GPU** 工作集比 CUDA 多 364–367 MiB，过了 512 MiB 门槛，但距离相对最快单设备至少节省 5% 的目标还有差距。三轮均分给 CUDA 24 张、Vulkan 8 张；初始化中位耗时由 0.46 s 增至 1.12 s，两路按序交接累计等待输入或队首约 6.25 s。后者含正常推理等待，不能直接当作可节省时间。
- WDDM 每秒采样，以下为三轮中观测到的最高值（MiB，适配器全局，含桌面与其他进程）：

  | 路径 | RTX 4060 专用 / 共享 | Radeon 780M 专用 / 共享 |
  | --- | ---: | ---: |
  | CPU | 1,184.1 / 46.2 | 50.8 / 48.1 |
  | CUDA | 3,043.6 / 114.1 | 50.8 / 48.1 |
  | Vulkan | 1,184.1 / 46.3 | 310.5 / 7,632.8 |
  | 双 GPU | 3,043.7 / 114.3 | 281.9 / 312.5 |

  RTX 和 780M 分别对应 LUID `0x00000000_0x00011105`、`0x00000000_0x00013f09`。峰值不一定同时出现，核显共享显存与进程工作集不能相加。250 ms 的 `nvidia-smi` 采样中，扣除启动前基线，单 CUDA 为 1,881–2,055 MiB，双 GPU 为 1,899–1,990 MiB。

基准最终退出码 1 表示候选未过准入门槛，不表示上述裁剪命令运行失败。

## 独立调度复测

原实现把推理槽的释放绑到按序交接。现在 CUDA 和 Vulkan 各自取图、完成即释放设备，只在结果队列达到条数或内存上限时产生背压。顺序仍由写线程保证，两套会话也改为并行预检，全部成功后才创建输出目录。YuNet 的小尺寸 Vulkan 路径将九个 FP32 检测输出放在同一次命令提交中读回，较大的图仍用原逐输出路径。

```powershell
python scripts/bench_smart_crop_cuda.py <旧双GPU-CLI> <32张真人图目录> <新报告目录> `
  --study dual-dispatch --candidate-exe <新CLI> --mode smart-real --rounds 3 --vulkan-device 1
```

在同一台机器、同一 OpenCV DNN DLL、同一份原始输入上，分别交错测量。墙钟包含预检和输出：

| 32 张真人图对照 | 单 CUDA | 旧双 GPU | 新双 GPU | 相对旧双 GPU | 相对单 CUDA |
| --- | ---: | ---: | ---: | ---: | ---: |
| 仅解耦调度 | 5.923 s | 7.406 s | 7.256 s | 快 2.0% | 慢 22.5% |
| 加并行预检及合并读回 | 5.886 s | 7.420 s | 6.938 s | 快 6.5% | 慢 17.9% |

两组各 9 次运行均通过逐文件 SHA-256、ROI、输入不变和无 CPU 回退检查。最终 32 张方案每轮为 CUDA 24 张、Vulkan 8 张，两路裁剪区间重叠 1.67–1.70 秒；该指标包括裁剪阶段的主机预处理，不代表 GPU 内核同时运行了同样长的时间。两套会话的预检中位数从约 1.01 秒降到 0.62 秒，工作集比单 CUDA 多约 364–368 MiB。回归中通过阻塞注入验证：核显的队首图未完成时，CUDA 能继续领取后续图片；核显注入错误后，已完成的后续图也不会写出。

另把同一组 32 张图排成三组，共 96 项，用于看启动成本摊薄后的吞吐，不计作 96 张新的独立样本。每条路径仍交错测三轮，输出完全一致。单 CUDA **16.897 s**、旧双 GPU **20.128 s**、新双 GPU **19.434 s**：新双 GPU 比旧双 GPU 快 **3.4%**，仍比单 CUDA 慢 **15.0%**。新路径每轮为 CUDA 68 项、Vulkan 28 项，两路裁剪区间重叠 5.84–5.92 秒，峰值工作集比单 CUDA 多 328–332 MiB。

另一次 96 项运行中，按 Mosaicraft 进程 PID 和两块卡的 LUID 查 WDDM `GPU Engine`：约 1 秒的 10 个采样窗口里，有 **8 个窗口**同时记录到 RTX 4060 的 `CUDA` 引擎和 780M 的 `Compute 0` 引擎活动。这证明同一进程确实在调度两块硬件，而不是只创建了两支 CPU 线程；没有用这组低频采样推断内核级重叠时长。

本轮的普通尺寸回归仍只跳过坏图，同尺寸 PNG 未变，旧 Vulkan 与新 Vulkan 的输出也一致。无效设备和缺失 Vulkan 模型均在创建输出前拒绝。原合成超大图仍由 Vulkan 成功处理；双 GPU 仍在同一张图遇到同一个 CUTLASS 错误，保留相同的已完成前缀。

本地证据：`parallel-32/report.json`、`parallel-batched-32/report.json`、`parallel-batched-96/report.json`、`parallel-wddm-engine/positive-per-process.csv`、`parallel-regression/report.json` 和 `ctest-parallel-batched.log`。**并行已经验证，但两组的相对单 CUDA 提速门槛均未通过，双 GPU 继续作为实验。**

### 1,000 项启动开销复测

同样的 32 张原图按原排序循环至 1,000 项（每张重复 31–32 次）。这是一轮长批次吞吐复测，不是 1,000 张独立样本，也不替代上面的三轮对照。三条路径在同一台设备上依次运行，全程重新执行解码、推理和写盘；原图和暂存输入 SHA-256 不变。

```powershell
python scripts/bench_smart_crop_cuda.py <旧双GPU-CLI> <32张真人图目录> <新报告目录> `
  --study dual-dispatch --candidate-exe <新CLI> --mode smart-real --rounds 1 `
  --vulkan-device 1 --repeat-to 1000 --timeout 600
```

| 路径 | 整批墙钟 | 模型预检 | 墙钟扣除模型预检 | 吞吐量 |
| --- | ---: | ---: | ---: | ---: |
| 单 CUDA | 164.43 秒 | 0.47 秒 | 163.96 秒 | 6.08 项/秒 |
| 旧双 GPU | 188.95 秒 | 1.03 秒 | 187.92 秒 | 5.29 项/秒 |
| 新双 GPU | 185.36 秒 | 0.61 秒 | 184.75 秒 | 5.39 项/秒 |

模型预检只占墙钟的 0.29%–0.54%。新双 GPU 比旧双 GPU 快 1.9%，但仍比单 CUDA 慢 12.7%；扣除预检后仍慢 12.7%，因此该差距不是模型预检造成的。扣除项只包括 CLI 报告的模型预检，不是全部进程启动成本。三条路径均完成 1,000/1,000 项，逐项文件名、ROI 和 SHA-256 一致，无 CPU 回退。新双 GPU 分配 CUDA 691 项、Vulkan 309 项，裁剪阶段重叠约 63.74 秒，峰值工作集比单 CUDA 多约 327 MiB。

本地报告：`parallel-batched-1000/report.json`。退出码 1 对应未达到相对单 CUDA 的提速门槛，三条路径本身均退出 0。

### HighLikes 的 1,000 张动漫原图（接通前的基线）

从 `library/HighLikes` 顶层 12,339 张 JPG/PNG 中按 `SHA-256("HighLikes-dual-gpu-1000-v1" + NUL + 文件名)` 排序抽样、按内容 SHA-256 去重，再按原文件名排序暂存。每个候选路径顺序跑一轮，输出 180×320、4 个应用线程；不修改、不缩小输入原图。两组样本的暂存输入及原图哈希均未改变。

- **原生动漫模式（`smart-anime`）：**单 CUDA 流水线完成 1,000/1,000 张，整批 **93.33 秒（10.71 张/秒）**，无失败或 CPU 回退。其中 996 张执行了智能裁剪。双 GPU 在创建输出目录前因不支持动漫动态模型而拒绝；没有原生动漫模式的双 GPU 耗时。
- **同批插图、真人模型（`smart-real`）：**单 CUDA、旧双 GPU、新双 GPU 都在第 266 张（4,465×7,805）遇到相同的 CUTLASS `Error Internal`。三条路径保留的 265 张输出文件名、SHA-256 与裁剪坐标一致。该失败不归类为 OOM，三条中止耗时不能作为整批吞吐对照。
- **真人模型的尺寸受限对照：**用同一抽样顺序，只取对齐到 32 后不超过 4 Mi 像素的原图，补足 1,000 张（与首组重合 788 张），与当前核显分流阈值一致。不是原生动漫模式的性能结果，也不代表全库尺寸分布。

| 尺寸受限的插图，使用 `smart-real` | 整批墙钟 | 吞吐量 | 峰值工作集（MiB） |
| --- | ---: | ---: | ---: |
| 单 CUDA | 51.88 秒 | 19.28 张/秒 | 332.94 |
| 旧双 GPU | 64.63 秒 | 15.47 张/秒 | 1,219.77 |
| 新双 GPU | 64.62 秒 | 15.48 张/秒 | 1,400.16 |

三路均完成 1,000/1,000 张，逐文件名、SHA-256 及 996 条裁剪坐标一致，无 CPU 回退。新双 GPU 分流 CUDA 673 张、Vulkan 323 张；裁剪阶段重叠约 62.80 秒，但整批仍较单 CUDA 慢 **24.6%**，峰值工作集多约 **1,067 MiB**。与旧双 GPU 的差值不足 0.01 秒，单轮不能判为提速。此时原生动漫双 GPU 尚不可用。

本地证据位于 `build/smart-crop-vulkan-dual-gpu/highlikes-anime-1000`：`sample.json`、`smart-anime/report.json`、`unsupported.json`、`smart-real-dispatch/report.json`、`size-bounded/sample.json` 和 `size-bounded/smart-real-dispatch/report.json`。

### 原生动漫双 GPU：接通与多轮优化

在 RTX 4060 Laptop + Radeon 780M 上，用上面的同一份 HighLikes 1,000 张原图和 180×320 输出复测，不剔除大图。最初按 4 Mi 像素分流：单 CUDA **105.83 秒**，双 GPU **125.48 秒**；Vulkan 完成了 292 张原生动漫裁剪，输出逐项一致，但墙钟慢 18.6%。

移除对齐 NCHW 张量的一次主机复制后，从这 1,000 张等距取 200 张做两轮交错测量。四档分流上限均满足一致性；下表比较同一二进制上的中位墙钟，不能把降幅单独归功于消除复制：

| 路径 | 200 张中位墙钟 | 相对单 CUDA |
| --- | ---: | ---: |
| 单 CUDA | 20.66 秒 | — |
| 双 GPU · 4 Mi 像素 | 25.07 秒 | 慢 21.4% |
| 双 GPU · 2.5 Mi 像素 | 24.82 秒 | 慢 20.1% |
| 双 GPU · 1.5 Mi 像素 | 22.90 秒 | 慢 10.8% |
| 双 GPU · 0.75 Mi 像素 | 21.15 秒 | 慢 2.4% |

0.75 Mi 像素比同一轮的 4 Mi 双 GPU 用时少 **15.6%**，因此只将**显式双 GPU 动漫实验**的默认分流上限设为该值。用全部 1,000 张做三轮交错对照；这是分流上限，不是降分辨率：

```powershell
python scripts/bench_smart_crop_cuda.py <实验CLI> <HighLikes的1000张暂存原图> <新报告目录> `
  --study dual --mode smart-anime --rounds 3 --vulkan-device 1
```

| 路径 | 三轮墙钟 | 中位墙钟 |
| --- | ---: | ---: |
| 单 CUDA | 104.78 / 93.15 / 93.22 秒 | **93.22 秒** |
| 双 GPU · 0.75 Mi 像素 | 105.57 / 95.42 / 95.85 秒 | **95.85 秒** |

六次运行均完成 1,000/1,000 张，文件名、输出 SHA-256、996 条裁剪坐标和输入哈希一致，无 CPU 回退。Vulkan 每轮处理 79–81 张；两路裁剪阶段重叠 14.71–15.00 秒（含主机预处理），峰值工作集较单 CUDA 增加约 376–383 MiB。**双 GPU 中位墙钟仍慢 2.8%，未过至少节省 5% 的门槛，不改变普通 CUDA 或 `auto` 默认路径。**

每轮核显在 79–81 张上的显著性阶段累计约 11 秒，动漫检测约 3.6–3.7 秒。这提示下一步应测只分担耗时较短的检测阶段、避免核显重复承担较慢的显著性阶段，而非把更多整图派给核显；这里的计时不是纯 GPU 内核时间。

本地报告：`anime-dual-baseline-1000/report.json`、`anime-dual-tune-200/report.json`、`anime-dual-final-1000/report.json`、`repro-anime-dynamic/report.json`，均位于 `build/smart-crop-vulkan-dual-gpu`。

## 回归与包体积

- 动漫更新后重新构建 CLI 和测试目标，CTest 单元及回归 2/2 通过。真人 32 张单 CUDA / 双 GPU 输出一致，真人的 4 Mi 像素阈值不变；动漫缺权重、非法分流上限、单 Vulkan 动漫请求均在创建输出前拒绝。HighLikes 的 1,000 张原图 SHA-256 再次核验未变。本地回归证据：`ctest-anime-dual-final.log`、`anime-dual-validation/status.json` 及同目录 `real-report/report.json`。
- CTest 单元和回归目标 2/2 通过，覆盖双路任务乱序完成、推理异常时只写正确前缀，以及原 CUDA `auto` 回退与恢复。
- 普通尺寸夹具共 9 个输入。串行 CUDA、流水 CUDA、Vulkan、双 GPU 都只跳过索引 1 的坏图，输出其余 8 张；同尺寸 PNG 与输入字节相同，四条路径的所有输出文件名、SHA-256 和 ROI 相同。关闭实验的构建收到 Vulkan 请求时在创建输出前拒绝。无效设备索引、双路指定 NVIDIA Vulkan 设备、此前未接通的动漫模式均在预检拒绝。真人新建图库保留原 CUDA 智能裁剪，共 8 张入库、跳过 1 张坏图。
- 混合尺寸压力夹具中，Vulkan 处理了 6534×8802 的合成大图；历史 cuDNN 对照的 8 个原有效压力输入，Vulkan 的 SHA-256 和 ROI 均匹配。当前 CUTLASS 单 CUDA 和双 GPU 都在这张大图的 `cutlass_convolution.cu` 报 `Error Internal`，两者保留相同的一个已完成文件，后续未写出。**根因未证实，不归类为 OOM，也不能宣称大图全路径通过。**
- 首轮打包时，从同一源码和同一组 CUTLASS/OpenCV DLL 构建，唯一实验构建差异是 Vulkan 适配器开关及其 SDK。用相同的 Deflate 9 做两个本地 CLI 审计包（不含 WebUI）：

  | 本地包 | ZIP（MiB） | 解包（MiB） |
  | --- | ---: | ---: |
  | Vulkan OFF | 43.01 | 175.75 |
  | Vulkan ON | 53.49 | 198.76 |
  | 增量 | 10.48 | 23.01 |

  ZIP 的增量约有 6.14 MiB 来自静态链接后变大的 EXE、4.23 MiB 来自新增的 4 个转换模型文件，其余为 OpenMP 运行库及许可/构建说明。普通和延迟导入扫描中，候选 CLI 只新增了 `VCOMP140.DLL`（锁定的 ncnn SDK 使用 MSVC OpenMP），没有 ncnn DLL、cuBLAS、cuBLASLt 或 cuDNN。从两份 ZIP 解包、将 PATH 限于 Windows 系统目录后运行也成功；进程采样见到了系统的 `vulkan-1.dll`、AMD 驱动的 `amdvlk64.dll` 和包内 `VCOMP140.DLL`，未观测到上述旧推理库。该测试仍在本机，**不是一台未装 Toolkit 的干净系统验收**。
- ZIP、二进制、权重、原图和采样日志均留在本地，未提交，未发布。

本地证据根目录为 `build/smart-crop-vulkan-dual-gpu`：模型 `repro/report.json`；基准 `single-32/report.json`、`dual-32/report.json` 及同目录的采样 CSV；回归 `ctest.log`、`regression-small-final/report.json`、`regression/report.json`、`regression/historical-cudnn-parity.json`；打包与加载 `zip-audit-final/report.json`、`zip-audit-final/smoke-report.json` 及模块清单。

后续仍需排查真人模式的 CUTLASS 大图错误和单 Vulkan 的大图工作集。本实验未接入 NPU，Intel 核显与其他独显也不在已实测范围内。
