# 发布清单

本清单定义当前发布流程。平台、依赖与运行库策略见 [CI 与打包](packaging.md)，旧版验收记录见[历史发布清单](../archive/releases/release-checklist-legacy.md)。整理文档本身不创建标签或发布附件。

## 准备版本与说明

- 保持工作区干净，确认将要发布的提交及分支。
- 版本唯一来源为 contracts.json；同步当前指南、API 参考和 docs/releases.md 的适用范围。
- 更新版本状态中的对应软件版本标题和高层变更摘要，链接详细实现与验收。
- 更新受影响的验收记录，编译、实机和候选状态分别说明。运行[原生文档检查](documentation.md#检查与打包)。

```powershell
git status --short --branch
```

完整发布必须通过固定依赖准备、构建测试、打包和独立验证。

## 构建和回归

六个附件来自同一提交。原生包运行 CTest、Web UI/API smoke 和独立附件验收，Qt 包运行桌面归档检查。工程变更另覆盖离线 HTML 浏览、回导、资源检查、ZIP64 与覆盖回滚；CLI/Web UI 参数改动检查命令生成。

```powershell
ctest --test-dir build/windows-x64 -C Release --output-on-failure
node tests/test_command_shell.js
```

## 六个发布附件

| 平台 | CLI / Web UI | Qt 客户端 |
| --- | --- | --- |
| Windows x64 | CUDA + Vulkan 智能裁剪 ZIP | CUDA + Vulkan Qt ZIP |
| Linux x64 | CUDA + Vulkan 智能裁剪 tar.gz | CUDA + Vulkan Qt tar.gz |
| macOS ARM64 | CPU 智能裁剪 tar.gz | CPU Qt tar.gz |

`cmake/release-packages.json` 是构建与独立验包共用的矩阵。正式集合由 `python scripts/verify_release_set.py v<版本> --directory <附件目录>` 按文件名和校验值检查，并生成 `SHA256SUMS.txt`。旧运行时仍可单独构建以复查历史包。

三个智能裁剪包均从模型锁准备真人、动漫与显著性 ONNX、许可和标注正例。两个 GPU 包复用同一份哈希锁定的 ncnn 转换模型，CUDA 使用 CUTLASS。云端以仓库 Secret `SMART_CROP_FIXTURE_URL` 读取正例 ZIP，内部保留模型锁中 `real/input/positive.png` 与 `anime/input/positive.png` 两个路径。公开包内只包含模型，不包含验收图片。本机可设 `SMART_CROP_FIXTURE_ROOT` 使用已保存的正例。

Windows 云端的两个 GPU 包为候选。正式附件在同提交上用 Required 模式构建并在实机验收，智能裁剪包的 CUDA、Vulkan 与显式 CPU 均运行真人和动漫正例。Linux 云端包按现有显式准入流程标记 GPU 为 `not-run`，并执行独立解包和 CPU 正例检查。[本机打包入口](packaging.md#cmake-发布入口)

## 归档包检查

- 包含 CLI、Web UI、页面资源、依赖、许可证、BUILD_INFO.txt 与 contracts.json。
- README 指向完整 docs/；保持 docs 的分类目录和所有内部链接，原始数据随同交付。
- 包含当前 API 参考、版本状态、GPU 验收与数据，历史文件留在 archive/。
- 不再将百科或 API 复制到包根。旧布局的已发布包仍由独立验证脚本识别。
- SHA256、版本、提交、平台、架构、运行时和验收字段一致。
- 独立解包，检查程序、中文/空格路径、Unix 权限和 macOS 签名。按各包型要求隔离开发依赖；CUDA 智能裁剪包的独立解包检查不要求另找无开发依赖的断网 Windows 系统。

## 使用原生验包器

用同提交构建产物中的 `mosaicraft_release_check`，或在独立的本机构建中生成它。这个工具不依赖 OpenCV、CUDA 或 Vulkan，也不从待验包取出。使用 Windows 时将工具路径指向 `.exe`，多配置生成器一般放在 `Release` 子目录。

```sh
cmake -S tools/release-check -B build/check-tool -DCMAKE_BUILD_TYPE=Release
cmake --build build/check-tool --config Release
cmake -DARCHIVE=<archive> -DEXPECTED_SHA256=<sha256> -DEXPECTED_COMMIT=<commit> -DCHECK_TOOL=<native-check> -P cmake/VerifyPackage.cmake
cmake "-DTAG=v1.15.10" -DASSET=<release-asset-name> -DCHECK_TOOL=<native-check> -P cmake/VerifyPackage.cmake
```

本地模式不联网，SHA256 应取自信任的旁文件或发布记录，提交来自对应的构建记录或版本标签。标签模式使用 `gh` 获取发布附件及摘要、解析标签提交。两种方式都在隔离的中文及空格目录内执行 CLI 和 Web UI/API 验收；尚未提供 `contracts.json` 的 v1.14.7–v1.14.11 使用旧版 CLI 命令与 API 连通性检查；正式 Vulkan 会隐藏 ICD 验证回退。从 Actions 下载的目录也可以用 `-DARCHIVE_DIR=<目录>` 自动读取唯一归档和同名 `.sha256`。

## 发布与复核

只有已授权的版本发布才创建并推送 v* 标签，触发 Native packages。收集三份智能裁剪包和三份 [Qt 桌面包](packaging.md#qt-桌面发布包) 的 SHA-256 和验收日志。以同提交的实机验收产物替换两个 Windows 候选，再核对正式六件套。发布时提供 Qt 许可证和对应源码来源。

为既有 Release 补发双后端桌面包时，手动运行 Native packages 并填 `desktop_release_tag`。该流程只构建和验包，不发布；核对两个归档的源提交均与原标签一致后，再追加附件并更新校验清单和发布说明，不移动标签。SDK 准备使用当前发布工具，产品和依赖锁仍取自原标签；Windows 正式包仍须实机验收。

如需在发布前重新独立验收整套原生 Actions 归档，运行 `Verify published release` 并填写原标签与完整打包的 `artifact_run` ID。它检查原标签提交，不把 Windows 云端 CUDA 候选当作正式包。

发布后运行 Verify published release 校验六个实际附件，并核对驱动及 not-run 说明。撤下候选时保留真实历史验收记录，更新[版本状态](../releases.md)，不要伪装为从未执行过测试。
