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

当前 API 合约为 **2.1**，主兼容版本仍为 **2**。保留 2.0 字段和端点，增加运行诊断、阶段进度、会话标识、结果产物和可选预览。维护清理接口仍只允许 POST，Host/Origin/Content-Type 检查保持不变。
Contract **2.1** retains major version **2** and the 2.0 fields/endpoints, adding diagnostics, stage progress, session identity, artifacts and optional previews. Purge remains POST-only with the same HTTP boundary checks.

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

`info.runtime` 包含 `sessionId`、`workingDirectory`（相对路径解析目录）和 `finishedJobLimit: 50`。服务重启后会话 ID 改变，任务 ID 可能重新从 `job-000001` 开始；客户端必须将会话和任务 ID 一起识别。`info.features` 新增 `job-diagnostics`、`job-progress`、`job-artifacts`、`result-preview`。
`info.runtime` identifies the current process session and working directory. Match jobs by session plus ID; IDs can repeat after a restart. Discover optional capabilities through `info.features`.

### `GET /api/endpoints`

返回结构化 API 列表。每个条目包含 / Returns the structured API list. Each entry contains:

| 字段 / Field | 说明 / Description |
|------|------|
| `method` | HTTP 方法或方法集 |
| `methods` | 机器可读的 HTTP 方法列表 / machine-readable HTTP method list |
| `path` | 端点路径 / endpoint path |
| `httpPattern` | 本地 HTTP 适配器使用的服务端路由模式 / server route pattern used by the local HTTP adapter |
| `operation` | 稳定的核心 API 操作名 / stable core API operation name |
| `requestShape` | HTTP 映射：`none`、`body`、`query`、`jobId`、`jobQuery`、`jobArtifact` 或 `legacyCommand` |
| `description` | 简短描述 / short endpoint description |
| `category` | 功能分组，如 `jobs`、`database`、`inspect` |
| `requestFields` | 端点接受的常用请求字段 / common request fields accepted by the endpoint |
| `requiredFields` | 调用方必须提供的字段。未列出的字段有默认值或可选 / fields that callers must provide |
| `fieldAliases` | 规范字段的可接受别名，如 `dbPath` 也接受 `db` |
| `queryKeys` | 端点接受的机器可读查询字符串键 / machine-readable query string keys |
| `acceptedQueryKeys` | HTTP 适配器接受的完整查询键，包括规范字段名和别名 |
| `successStatus` | 成功响应的正常 HTTP 状态码 / normal HTTP status code for a successful response |
| `responseKey` | 成功时返回的主要 JSON 载荷键 / primary JSON payload key returned on success |
| `responseFormat` | `json` 或 `file`；文件成功响应为二进制，失败仍为 JSON / binary on file success, JSON on error |
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

2.1 增加 `diagnostic`，无终止错误时为 `null`。客户端应使用 `code` 和 `field`，避免解析英文 `message`。CLI、同步生成与异步任务共用执行失败的原因码；CLI 控制台格式为 `ERROR: [code] message`。异步执行失败保持 HTTP 200 查询成功、`job.state: "failed"` 的含义。
Use structured `diagnostic.code` and `field`; do not infer them from English messages. Async execution failure is represented by job state, not by changing the successful job-query HTTP status.

| 诊断字段 | 含义 |
| --- | --- |
| `code`, `severity`, `stage` | 稳定原因码；`info`/`warning`/`error`；发生阶段 |
| `message`, `field`, `path` | 技术原因；相关规范请求字段和路径（可为空） |
| `sequence`, `timestamp` | 任务内递增序号；Unix 毫秒时间 |

主要原因码：`invalid_argument`、`input_unreadable`、`library_unavailable`、`library_incompatible`、`library_write_failed`、`no_candidates`、`cuda_unavailable`、`cuda_resource_fallback`、`cuda_execution_failed`、`out_of_memory`、`source_skipped`、`feature_missing`、`output_tile_missing`、`output_write_failed`、`preview_write_failed`、`analysis_write_failed`、`usage_commit_failed`。警告保持原有继续执行规则，不代表任务失败。

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
| `colorStrength` | `0..0.5`, default `0.04` |
| `overlayStrength` | finite, `0..1`, default `0` (off) |
| `threads` | `0..256` |
| `normalizeWidth`, `normalizeHeight` | `1..8192` |
| `limit` | `>= 1` |

亮度调整默认关闭（`colorAdjust: false`），`colorStrength` 默认 `0.04`；单独设置强度不会启用。选图后，每个输出小块在 `1±colorStrength` 范围的 1001 档乘数中等概率抽取一档，统一缩放 LAB L 通道，A、B 通道保持不变。默认乘数范围为 `0.96～1.04`，平均值为 1；强度为 0 时跳过颜色转换，保持原像素。像素取整、高亮截断和颜色转换仍可能影响最终图像的平均亮度。
Brightness adjustment defaults to off (`colorAdjust: false`), with `colorStrength` defaulting to `0.04`; setting strength alone does not enable it. After matching, each output tile uniformly samples one of 1001 multipliers in `1±colorStrength` and scales its LAB L channel, leaving A and B unchanged. The default multiplier range is `0.96..1.04`, with a mean of 1; zero strength skips color conversion and preserves the pixels. Quantization, highlight clipping, and color conversion can still affect the final image's mean brightness.

