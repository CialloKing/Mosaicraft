# Mosaicraft v1.13.7 Release Notes

发布日期：2026-07-05

## 发布定位

v1.13.7 是一次发布收口版本，重点是提升可交付性、API 稳定性和 Web UI 运行可靠性。此版本没有引入新的核心算法方向，主要目标是确保 CLI、Web UI、结构化 API 和发布包行为一致。

## 主要变化

- 统一版本与发布文档到 `1.13.7`。
- 收紧结构化 API 输入校验，非法字段类型会返回明确错误。
- Web UI 默认使用结构化 API，不再依赖旧式 `/api/run` 命令端点。
- Web UI 启动后会校验 `/api/info` 和 `/api/endpoints`，确认 API 合约和必需端点可用。
- Web UI 增加前置表单校验，`mosaic`、`build`、`inspect` 的必填项会在请求前检查。
- Web UI 对导出和实际清理类操作增加确认步骤。
- 新增服务层回归测试，覆盖建库、数据库统计、图片检查、生成马赛克和错误路径。
- 新增 Web UI/API smoke 验证，真实启动 `MosaicraftWebUI.exe` 并通过 HTTP 提交 build/mosaic job。

## 验证范围

本版本发布前已验证：

- CUDA 默认 Release 构建通过。
- CPU-only Release 构建通过。
- `mosaicraft_tests` 通过。
- `mosaicraft_regression_tests` 通过。
- `mosaicraft_webui_smoke` 在 CUDA 构建和 CPU-only 构建上通过。
- `tools/command-builder/index.html` 内嵌脚本语法检查通过。

## 发布包内容

`Mosaicraft_v1.13.7.zip` 应包含：

- `mosaicraft.exe`
- `MosaicraftWebUI.exe`
- `index.html`
- 运行所需 OpenCV / SQLite / TIFF / PNG / JPEG / WebP / CUDA runtime DLL
- `README.md`
- `API.md`
- `LICENSE`
- `third_party_versions.txt`
- `RELEASE_NOTES_v1.13.7.md`

## 兼容性说明

- `/api/run` 仅作为旧命令兼容端点保留，默认禁用。
- 新 Web UI 和新集成方应使用结构化 API。
- CUDA 构建需要可用的 NVIDIA 驱动；无 GPU 环境可使用 `MOSAICRAFT_CUDA=OFF` 构建。
