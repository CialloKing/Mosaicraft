# Mosaicraft API / API 文档

> English · 中文

## 概述 / Overview

Mosaicraft 有两个可执行入口 / Two executable entry points:

- `mosaicraft.exe` — 命令行入口 / command line entry
- `MosaicraftWebUI.exe` — 本地 Web UI 和 HTTP API 入口 / local Web UI and HTTP API entry

两个入口调用同一套核心服务（马赛克生成、图库构建、图片检查、数据库维护）。
Both entries call the same core services for mosaic generation, library building, image inspection, and database maintenance.

`/api/info` 在 `entryPoints` 中暴露相同的入口边界，调用方可据此区分当前入口和支持的可执行入口集合。
`/api/info` exposes the same entry point boundary in `entryPoints`, so callers can distinguish the current entry from the supported executable entry set.

---

## 服务发现 / Discovery

### `GET /api/info`

返回服务版本、入口名称、结构化 API 状态、legacy `/api/run` 状态、API 合约版本、入口点列表、端点数量、元数据校验状态和功能清单。
Returns service version, entry name, structured API status, legacy `/api/run` status, API contract version, entry point list, endpoint counts, metadata validation status, and a compact feature list.

结构化 API 上报字段 / Structured API fields:

| 字段 | 说明 |
|------|------|
| `contractVersion` | HTTP/服务合约的语义版本 / semantic version of the HTTP/service contract |
| `contractMajorVersion` | 主兼容版本。增量式元数据或端点变更应保持此值稳定；破坏性的请求/响应变更必须递增 / breaking request/response changes must increment it |
| `compatibility` | 稳定性标签，供生成客户端和 UI 适配器使用 / stability label for generated clients and UI adapters |
| `stable` | 当前合约是否面向常规调用方 / whether the current contract is intended for regular callers |

### `GET /api/endpoints`

返回结构化 API 列表。每个条目包含 / Returns the structured API list. Each entry contains:

| 字段 / Field | 说明 / Description |
|------|------|
| `method` | HTTP 方法或方法集 |
| `methods` | 机器可读的 HTTP 方法列表 / machine-readable HTTP method list |
| `path` | 端点路径 / endpoint path |
| `httpPattern` | 本地 HTTP 适配器使用的服务端路由模式 / server route pattern used by the local HTTP adapter |
| `operation` | 稳定的核心 API 操作名 / stable core API operation name |
| `requestShape` | 端点如何将 HTTP 数据映射为核心请求：`none`、`body`、`query`、`jobId` 或 `legacyCommand` |
| `description` | 简短描述 / short endpoint description |
| `category` | 功能分组，如 `jobs`、`database`、`inspect` |
| `requestFields` | 端点接受的常用请求字段 / common request fields accepted by the endpoint |
| `requiredFields` | 调用方必须提供的字段。未列出的字段有默认值或可选 / fields that callers must provide |
| `fieldAliases` | 规范字段的可接受别名，如 `dbPath` 也接受 `db` |
| `queryKeys` | 端点接受的机器可读查询字符串键 / machine-readable query string keys |
| `acceptedQueryKeys` | HTTP 适配器接受的完整查询键，包括规范字段名和别名 |
| `successStatus` | 成功响应的正常 HTTP 状态码 / normal HTTP status code for a successful response |
| `responseKey` | 成功时返回的主要 JSON 载荷键 / primary JSON payload key returned on success |
| `errorStatuses` | 结构化 handler 对预期失败可能返回的 HTTP 状态码 |
| `errorShapes` | 端点使用的 JSON 错误信封变体：`serviceResult`、`apiError`、`jobError`、`payloadResult` |
| `errorResponseKeys` | 错误响应中也可能出现的主要载荷键，如 `job`、`export`、`purge` |
| `errorResponses` | 配对的错误响应合约，包含 `status`、`shape` 和可选 `responseKey` |
| `sideEffects` | 端点是否会修改文件、任务或数据库 / whether the endpoint can modify files, jobs, or databases |
| `longRunning` | 调用方是否应优先使用异步处理或轮询 / whether callers should prefer async handling or polling |
| `legacy` | 是否仅为兼容端点 / whether this endpoint is only for compatibility |
| `enabled` | 端点当前是否可用 / whether the endpoint is currently available |

标记为 `legacy: true` 的条目为兼容端点，新 UI 代码不应使用 / Entries marked `legacy: true` are compatibility endpoints and should not be used by new UI code.

---

## 健康检查 / Health

### `GET /api/ping`

返回标准 JSON 健康响应 / Returns the standard JSON health response:

```json
{
  "ok": true,
  "message": "pong"
}
```

