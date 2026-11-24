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

三平台 CPU 包来自同一提交，通过完整 CTest、Web UI/API smoke 和独立附件验收。工程变更另覆盖离线 HTML 浏览、回导、资源检查、ZIP64 与覆盖回滚；CLI/Web UI 参数改动检查命令生成。

```powershell
ctest --test-dir build/windows-x64 -C Release --output-on-failure
node tests/test_command_shell.js
```

## CUDA 与新后端

Windows 正式 CUDA 包使用 CUDA 13.5 SDK 与 Required 验收，在实际 GPU 上完整运行，不能跳过 CTest、smoke 或解压验证。云端 BuildOnly 保留 not-run；Linux 的未实机 CUDA 正式附件须按 CMake 入口的显式准入流程执行。

Windows/Linux x64 正式 Vulkan 包启用 Vulkan → CPU，并在独立无驱动环境验证回退；Metal 及显式选择的 Vulkan 构建仍为候选。不得把本机限定场景、其他品牌 GPU 的结果或编译通过写成该平台实机验收。跨后端 1% 质量门槛（素材 ID 差异仅作诊断）、CUDA 5% 回归门槛以及新后端性能准入详见[四后端验收](portable-gpu.md)。

使用 [CMake 发布入口](packaging.md#cmake-发布入口)构建 CPU、CUDA 和 Vulkan 包。

## 智能裁剪发布门禁

智能裁剪仅在 Native packages 手动运行时显式启用 `include_smart_crop` 才会进入 Windows CPU 包；标签触发的标准发布默认关闭该可选功能。开启前，权重再分发、人工质量审核和各目标平台解包验证都必须通过。参见[智能裁剪发布条件](smart-crop.md#模型锁与发布条件)。

## 归档包检查

- 包含 CLI、Web UI、页面资源、依赖、许可证、BUILD_INFO.txt 与 contracts.json。
- README 指向完整 docs/；保持 docs 的分类目录和所有内部链接，原始数据随同交付。
- 包含当前 API 参考、版本状态、GPU 验收与数据，历史文件留在 archive/。
- 不再将百科或 API 复制到包根。旧布局的已发布包仍由独立验证脚本识别。
- SHA256、版本、提交、平台、架构、运行时和验收字段一致。
- 在没有开发依赖的独立环境验证程序、中文/空格路径、Unix 权限和 macOS 签名。

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

只有已授权的版本发布才创建并推送 v* 标签，触发 Native packages。检查七个原生包和同一提交的五个 [Qt 桌面包](packaging.md#qt-桌面发布包)，收集 SHA-256 和验收日志。Windows 双后端桌面候选包不可作为正式附件，须用实机验收产物替代；Linux 的 GPU 未实测时标 `not-run`。发布时提供 Qt 许可证和对应源码来源。

为既有 Release 补发双后端桌面包时，手动运行 Native packages 并填 `desktop_release_tag`。该流程只构建和验包，不发布；核对两个归档的源提交均与原标签一致后，再追加附件并更新校验清单和发布说明，不移动标签。若还需从同一标签补建原生 CUDA 包，同时勾选 `include_native_cuda`。SDK 准备使用当前发布工具，产品和依赖锁仍取自原标签；Windows 正式包仍须实机验收。

如需在发布前重新独立验收整套原生 Actions 归档，运行 `Verify published release` 并填写原标签与完整打包的 `artifact_run` ID。它检查原标签提交，不把 Windows 云端 CUDA 候选当作正式包。

发布后运行 Verify published release 下载真实附件验证，并核对页面的候选、驱动和 not-run 说明。撤下候选时保留真实历史验收记录，更新[版本状态](../releases.md)，不要伪装为从未执行过测试。
