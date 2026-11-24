# v1.15.15 智能裁剪发布证据状态



## 放行还需要的文件


- 审核结果报告、带主脸标注的真人和动漫正例各一张。私人测试图和 HighLikes 原图不作为发布附件。取得这些文件后再写入模型锁的 `qualityReview` 和 `fixtures`，并锁定大小及 SHA-256。
- 专用 CUDA 包在真实 GPU 干净系统上的离线解包记录、正式故障与性能验收；结果绑定源码提交与归档 SHA-256，不能使用开发机安装树或 `BuildOnly` 日志替代。

## 候选打包与独立验收

运行 `cmake -DMODE=SmartCropCudaRedist -P cmake/PrepareRelease.cmake` 从两份固定的 NVIDIA 官方归档提取白名单。默认目录 `build/tools/smart-crop-cuda-redist` 只含 cuBLAS 13.10.0.20 的 2 个 DLL、cuDNN 9.28.1.4 的 8 个 DLL，以及 `licenses/cuda-smart-crop/` 下的两份原始许可。DLL 和许可均由 `cmake/smart-crop-cuda-redist-windows.json` 核对大小和 SHA-256，不携带 NVIDIA 驱动 DLL、整个 SDK、NVRTC 或未通过清单的 DLL。运行库清单是打包输入，不是项目再分发许可结论；公开分发前须确认 NVIDIA 条款（CUDA Toolkit 附件 A 和 cuDNN 补充协议）与本项目许可在该分发组合中的适用性。模型锁仍独立控制三个 ONNX 和两份正例的完整性。

使用 `PrepareRelease.cmake` 准备锁定依赖和模型，CUDA DNN 使用独立的 `smart-crop-cuda` 依赖树。`Release.cmake` 的本地候选参数为 `RUNTIME=cuda`、`CHANNEL=candidate`、`CUDA_VALIDATION=Required`、`SMART_CROP=ON`、`SMART_CROP_CUDA=ON`、`SMART_CROP_CUDA_REDIST=<绝对路径>`。指定独立的 `BUILD_DIR`、`OUTPUT_DIR`、`VCPKG_INSTALLED_DIR`。候选归档通过后不重新打包，用同一 SHA-256 做正式验收；独立验包还必须分别运行 CUDA 和显式 CPU 回退两路。v1.15.15 不生成单独的 CPU 智能裁剪包，普通 CPU 包保持不携带智能裁剪模型。

在有兼容 NVIDIA 驱动、无 Toolkit/cuDNN 且断网的 Windows x64 环境，用外部 `mosaicraft_release_check`、同提交的模型锁和标注，验证 ZIP 内 CLI 和 Web/API 的真人、动漫两路。确认实际 `device=cuda` 且主脸保全。保留日志，填写 JSON 键 `commit`、`sha256`、`platform`（`windows-x64`）、`gpu`、`driver`，以及布尔通过项 `offline`、`noToolkit`、`noCudnn`、`cliReal`、`cliAnime`、`webReal`、`webAnime`。独立执行 `VerifyPackage.cmake` 时以绝对路径传 `SMART_CROP_CUDA_OFFLINE_EVIDENCE`；脚本重新核对该 ZIP 的内容及 GPU 正例，不接受绑定到其他归档的记录。

## 2026-11-24 开发机复验（非正式验收）

测试的推理源码为 `c379ad509`。关闭智能裁剪、CPU 智能、CUDA 智能三种 Windows x64 构建各通过 7/7 项 CTest；可控故障覆盖 CUDA → CPU → CUDA、连续退避、需要重启时停止探测，以及批尾的模型契约错误。隐藏 GPU 后，`auto` 以 CPU 完成，严格 `cuda` 在创建输出前拒绝。未停止运行中的其他归一化任务。

在同一 RTX 4060 Laptop（8,188 MiB，驱动 621.14）上，用同一 CUDA 智能裁剪二进制分别指定 `cpu` / `cuda`，每种设备各跑三轮，输出 `180×320`。真人 5 张输入同源，不是独立的正式真人质量集；动漫为 HighLikes 96 张旧抽样加一张 `6534×8802` 原图。各轮零失败，输入 SHA-256 不变，两个设备的输出逐字节相同。

| 输入 | CPU 中位耗时 / 张每秒 | CUDA 中位耗时 / 张每秒 | 中位吞吐比 | CUDA 设备级最高采样 |
| --- | ---: | ---: | ---: | ---: |
| 真人 5 张，含 `6534×8802` | 9.41 s / 0.531 | 3.02 s / 1.654 | 3.11× | 5,184 MiB |
| 动漫 97 张，含 `6534×8802` | 248.13 s / 0.391 | 25.23 s / 3.844 | 9.83× | 6,798 MiB |

U²-NetP / 人脸适配器的整批耗时中位数（含预处理和解析）：真人 CPU 3.04/4.48 s、CUDA 0.10/1.11 s；动漫 CPU 71.83/166.50 s、CUDA 2.61/12.41 s。设备级显存含其他进程，不是单进程峰值；后台任务和同源真人样本使该结果不能替代正式性能与质量门禁。

用只保留 Windows 系统目录的 `PATH`，从另一工作目录运行两份**私人测试 ZIP**的解包结果。CLI 和 Web/API 的两种正例均通过；CLI 像素验证确认中心裁剪切到主脸，而 CPU 与 CUDA 智能结果都保全独立标注的主脸。CUDA ZIP 从解包目录加载白名单内 10 个 NVIDIA DLL；`auto` 处理最大动漫输入时报告实际使用 CUDA。CPU ZIP 不含 cuBLAS/cuDNN。此次只是开发机的运行时路径隔离，不是未安装 SDK 的系统验收。

| 本地测试文件（不可发布） | 大小（字节） | SHA-256 |
| --- | ---: | --- |
| `private-current-cpu-runtime-smoke.zip` | 30,890,652 | `941b0e06f680b12af529c8195f2de311fa202bcc51977e2659ee38995d53e7e3` |
| `private-cuda-runtime-smoke.zip` | 784,563,691 | `59358615df285ac3b87f447f3b1af6d64de1aa51db7114791bdeec7f27f9872d` |

上述私测 ZIP 相差 753,673,039 字节，不能据此填写正式包的校验清单。日志及原始性能报告位于本地 `build/v11515-local/`；样本和输出不入库。仍缺真实显存耗尽后释放并恢复、严格 CUDA 途中失败保留部分输出、独占环境的正式性能及干净 Windows 断网验包。CPU 与 CUDA 两路 `Release.cmake` 当前都在 U²-NetP 的权重证据门禁拒绝生成候选；未打标签、未上传任何附件。
