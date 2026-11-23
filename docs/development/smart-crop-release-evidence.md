# v1.15.15 智能裁剪发布证据状态

## 已有证据

- 维护者确认权重可随包分发、已审核样本质量满足要求。这两项确认不等同于仓库中已有可审计的附件清单和审核报告。
- YuNet `2023mar` 的固定权重已有随包许可声明。DeepGHS 的本地上游模型卡标为 MIT；以模型锁记录的提交和文件哈希为准。
- U²-Net 发布者在 [权重许可说明](https://github.com/xuebinqin/U-2-Net/issues/208) 中确认原仓库权重适用仓库许可（Apache-2.0）。这不同于把 rembg 代码的 MIT 许可套到具体 ONNX 发布附件上；该附件需用来源记录和适用声明关联到锁定的 SHA-256。
- HighLikes 既有 RTX 4060 Laptop 严格 CUDA 全量批次完成 12,340 / 12,340，零失败；它是自动设备策略变更前的基线，不是新包验收。

## 放行还需要的文件

- 权重审核所用的具体来源及许可声明，尤其是 rembg ONNX 导出物；DeepGHS 提交对应的随包声明。不要仅改写 `redistributionVerified`。
- 审核结果报告，以及获准公开分发、带主脸标注的真人和动漫正例各一张。私人测试图和 HighLikes 原图不作为发布附件。取得这些文件后再写入模型锁的 `qualityReview` 和 `fixtures`，并锁定大小及 SHA-256。
- 目标 ZIP 的 NVIDIA 运行库白名单、许可证、真实 GPU 离线解包记录；结果绑定源码提交与归档 SHA-256，不能使用开发机安装树或 `BuildOnly` 日志替代。

## 候选打包与独立验收

批准 NVIDIA 运行时清单后，提交 `cmake/smart-crop-cuda-redist-windows.json`。其 `files` 和 `notices` 数组各包含 `{file, size, sha256}`，分别对应已审批的 DLL 和许可文本。准备独立的 redist 目录：DLL 在根目录，许可在 `licenses/cuda-smart-crop/`；不要传整个 SDK。模型锁仍独立控制三个 ONNX 和两份正例的完整性。

使用 `PrepareRelease.cmake` 准备锁定依赖和模型，CUDA DNN 使用独立的 `smart-crop-cuda` 依赖树。`Release.cmake` 的本地候选参数为 `RUNTIME=cuda`、`CHANNEL=candidate`、`CUDA_VALIDATION=Required`、`SMART_CROP=ON`、`SMART_CROP_CUDA=ON`、`SMART_CROP_CUDA_REDIST=<绝对路径>`。指定独立的 `BUILD_DIR`、`OUTPUT_DIR`、`VCPKG_INSTALLED_DIR`。候选的归档不重新打包，用同一 SHA-256 做正式验收。

在有兼容 NVIDIA 驱动、无 Toolkit/cuDNN 且断网的 Windows x64 环境，用外部 `mosaicraft_release_check`、同提交的模型锁和标注，验证 ZIP 内 CLI 和 Web/API 的真人、动漫两路。确认实际 `device=cuda` 且主脸保全。保留日志，填写 JSON 键 `commit`、`sha256`、`platform`（`windows-x64`）、`gpu`、`driver`，以及布尔通过项 `offline`、`noToolkit`、`noCudnn`、`cliReal`、`cliAnime`、`webReal`、`webAnime`。独立执行 `VerifyPackage.cmake` 时以绝对路径传 `SMART_CROP_CUDA_OFFLINE_EVIDENCE`；脚本重新核对该 ZIP 的内容及 GPU 正例，不接受绑定到其他归档的记录。
