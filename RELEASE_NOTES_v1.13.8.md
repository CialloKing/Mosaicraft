# Mosaicraft v1.13.8 Release Notes

发布日期：2026-07-05

## 发布定位

v1.13.8 是 v1.13 系列的发布工程增强版本。相比 v1.13.7，本版本将近期较多的发布一致性、API 校验、服务层回归测试和 Web UI/API 验证工作统一纳入新的补丁版本号。

## 主要变化

- 将项目版本从 `1.13.7` 提升到 `1.13.8`。
- 保持 CLI、API、Web UI badge、README、百科文档和测试断言中的版本一致。
- 保留结构化 API 输入校验收紧结果，非法字段类型会返回明确错误。
- 保留 Web UI 对 `/api/info` 和 `/api/endpoints` 的 API 合约校验。
- 保留 Web UI 前置表单校验和危险操作确认。
- 保留服务层回归测试，覆盖建库、数据库统计、图片检查、生成马赛克和错误路径。
- 保留 Web UI/API smoke 验证，真实启动 `MosaicraftWebUI.exe` 并通过 HTTP 提交 build/mosaic job。

## 验证范围

本版本发布前应验证：

- CUDA 默认 Release 构建通过。
- CPU-only Release 构建通过。
- `mosaicraft_tests` 通过。
- `mosaicraft_regression_tests` 通过。
- `mosaicraft_webui_smoke` 在发布构建上通过。
- 解压 `Mosaicraft_v1.13.8.zip` 后，CLI 返回 `Mosaicraft 1.13.8`，Web UI/API smoke 通过。

## 发布包内容

`Mosaicraft_v1.13.8.zip` 应包含：

- `mosaicraft.exe`
- `MosaicraftWebUI.exe`
- `index.html`
- 运行所需 OpenCV / SQLite / TIFF / PNG / JPEG / WebP / CUDA runtime DLL
- `README.md`
- `API.md`
- `LICENSE`
- `third_party_versions.txt`
- `RELEASE_NOTES_v1.13.8.md`

## 兼容性说明

- `/api/run` 仅作为旧命令兼容端点保留，默认禁用。
- 新 Web UI 和新集成方应使用结构化 API。
- CUDA 构建需要可用的 NVIDIA 驱动；无 GPU 环境可使用 `MOSAICRAFT_CUDA=OFF` 构建。
