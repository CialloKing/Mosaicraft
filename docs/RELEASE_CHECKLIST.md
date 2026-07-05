# Mosaicraft Release Checklist

发布日期 / Applies to：v1.13.9 及后续补丁版本

本清单定义正式发布的准入标准。正式 CUDA 包仍由本机或专用 GPU 环境构建；GitHub Actions CPU-only CI 是基础门禁，不替代 CUDA 发布包。

## 1. 发布前状态

- 工作区必须干净：`git status --short --branch`
- 当前版本号必须一致：`CMakeLists.txt`、`core/Version.h`、README、Web UI badge、测试断言、release notes
- 对应 release notes 必须存在：`RELEASE_NOTES_v<version>.md`
- 当前提交必须已推送到远端 `main`

## 2. 环境检查

```powershell
.\scripts\release.ps1 -InspectOnly -BuildDir build -Configuration Release
```

检查项：

- CMake、CTest、PowerShell 可执行文件可用
- vcpkg toolchain 可定位
- 发布所需文档和 license 文件存在
- 对应 release notes 存在
- 如果跳过构建，现有 build 输出目录内必须已有 CLI、WebUI 和 `index.html`

## 3. 本机 CUDA 发布包

```powershell
.\scripts\release.ps1 -BuildDir build -Configuration Release
```

脚本必须完成：

- CUDA Release 配置和构建
- `mosaicraft_tests`
- `mosaicraft_regression_tests`
- Web UI/API smoke
- 生成 `Mosaicraft_v<version>.zip`
- 解压发布包并验证包内 CLI `--version`
- 使用包内 `MosaicraftWebUI.exe` 再跑 Web UI/API smoke
- 输出 SHA256

记录发布包路径、大小和 SHA256。正式 release 附件必须使用脚本生成的 zip。

## 4. 远端 CPU-only CI 门禁

推送后检查 GitHub Actions：

```powershell
gh run list --branch main --limit 5
gh run view <run-id> --json status,conclusion,url,jobs
```

要求：

- 最新 `CI` workflow 必须通过
- `Windows CPU-only` job 必须通过
- `Release gate` 必须完成
- `Mosaicraft_v<version>_ci-cpu.zip` artifact 必须上传成功

说明：

- 首次或依赖变更后的 vcpkg install 可能较慢
- workflow 已启用 vcpkg binary cache，后续运行应复用缓存
- CPU-only CI 验证核心服务、API、Web UI、发布脚本和无 CUDA fallback

## 5. 创建 GitHub Release

仅当本机 CUDA 发布包和远端 CPU-only CI 都通过后创建 release。

```powershell
gh release create v<version> `
  .\Mosaicraft_v<version>.zip `
  --title "Mosaicraft v<version>" `
  --notes-file .\RELEASE_NOTES_v<version>.md
```

发布说明中必须包含：

- SHA256
- CUDA 包说明
- CPU-only CI 通过的 run 链接
- 已知限制：GitHub hosted CI 不验证 CUDA，CUDA 由本机或专用 GPU 环境验证

## 6. 不允许发布的情况

- 本机 CUDA 构建或测试失败
- 发布包解压验证失败
- 远端 CPU-only CI 未完成或失败
- release notes 缺失
- zip 不是由 `scripts/release.ps1` 生成
- 未记录 SHA256

## 7. CUDA CI 后续方案

当前策略：

- GitHub hosted runner 跑 CPU-only 基础门禁
- 本机或专用 GPU 机器生成正式 CUDA 包

未来可选：

- 配置自托管 Windows GPU runner
- 将 CUDA Release 构建和 Web UI/API smoke 纳入独立 `windows-cuda` job
- 保留 CPU-only job 作为无 GPU fallback 门禁
