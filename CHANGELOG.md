# Mosaicraft Changelog

本文件记录每个版本的高层变化。详细发布说明放在 `docs/releases/`，GitHub Release 页面只保留简短使用说明。

## v1.13.9 - 2026-07-05

- 发布准入清单、远端 CPU-only CI 和 vcpkg 缓存稳定化。
- 发布包名改为包含平台、架构和运行时：`Mosaicraft_v1.13.9_windows-x64_cuda.zip`。
- Web UI/API 错误展示继续打磨，显示 HTTP 状态、API 路径、字段名和修正建议。

详细说明：[docs/releases/v1.13.9.md](docs/releases/v1.13.9.md)

## v1.13.8 - 2026-07-05

- 统一版本到 `1.13.8`。
- 保留 API 输入校验、Web UI 合约校验、表单前置校验和服务层回归测试。
- 保留 Web UI/API smoke 验证。

详细说明：[docs/releases/v1.13.8.md](docs/releases/v1.13.8.md)

## v1.13.7 - 2026-07-05

- 发布收口版本，重点提升可交付性、API 稳定性和 Web UI 运行可靠性。
- Web UI 默认使用结构化 API。
- 新增服务层回归测试和 Web UI/API smoke 验证。

详细说明：[docs/releases/v1.13.7.md](docs/releases/v1.13.7.md)
