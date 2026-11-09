# Mosaicraft Release Checklist

发布日期 / Applies to：v1.14.0 及后续版本

本清单定义正式发布的准入标准。Windows x64、Linux x64、macOS ARM64 三个平台的 CPU 包全部验收通过后，使用同一提交在本机完成 Windows CUDA GPU 验收。Windows 云端 CUDA 仍是候选；Linux CUDA 可作为正式附件，保留 GPU 未验收标记并通过完整回归、打包和独立验收。工作流不自动发布 Release。详细环境和缓存规则见 [CI 与打包](CI_PACKAGING.md)。

## 1. 发布前状态

- 工作区必须干净：`git status --short --branch`
- 当前版本号必须一致：`CMakeLists.txt`、`core/Version.h`、README、Web UI badge、测试断言、百科版本记录
- `docs/ENCYCLOPEDIA.md` 必须追加当前版本高层摘要和必要的详细说明
- 当前提交必须已推送到远端 `main`

## 2. 环境检查

```powershell
.\scripts\release.ps1 -InspectOnly -BuildDir build -Configuration Release
```

检查项：

- CMake、CTest、PowerShell 可执行文件可用
- vcpkg toolchain 可定位
- 发布所需文档和 license 文件存在
- `docs/ENCYCLOPEDIA.md` 存在，且包含当前版本记录
- 如果跳过构建，现有 build 输出目录内必须已有 CLI、WebUI 和 `index.html`

## 3. 本机 CUDA 发布包

```powershell
.\scripts\release.ps1 -BuildDir build/package-cuda-local -CudaValidation Required -OutputDir build/archives
```

脚本必须完成：

- CUDA Release 配置和构建
- 必需 GPU 探测通过；无可用 GPU 时失败，禁止跳过测试、smoke 和解压验证
- `mosaicraft_tests`
- `mosaicraft_regression_tests`
- Web UI/API smoke
- 生成 `Mosaicraft_v<version>_windows-x64_cuda.zip`
- 正式打包前 git 工作区必须保持干净，包内 `BUILD_INFO.txt` 才能准确对应 tag commit
- 解压发布包并验证包内 CLI `--version`
- 验证包内文档规则：必须包含 `ENCYCLOPEDIA.md`，不得包含 `CHANGELOG.md` 或 `RELEASE_NOTES_*`
- 写入并验证 `BUILD_INFO.txt`，记录版本、平台、架构、运行时、git commit 和 `GpuValidation: passed`
- 使用包内 `MosaicraftWebUI.exe` 再跑 Web UI/API smoke
- 输出 SHA256

记录发布包路径、大小和 SHA256。正式 release 附件必须使用脚本生成的 ZIP/tar.gz。

## 4. 远端 CPU-only CI 门禁

推送后检查 GitHub Actions：

```powershell
gh run list --branch main --limit 5
gh run view <run-id> --json status,conclusion,url,jobs
```

要求：

- 最新 `CI` workflow 必须通过
- Windows/Linux CPU 日常构建、CTest 和 Web UI/API smoke 必须通过
- 手动触发同一提交的 `Native packages`，三平台 CPU 构建、打包和独立附件验收必须全部通过
- 保存 CPU 包、Linux CUDA 包、SHA256 及工作流链接；Windows ZIP、Linux/macOS tar.gz
- Linux CUDA 使用 BuildOnly + AllowUnvalidatedCudaRelease，文件名不带 candidate，GpuValidation 仍为 not-run；通过编译、完整回归和独立包验收后才纳入 Release

说明：

- 首次或依赖变更后的 vcpkg install 可能较慢
- workflow 已启用 vcpkg binary cache，后续运行应复用缓存
- CPU-only CI 验证核心服务、API、Web UI、发布脚本和无 CUDA fallback

## 5. 创建 GitHub Release

仅当三平台 CPU 包和本机 Windows CUDA 包的同提交验收全部通过后创建 release。Linux CUDA tar.gz 通过完整回归和独立验收后进入正式附件，并注明 GPU 未实机验收；所有 `_candidate` 包不进入正式附件。创建 Release 仍由维护者显式执行，当前 CI 没有发布权限。

```powershell
gh release create v<version> `
  .\Mosaicraft_v<version>_windows-x64_cuda.zip `
  --title "Mosaicraft v<version>" `
  --notes-file <concise-release-page-notes.md>
