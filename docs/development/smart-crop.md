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

运行摘要中的 `smartCropFacesFound`、`smartCropMoved`、`smartCropCentered` 和 `smartCropMainFaceClipped` 按图片计数。无需裁边的图片不计入；`smartCropInferenceWorkerMs` 是所有 worker 的逐图推理耗时之和，不包括启动时的模型校验和探测，也不等于批处理的墙钟时间。同尺寸特快路径保留旧归一化的行为，包括已有的 PNG 紧凑化规则。

## 模型锁与发布条件

- `cmake/smart-crop-models.json` 锁定三个权重的来源、大小和哈希。YuNet `2023mar` 有对应的 MIT 声明。原 U²-Net 权重的授权说明不能单独证明 rembg 发布附件的导出来源；DeepGHS 仓库模型卡标为 MIT，但所选权重的上游权利链还需核对。当前两项均未获准再分发。禁止把私人测试安装目录当作可发布附件。
- OpenCV 5.1.0 DNN 直接读取锁定的 DeepGHS 原版动态 ONNX，不改图、不重存权重。生产裁剪保持 `640×640` 输入，并校验 `output0` 为 `1×5×8400`；此前 OpenCV 4.12 本地验证用的是固定形状转换产物，两者的模型哈希不同。
- 三份权重的再分发依据和许可证齐备、独立人工标注的质量审核文档及两份可再分发的正例通过后，才可设置仓库变量 `MOSAICRAFT_SMART_CROP_RELEASE=1`（本地发布入口对应 `-DSMART_CROP=ON`）。归档验收用 `scripts/requirements-smart-crop-verification.txt` 的 Pillow 对照像素与人工标注，不属于解包后的运行依赖。
- 初审须覆盖偏侧真人、偏侧动漫、多人、主脸比窗口宽、无脸主体及无明显主体。分别统计两个模式在可保全主脸的样本上是否优于中心裁剪，以及中心裁剪已保全的主脸是否有新切断；另记录无脸主体效果、误移动率、批耗时及峰值内存。把批准的审核文档路径、大小和 SHA-256 写入 `qualityReview`；不得用两个解包正例代替总体质量审核。
- 解包正例锁定 PNG 和人工标注的 `mainFace: [left, top, right, bottom]`（半开坐标）。每张图高 320、宽大于 180，主脸能完整落入 `180×320`，但中心裁剪切脸。发布和独立验包从其他工作目录运行，以输出像素核对实际裁剪位置和完整主脸，同时检查检测和移动指标。任何目标平台失败都不能交付宣称完整支持的包。复验已有智能裁剪版本时，使用该版本的源码锁及标注。

## 2026-11-23 本地验收记录

- Windows x64 OpenCV 4.12：默认构建和启用构建均通过 7 项 CTest；命令转义测试通过。CLI 两路分别移走未选模型仍可运行；移走或篡改所选模型时失败且不创建输出目录。同尺寸、隐式和显式中心裁剪与 v1.15.14 对应输出逐字节一致，原图哈希不变。
- Windows x64 私人 CPU ZIP：两张人工画框的偏侧正例均检测到脸并移动裁剪框，复验确认输出像素完整覆盖标注主脸。HTTP API 两路成功并保留模式，非法值及非归一化用途返回 `400`；Chromium、Firefox 检查了选择器、生成的 CLI、参数恢复和旧后端的禁用保护。未做操作系统级断网验证或跨平台公开包验收。
- 同一台机器、每次输入一张、`--threads 1`：中心约 0.16–0.18 秒、进程峰值工作集约 12–13 MiB；真人约 1.67 秒、306.5 MiB；动漫约 2.05 秒、398.7 MiB。两份私人安装树用同样的 ZIP 工具压缩，开启后由 6,985,686 字节增至 27,880,433 字节（约 +19.93 MiB）。这不是批量基准或正式发布包体积。
- 隔离的 OpenCV **5.1.0** Windows CPU 实验：直接加载 DeepGHS 原版动态 ONNX（未改文件），用同一个网络实例交替输入 `640×640`、`320×320`、`640×384` 和 `960×960`，`output0` 分别为 `1×5×8400`、`1×5×2100`、`1×5×5040` 和 `1×5×18900`。同一动漫正例与原版 ONNX Runtime 相比，最大框坐标差约 0.00121 像素、置信度差约 2.81×10⁻⁶。YuNet 和 U²-NetP 也做了接口探测。5.1 新图执行器会提示不支持显式设置 target；这次单独测试使用 CPU wheel，不替代原生产品构建验收。

正式质量审核、三平台发布验收以及两份权重的再分发核对仍未完成。当前启用官方智能裁剪发布会被门禁拒绝。