---

## 错误格式 / Errors

格式错误的 JSON 请求体返回 HTTP 400，附带标准结果结构 / Malformed JSON request bodies return HTTP 400 with the standard result shape:

```json
{
  "ok": false,
  "exitCode": 1,
  "message": "invalid JSON body: ..."
}
```

字段类型或范围不合法也返回 HTTP 400，且 `message` 指向具体字段。
Invalid field types or ranges also return HTTP 400, and `message` names the field.

常见范围约束 / Common range constraints:

| 字段 / Field | 范围 / Range |
|------|------|
| `tileW`, `tileH` | `>= 4` |
| `candidates` | `>= 10` |
| `topNrandom` | `>= 1` |
| `neighborWindow` | `>= 0` |
| `upscale` | `>= 1` when provided |
| `quality` | `1..100` |
| `pngLevel` | `1..9` |
| `lRange`, `neighborPenalty` | `>= 0` |
| `colorStrength` | `0..0.5` |
| `threads` | `>= 0` |
| `normalizeWidth`, `normalizeHeight` | `>= 1` |
| `limit` | `>= 1` |

示例 / Example:

```json
{
  "ok": false,
  "exitCode": 1,
  "message": "tileW must be at least 4"
}
```

---

## 任务 / Jobs

### `POST /api/jobs/mosaic`

启动异步马赛克任务 / Starts an asynchronous mosaic job.

### `POST /api/jobs/build`

启动异步图库构建任务 / Starts an asynchronous library build job.

### `GET /api/jobs`

列出已知任务 / Lists known jobs.

### `DELETE /api/jobs`

从内存任务列表中移除已完成的任务。运行中和排队的任务保留。
Removes finished jobs from the in-memory job list. Running and queued jobs are kept.

### `GET /api/jobs/{id}`

返回单个任务快照 / Returns a single job snapshot.

### `DELETE /api/jobs/{id}`

取消排队的任务。运行中的任务不会被中断，返回 HTTP 409。
Cancels a queued job. Running jobs are not interrupted and return HTTP 409.

---

## 马赛克 / Mosaic

### `POST /api/mosaic`

同步执行马赛克请求。新 UI 代码应优先使用 `POST /api/jobs/mosaic` 处理长时间任务。
Runs a mosaic request synchronously. New UI code should prefer `POST /api/jobs/mosaic` for long-running work.

---

## 图片检查 / Inspect

### `GET|POST /api/inspect`

检查源图片，可选择报告数据库匹配覆盖率。
Inspects a source image and optionally reports database match coverage.

常用 JSON 字段 / Common JSON fields:

```json
{
  "imagePath": "target.jpg",
  "dbPath": "library/mosaicraft.db"
}
```

---

## 数据库 / Database

### `GET|POST /api/db/stats`

返回数据库大小、LAB 范围、亮度分桶和覆盖间隙。
Returns database size, LAB ranges, brightness buckets, and coverage gaps.

### `GET|POST /api/db/health`

返回健康警告和建议 / Returns health warnings and recommendations.

### `GET|POST /api/db/usage`

返回使用统计 / Returns usage statistics.

常用 JSON 字段 / Common JSON fields:

```json
{
  "dbPath": "library/mosaicraft.db",
  "limit": 50,
  "showUnused": false
}
```

### `POST /api/db/usage/export`

按总 tile 使用量导出已用图片。此端点有文件系统副作用，需要 `confirm: true`。
Exports used images ordered by total tile count. This endpoint has filesystem side effects and requires `confirm: true`.

```json
{
  "dbPath": "library/mosaicraft.db",
  "outputDir": "used-images",
  "confirm": true
}
```

### `GET|POST /api/db/purge`

预览或移除孤立的数据库记录。默认为干运行。实际删除需要 `dryRun: false` 和 `confirm: true`。
Previews or removes orphan database records. Default is dry-run. Actual deletion requires `dryRun: false` and `confirm: true`.

```json
{
  "dbPath": "library/mosaicraft.db",
  "dryRun": true,
  "confirm": false
}
```

---

## 兼容端点 / Legacy

### `POST /api/run`

旧式命令风格调用方的兼容端点。Web UI 不再使用此端点。默认禁用；在启动 `MosaicraftWebUI.exe` 前设置 `MOSAICRAFT_ENABLE_LEGACY_RUN=1` 可启用兼容模式。
Compatibility endpoint for old command-style callers. The Web UI no longer uses this endpoint. It is disabled by default; set `MOSAICRAFT_ENABLE_LEGACY_RUN=1` before starting `MosaicraftWebUI.exe` to enable compatibility mode.
