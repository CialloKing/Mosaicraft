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

当前 API 合约为 **2.0**：维护清理接口仅允许 POST，HTTP Host/Origin/Content-Type 校验已收紧。随程序提供的 Web UI 已同步升级；旧客户端需按下文更新请求。
The current API contract is **2.0**: purge accepts POST only, and HTTP Host/Origin/Content-Type validation is stricter. The bundled Web UI is updated; older clients must update requests as described below.

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

API 只接受本机服务的 Host（`localhost`、`127.0.0.1` 或 `[::1]` 加当前端口）。浏览器请求的 Origin 必须与该 Host 同源，跨站请求返回 HTTP 403；命令行客户端可以不发送 Origin。结构化请求体必须使用 `Content-Type: application/json`，否则返回 HTTP 415。非法 UTF-8 或包含 NUL 的 query 参数返回 HTTP 400 JSON。
The API requires a local Host with the current port. Browser Origin must match that Host; cross-site requests return HTTP 403. Non-browser clients may omit Origin. Structured request bodies require `Content-Type: application/json` or return HTTP 415. Invalid UTF-8 or NUL bytes in query parameters return HTTP 400 JSON.

常见范围约束 / Common range constraints:

| 字段 / Field | 范围 / Range |
|------|------|
| `tileW`, `tileH` | `4..1000000`, tile pixels `<=100000000` |
| `outW`, `outH` | `0..1000000`; `0=auto`, one axis preserves aspect ratio |
| `nativeTileW`, `nativeTileH` | `1..1000000`, tile pixels `<=100000000` |
| `candidates` | `10..10000` |
| `topNrandom` | `1..10000` |
| `neighborWindow` | `0..1000000` |
| `upscale` | `1..64` when provided |
| `quality` | `1..100` |
| `pngLevel` | `1..9` |
| `lRange` | `0..255`, candidate L difference limit |
| weights | finite, nonnegative, positive finite sum |
| `usePenalty`, `neighborPenalty` | finite, `>= 0` |
| `colorStrength` | `0..0.5` |
| `threads` | `0..256` |
| `normalizeWidth`, `normalizeHeight` | `1..8192` |
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

成功提交返回 HTTP 202 和 `job` / Successful submission returns HTTP 202 with `job`.

### `POST /api/jobs/build`

启动异步图库构建任务 / Starts an asynchronous library build job.

默认对全库按平均颜色的 HSV 色相和明暗排序并从 `000000` 连续编号，`appendMode: true` 也会重新排列旧图片。输入与输出同目录时也更改已索引原图的文件名。图片 ID、使用统计和图片内容保持不变；`normalizeOnly: true` 不执行全库重排。不增加新的请求字段。
Builds order the entire library by mean-color HSV hue and lightness and number files consecutively from `000000`, including append and same-directory builds. Image IDs, usage statistics and bytes are preserved. `normalizeOnly: true` skips reordering. No new request fields are required.

排序复用数据库平均 LAB，色度 `C < 0.5` 的图片按灰度亮度排列，彩色每 15° 分段并从 345° 开始。CPU/CUDA 独立建库可能因均值精度不同产生顺序差异，同一数据库重复排序保持稳定。
Ordering reuses stored mean LAB: chroma below 0.5 enters the grayscale group; colors use 15-degree hue bands starting at 345 degrees. Independent CPU/CUDA builds may differ because their mean-color extraction differs. Reordering the same database is stable.

同一图库的建库与读取、生成、维护通过文件锁协调。图库忙或存在未恢复的重命名清单时，数据库操作返回失败；用建库任务恢复后继续。目标被未索引文件占用或旧图库图片需要跨卷移动时，建库失败并保留已有图片。
An operation lock coordinates builds with library readers and maintenance. Busy libraries and pending rename journals fail database operations; a build recovers pending renames. Unindexed destination conflicts and cross-volume moves of indexed images fail without overwriting existing images.

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

成功生成后返回 HTTP 200 和服务结果，不返回 job。同步生成和异步建库/生成共用串行执行锁，以保证特征缓存写入一致。
Returns HTTP 200 with a service result after successful generation, without a job. Synchronous generation and asynchronous build/mosaic pipelines share a serial execution lock to keep feature cache writes consistent.

两个生成端点默认允许超大输出，无需额外字段或开关。目标缩放/上采样后的工作图像最多 1 亿像素。单图 JPEG/WebP 超过各自尺寸限制时返回失败，请选择 PNG、TIFF 或 `tiled: true`。PNG/TIFF 可指定 `writeMode: "stream"` 降低内存占用。旧版图库特征需先通过建库端点原地刷新。
Both mosaic endpoints allow large output by default, without an extra field or switch. Resized/upscaled working targets are limited to 100 million pixels. JPEG/WebP output exceeding format dimensions fails; select PNG, TIFF or `tiled: true`. PNG/TIFF can use `writeMode: "stream"` to reduce memory use. Refresh legacy features through the build endpoint before generation.

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

### `POST /api/db/purge`

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

命令文本可使用 `Content-Type: text/plain`，仍须通过同一 Host/Origin 校验。
Raw command text may use `Content-Type: text/plain`, with the same Host/Origin validation.
