# Mosaicraft v1.13.9 Release Notes

发布日期：2026-07-05

## 发布定位

v1.13.9 是 v1.13 系列的发布工程与交付稳定性补丁版本。相比 v1.13.8，本版本不改变核心马赛克算法，重点固化发布准入、远端 CI 门禁、vcpkg 缓存和 Web UI/API 错误反馈。

## 主要变化

- 将项目版本从 `1.13.8` 提升到 `1.13.9`。
- 新增正式发布准入清单 `docs/RELEASE_CHECKLIST.md`，明确本机 CUDA 包和远端 CPU-only CI 的分工。
- 增强 `scripts/release.ps1`：
  - 新增 `-InspectOnly` 发布前环境检查模式。
  - 发布包名包含平台、架构和运行时，例如 `Mosaicraft_v1.13.9_windows-x64_cuda.zip`。
  - 自动检查 CMake、CTest、PowerShell、vcpkg toolchain、release notes、发布必需文件和现有 build 输出。
  - 可从 `CMakePresets.json` 自动读取默认 vcpkg toolchain。
- 完成 GitHub Actions Windows CPU-only CI 门禁：
  - 构建、CTest、Web UI/API smoke、打包、解压验证、artifact 上传。
  - 启用 vcpkg binary cache，后续依赖安装从约 44 分钟降至约 19 秒。
- 打磨 Web UI/API 错误展示：
  - 显示 HTTP 状态、API 路径、`exitCode` 和字段名。
  - 对常见字段错误提供明确修正建议。
  - 取消任务、清理任务、轮询任务失败路径统一使用结构化错误展示。

## 验证范围

本版本发布前应验证：

- `scripts/release.ps1 -InspectOnly -BuildDir build -Configuration Release` 通过。
- CUDA 默认 Release 构建通过。
- CPU-only Release 构建通过。
- `mosaicraft_tests` 通过。
- `mosaicraft_regression_tests` 通过。
- `mosaicraft_webui_smoke` 在发布构建上通过。
- `scripts/release.ps1 -BuildDir build -Configuration Release` 生成并解压验证 `Mosaicraft_v1.13.9_windows-x64_cuda.zip`。
- 远端 GitHub Actions `CI` 在 `main` 上通过。

## 发布包内容

`Mosaicraft_v1.13.9_windows-x64_cuda.zip` 应包含：

- `mosaicraft.exe`
- `MosaicraftWebUI.exe`
- `index.html`
- 运行所需 OpenCV / SQLite / TIFF / PNG / JPEG / WebP / CUDA runtime DLL
- `README.md`
- `API.md`
- `LICENSE`
- `third_party_versions.txt`
- `RELEASE_NOTES_v1.13.9.md`

## 兼容性说明

- 核心 CLI、HTTP API 合约主版本和输出算法保持兼容。
- `/api/run` 仍仅作为旧命令兼容端点保留，默认禁用。
- CUDA 正式包仍由本机或专用 GPU 环境构建；GitHub hosted CI 当前验证 CPU-only 基础门禁。