基础图透明叠加通过 `overlayStrength` 开启，例如 `0.2` 表示 80% 素材与 20% 基础图混合，无需额外开关。同步 `/api/mosaic` 和异步 `/api/jobs/mosaic` 均支持。使用经过尺寸调整、upscale 和镜像补边后的目标工作图，按全图坐标逐块双线性采样；强度 `0` 跳过叠加，`1` 在成功加载的小块中完全显示该参考图。不会改变选图和图库；缺失素材仍计为失败并沿用原背景。若同时开启亮度扰动，先调整亮度再叠加。分析分数仍衡量选图质量，不代表最终合成画面的相似度。

Base-image overlay is enabled by `overlayStrength`; `0.2` blends 80% tile pixels with 20% target pixels, with no separate switch. Both synchronous `/api/mosaic` and asynchronous `/api/jobs/mosaic` support it. The reference is the resized, upscaled and reflect-padded working target, sampled per tile using global coordinates and bilinear interpolation. Zero skips overlay; one fully shows the reference in successfully loaded tiles. Matching and the library remain unchanged. Missing tiles retain their failure counts and background. Optional brightness jitter runs before overlay. Analysis scores continue to describe tile matching, not final composite similarity.

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

按创建时间倒序返回任务摘要，包含原状态/路径/时间、`progress`、`warningCount`、`diagnostic` 和 `canCancel`。不包含完整请求、警告样例、产物和性能数据。响应根对象提供 `sessionId`。
Lists summaries newest first, without full requests, warning samples, artifacts or metrics. The response includes the session ID.

任务及诊断仅保留在服务内存中。自动保留最近 50 个完成任务，运行和排队任务不淘汰；清理或淘汰记录不删除输出文件。服务关闭后不能恢复旧会话任务。
History is process-local: retain the latest 50 finished jobs, plus all active/queued jobs. Removing records never deletes files.

### `DELETE /api/jobs`

从内存任务列表中移除已完成的任务。运行中和排队的任务保留。
Removes finished jobs from the in-memory job list. Running and queued jobs are kept.

### `GET /api/jobs/{id}`

返回 `sessionId` 和完整 `job`，保留原字段，增加以下字段。`outputPath` 始终是请求路径，不能用它猜测实际文件名。
Returns the complete snapshot. `outputPath` retains its requested-path meaning; actual files are listed in `artifacts`.

| 字段 | 内容 |
| --- | --- |
| `progress` | `stage`、`startedAt`、`updatedAt`（Unix 毫秒）、`activities: [{id, completed, total, unit}]`；未知总量为 `null` |
| `warningCount`, `warnings` | 总警告数量；按原因码汇总的 `code/count/message/paths`，每码最多 3 个路径样例 |
| `diagnostic` | 首个终止错误，独立于事件缓冲保留 |
| `runtime` | `featureBackend/scoreBackend/fallbackReason`、实际 `format/width/height/matched/failed`、`metrics` |
| `artifacts` | 已成功关闭的文件/目录：`id/kind/path/mime/bytes/directory`；路径为规范化绝对路径 |
| `request` | 规范请求参数，供页面恢复编辑；恢复不会自动执行 |
| `canCancel` | 仅排队任务为 `true` |

后端值为 `cpu`、`template_cuda`、`compatible_cuda` 或 `none`；未检测时为空。`metrics` 仅在采集后含数据，不能将空对象解释为耗时为零。阶段计数最多每 200ms 更新一次，阶段切换及诊断立即可见；归一化与特征处理可能并行，应分别显示，不拼接整体百分比。阶段结束、计数达到总量或主图落盘不代表任务已经成功。

主图成功、预览或使用统计随后失败时，任务按原规则失败，已经保存的 `main` 产物仍然可用。报告失败沿用原有软失败/异常与提交边界。

### `GET /api/jobs/{id}/events?after=0&limit=100`

返回 `sessionId`、`events`、`firstSequence`、`nextCursor`、`dropped`、`truncated`。`after` 为排他游标，`limit` 为正整数，默认及最大值均为 100。持续使用返回的 `nextCursor` 获取增量；游标超过当前尾部会收敛到尾部。

每任务最多保留 256 条、约 256 KiB 的事件（按内部记录占用计量）；消息和路径分别最多 4096 UTF-8 字节，原因码和字段最多 128 字节，在字符边界截断。旧游标已落后于缓冲时 `truncated: true`；`dropped` 是累计淘汰条数。警告总数和终止错误不随事件淘汰丢失。

### `GET /api/jobs/{id}/artifacts/{artifactId}`

只接受任务已登记的产物 ID，不接受文件路径。成功按登记 MIME 流式返回文件，支持 HEAD 与字节 Range；主图及 HTML 以附件下载，JPEG 预览可内联显示。使用固定 64 KiB 缓冲，不将整图读入内存。

文件不存在或记录已清理返回 404；文件路径、大小或修改时间与登记指纹不一致返回 409（`artifact_changed`）。目录不能下载，仅在详情中提供真实路径。分块为 `tiles` 目录，DeepZoom 另有 `manifest` 和 `viewer`；本接口不打包目录，也不托管完整 DeepZoom 查看器的关联文件。

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

两个生成端点均接受 `generatePreview: true`，默认 `false`，等价于 CLI `--preview`。只支持单图；与 `tiled` 或 `deepZoom` 组合返回 400，诊断字段为 `generatePreview`。沿用宽度最多 1400、总像素最多 400 万、不放大的 JPEG 预览，质量固定为 92。同步响应只返回产物元数据，使用任务端点生成才能通过任务产物接口下载。
`generatePreview` is optional and off by default. It uses the existing bounded JPEG preview generator and is incompatible with tiled/DeepZoom output.

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