```

GitHub Release 正文必须保持简洁，只包含：

- 各平台下载哪个 ZIP/tar.gz
- 解压后如何启动 `MosaicraftWebUI.exe`
- CLI 用户如何运行 `mosaicraft.exe --help`
- Windows 10 22H2 x64、Ubuntu 22.04/glibc 2.35、macOS 13 ARM64 及 CUDA 运行环境说明
- SHA256

完整变更、CI、发布脚本和验证细节保留在 `docs/ENCYCLOPEDIA.md`、README 和项目文档中，不直接展开到 GitHub Release 页面。

## 6. 发布后真实附件验收

创建或替换 GitHub Release 附件后，必须在对应平台重新下载真实附件验收。

```powershell
.\scripts\verify-release-asset.ps1 -Tag v<version> -AssetName <archive-name> -RequireStandalone
```

也可手动运行 `Verify published release` workflow，指定 `tag`。它在三个原生平台分别下载真实 Release 的五个正式附件（v1.14.8 起；较旧版本跳过 Linux CUDA），并执行相同的标签、SHA256、依赖路径和 Web UI/API 验收；不安装项目依赖。Windows CUDA 的云端附件检查不替代发布前的本机 GPU Required 验收。

脚本必须完成：

- 读取 GitHub Release 元数据并明确选择 ZIP/tar.gz 资产
- 下载真实 Release 附件
- 校验 GitHub asset digest、Release 页面 SHA256 和本地文件 SHA256 一致
- 解压附件并检查 CLI、Web UI、`index.html`、README、API、百科、`BUILD_INFO.txt`、license 和第三方版本清单
- 校验包内 `BUILD_INFO.txt` 的 commit 与 GitHub tag 指向的 commit 一致
- 确认包内没有 `CHANGELOG.md` 或 `RELEASE_NOTES_*`
- 验证包内 CLI `--version`、CLI `--help` 和 Web UI `--help`
- 使用包内 `MosaicraftWebUI.exe` 跑 Web UI/API smoke

## 7. 不允许发布的情况

- 本机 CUDA 构建或测试失败
- 发布包解压验证失败
- 发布包文档规则验证失败
- 远端 CPU-only CI 未完成或失败
- `docs/ENCYCLOPEDIA.md` 当前版本记录缺失
- 附件不是由 `scripts/release.ps1` 生成；Windows CUDA 不是 passed；Linux CUDA 未通过完整回归和独立验收，或隐瞒 not-run 状态
- 未记录 SHA256
- GitHub Release 真实附件未通过 `scripts/verify-release-asset.ps1`

## 8. 云端 CUDA 包

当前策略：

- GitHub hosted runner 生成 Windows CUDA 候选和 Linux CUDA 正式 tar.gz，均含 CUDA 13.3 七架构及对应 PTX，运行完整 CPU 回退测试和独立包验收
- Windows 候选使用 `-CudaValidation BuildOnly`，文件名带 `_candidate`，GPU 状态为 `not-run`
- Linux 正式包另加 `-AllowUnvalidatedCudaRelease`，保留 not-run，禁止跳过完整测试和验收；Linux 实机 GPU 验收尚未完成
- 本机 GPU 生成正式 Windows CUDA 包，Required 验收通过后状态才是 `passed`

本轮不配置自托管 GPU runner，不引入付费缓存或定时保活，也不修改已发布的 v1.14.6。

## 9. v1.14.0 发布候选验收

候选包使用 `-PackageSuffix rc1`；保留默认的构建、CTest、Web UI/API smoke、解压后验证和 SHA256 流程。正式发布仍执行前述远端 CI、tag 和真实附件验收。

- CUDA 和 CPU-only Release 各运行 CTest、Web UI/API smoke。
- 使用真实 GPU 检查特征工作区复用、末尾批次、分批评分相等和失败返回。
- 固定图库快照、目标图、`--topn-random 1`，关闭颜色调整；每轮恢复数据库，预热后测量三次取中位数。
- `scripts/benchmark.py --baseline-exe <old-exe>` 交替运行新旧版本，降低整机负载漂移的影响。
- 单独构建 `mosaicraft_ann_benchmark` 可排除索引加载时间，对比 708 维串行、196 维串行/并行，并检查完整查询结果。
- ANN 查询至少降低 30%；代表性总耗时回退不超过 5%；平均/P90 匹配分数恶化不超过 1%。未达到门槛的项目不得作为性能成果发布。
- 记录首次构建、进程峰值内存和显存采样口径；性能报告见 `docs/PERFORMANCE_v1.14.0.md`。

## CUDA 多架构补充检查

- 通用包必须包含 sm_75/80/86/89/90/100/120 及对应七种 PTX；发布脚本逐模块和最终 CLI/Web UI 程序检查，旧缓存中的单 sm_89 配置须先重新配置。
- Required 的预检必须加载全部实际内核和模板实例，并通过完整 GPU CTest、Web UI/API smoke；没有兼容设备不能用 CPU 回退冒充 GPU 验收。
- 核对 BUILD_INFO 的目标、PTX、GPU 名称、计算能力和驱动；`passed` 仅对应记录的实机。其余架构写明“已编译覆盖，尚未实机验证”。
- Windows BuildOnly 保持候选；Linux BuildOnly 可按本节明确规则作为正式附件。既有版本标签及附件保持不变。PTX JIT 通过不能替代其他型号的实机验证。

- 本机分别设置 CUDA_DISABLE_PTX_JIT=1 和 CUDA_FORCE_PTX_JIT=1 运行 GPU 必需回归，独立验证原生机器码与 PTX JIT；每次测试后恢复环境变量。
