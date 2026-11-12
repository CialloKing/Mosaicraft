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

当前 API 版本为 **1.5**，主版本为 **1**。增加设备发现、Vulkan/Metal 及评分模式，保留已有请求字段及路径语义。端点继续使用 `/api/*`，图库数据库结构版本仍为 1。

API **1.5** adds device discovery, Vulkan/Metal and compatible/fast scoring. Existing CPU/CUDA requests remain compatible. The library schema version remains 1.

`GET /api/info` 新增 `devices`：每项包含 `id`、`backend`、`name`、`available`、`integrated`、`validated`、`reason`、`maxBufferBytes`。`available` 仅表示运行时发现能力，不能代替性能或包的硬件验收。新后端的 `validated=false` 表示尚未获得跨任务的默认准入。

建库、追加和生成请求的 `backend` 支持 `auto|cpu|cuda|vulkan|metal`，`device` 缺省 `auto`，也可使用设备发现返回的标识（例如 `vulkan:1`）。后端、设备与 `useGpu` 冲突会拒绝请求。显式 GPU 请求不切换到其他 GPU 后端。

生成和预览增加 `scoreMode=compatible|fast`，缺省 `compatible`。Vulkan/Metal 的兼容评分在 CPU FP64 执行，快速评分在 GPU FP32 计算五种距离，由 CPU 使用现有双精度权重和惩罚合成。CPU/CUDA 首期保留已有精度行为。新后端暂不进入 `auto`。

运行结果 `runtime` 新增实际 `device`、`scoreMode`、`kernelVersion`。`gpuComputeWaitMs` 是主机提交、执行及等待的合计，不能当作纯内核事件时间；其他便携 GPU 指标包括分配、上传、下载、库常驻及峰值缓冲。`gpuSharedMemoryBytes` 是共享 GPU 缓冲的同一份分配，不应再次加到 CPU RSS 作为物理内存总量。NVML 指标只适用于实际 CUDA 设备，其他不可取得值继续使用不可用语义。

保留已有 `cuda_*` 诊断；新增 `gpu_required`、`gpu_execution_failed`，CLI 退出码 4 表示 GPU 错误。GPU 批次执行失败会结束任务，不静默改用 CPU 重算。缓存身份含实际后端、设备、评分模式和内核版本。

HTML 工程仅使用当前内部结构，不设置工程格式版本，清单、浏览快照及工程信息均不输出 version 字段，`GET /api/info` 也不输出 projectFormatVersion。工程仅保存清单、离线 HTML、无损目标图及一份按字节去重的已用素材。素材保留图库文件名，按素材索引分目录防止同名冲突；不生成 overview 或派生缩略图，离线查看器不设置应用图片缓存上限。旧总览/缩略图工程不再支持，需要重新生成。

---

## 服务发现 / Discovery

### `GET /api/info`

返回服务版本、入口名称、结构化 API 状态、API 合约版本、入口点列表、端点数量、元数据校验状态和功能清单。
Returns service version, entry name, structured API status, API contract version, entry point list, endpoint counts, metadata validation status, and a compact feature list.

结构化 API 上报字段 / Structured API fields:

| 字段 | 说明 |
|------|------|
| `contractVersion` | HTTP/服务合约的语义版本 / semantic version of the HTTP/service contract |
| `contractMajorVersion` | 主兼容版本。增量式元数据或端点变更应保持此值稳定；破坏性的请求/响应变更必须递增 / breaking request/response changes must increment it |
| `compatibility` | 稳定性标签，供生成客户端和 UI 适配器使用 / stability label for generated clients and UI adapters |
| `stable` | 当前合约是否面向常规调用方 / whether the current contract is intended for regular callers |

`info.runtime` 包含 `sessionId`、`workingDirectory`（相对路径解析目录）和 `finishedJobLimit: 50`。服务重启后会话 ID 改变，任务 ID 可能重新从 `job-000001` 开始；客户端必须将会话和任务 ID 一起识别。`info.features` 包含 `job-diagnostics`、`job-progress`、`job-artifacts`、`result-preview`；同时包含 `output-format-policy`。
`info.runtime` identifies the current process session and working directory. Match jobs by session plus ID; IDs can repeat after a restart. Discover optional capabilities through `info.features`.

1.3 功能标记包括 `mosaic-plan`、`mosaic-size-sample`、`library-visuals`、`preview-quality`、`task-resources`、`preview-comparison` 和 `task-history`。

1.4 新增 `portable-project`、`project-render`、`project-check`、`project-pack`。CLI 已更换命令树且不保留旧别名，参见 [CLI 与工程说明](CLI_PROJECTS.md)。旧 HTTP 请求字段继续可用。

### `GET /api/endpoints`

返回结构化 API 列表。每个条目包含 / Returns the structured API list. Each entry contains:

| 字段 / Field | 说明 / Description |
|------|------|
| `method` | HTTP 方法或方法集 |
| `methods` | 机器可读的 HTTP 方法列表 / machine-readable HTTP method list |
| `path` | 端点路径 / endpoint path |
| `httpPattern` | 本地 HTTP 适配器使用的服务端路由模式 / server route pattern used by the local HTTP adapter |
| `operation` | 稳定的核心 API 操作名 / stable core API operation name |
| `requestShape` | HTTP 映射：`none`、`body`、`query`、`jobId`、`jobQuery`、`jobArtifact` |
| `description` | 简短描述 / short endpoint description |
| `category` | 功能分组，如 `jobs`、`database`、`inspect` |
| `requestFields` | 端点接受的常用请求字段 / common request fields accepted by the endpoint |
| `requiredFields` | 调用方必须提供的字段。未列出的字段有默认值或可选 / fields that callers must provide |
| `queryKeys` | 端点接受的机器可读查询字符串键 / machine-readable query string keys |
| `acceptedQueryKeys` | HTTP 适配器接受的完整规范查询键，不接受旧别名 |
| `successStatus` | 成功响应的正常 HTTP 状态码 / normal HTTP status code for a successful response |
| `responseKey` | 成功时返回的主要 JSON 载荷键 / primary JSON payload key returned on success |
| `responseFormat` | `json` 或 `file`；文件成功响应为二进制，失败仍为 JSON / binary on file success, JSON on error |
| `errorStatuses` | 结构化 handler 对预期失败可能返回的 HTTP 状态码 |
| `errorShapes` | JSON 错误统一为 `serviceResult`，包含 `ok/exitCode/message/diagnostic` |
| `errorResponseKeys` | 错误响应中也可能出现的主要载荷键，如 `job`、`export`、`purge` |
| `errorResponses` | 配对的错误响应合约，包含 `status`、`shape` 和可选 `responseKey` |
| `sideEffects` | 端点是否会修改文件、任务或数据库 / whether the endpoint can modify files, jobs, or databases |
| `longRunning` | 调用方是否应优先使用异步处理或轮询 / whether callers should prefer async handling or polling |
| `enabled` | 端点当前是否可用 / whether the endpoint is currently available |


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
  "message": "invalid JSON body: ...",
  "diagnostic": {"code": "invalid_argument", "severity": "error", "message": "invalid JSON body: ...", "field": ""}
}
```

字段类型或范围不合法也返回 HTTP 400，且 `message` 指向具体字段。
Invalid field types or ranges also return HTTP 400, and `message` names the field.

所有 JSON 错误均包含 `ok/exitCode/message/diagnostic`，无终止错误时为 `null`。客户端应使用 `code` 和 `field`，避免解析英文 `message`。CLI、同步生成与异步任务共用执行失败的原因码；CLI 控制台格式为 `ERROR: [code] message`。异步执行失败保持 HTTP 200 查询成功、`job.state: "failed"` 的含义。
Use structured `diagnostic.code` and `field`; do not infer them from English messages. Async execution failure is represented by job state, not by changing the successful job-query HTTP status.

| 诊断字段 | 含义 |
| --- | --- |
| `code`, `severity`, `stage` | 稳定原因码；`info`/`warning`/`error`；发生阶段 |
| `message`, `field`, `path` | 技术原因；相关规范请求字段和路径（可为空） |
| `sequence`, `timestamp` | 任务内递增序号；Unix 毫秒时间 |

主要原因码：`invalid_argument`、`input_unreadable`、`library_unavailable`、`library_incompatible`、`library_write_failed`、`no_candidates`、`cuda_unavailable`、`cuda_resource_fallback`、`cuda_execution_failed`、`out_of_memory`、`source_skipped`、`feature_invalid`、`output_tile_missing`、`output_write_failed`、`preview_write_failed`、`analysis_write_failed`、`usage_commit_failed`。警告保持原有继续执行规则，不代表任务失败。

API 只接受本机服务的 Host（`localhost`、`127.0.0.1` 或 `[::1]` 加当前端口）。浏览器请求的 Origin 必须与该 Host 同源，跨站请求返回 HTTP 403；命令行客户端可以不发送 Origin。结构化请求体必须使用 `Content-Type: application/json`，否则返回 HTTP 415。非法 UTF-8 或包含 NUL 的 query 参数返回 HTTP 400 JSON。
The API requires a local Host with the current port. Browser Origin must match that Host; cross-site requests return HTTP 403. Non-browser clients may omit Origin. Structured request bodies require `Content-Type: application/json` or return HTTP 415. Invalid UTF-8 or NUL bytes in query parameters return HTTP 400 JSON.

常见范围约束 / Common range constraints:

| 字段 / Field | 范围 / Range |
|------|------|
| `tileW`, `tileH` | `4..1000000`, tile pixels `<=100000000` |
| `outW`, `outH` | `0..1000000`; `0=auto`, one axis preserves aspect ratio |
| `nativeTileW`, `nativeTileH` | `1..1000000`, tile pixels `<=100000000` |
| `candidates` | `10..10000` |
| `topNRandom` | `1..10000` |
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
| `limit` | 图库排行 `>= 0`，0 表示全部；事件分页仍 `>= 1` |
| `seed` | `0..4294967295`，省略时生成并记录种子 |
| `backend` | 建库和匹配 `auto\|cpu\|cuda`，显式 cuda 不静默回退 |

亮度调整默认关闭（`colorAdjust: false`），`colorStrength` 默认 `0.04`；单独设置强度不会启用。选图后，每个输出小块在 `1±colorStrength` 范围的 1001 档乘数中等概率抽取一档，统一缩放 LAB L 通道，A、B 通道保持不变。默认乘数范围为 `0.96～1.04`，平均值为 1；强度为 0 时跳过颜色转换，保持原像素。像素取整、高亮截断和颜色转换仍可能影响最终图像的平均亮度。
Brightness adjustment defaults to off (`colorAdjust: false`), with `colorStrength` defaulting to `0.04`; setting strength alone does not enable it. After matching, each output tile uniformly samples one of 1001 multipliers in `1±colorStrength` and scales its LAB L channel, leaving A and B unchanged. The default multiplier range is `0.96..1.04`, with a mean of 1; zero strength skips color conversion and preserves the pixels. Quantization, highlight clipping, and color conversion can still affect the final image's mean brightness.

基础图透明叠加通过 `overlayStrength` 开启，例如 `0.2` 表示 80% 素材与 20% 基础图混合，无需额外开关。同步 `/api/mosaic` 和异步 `/api/jobs/mosaic` 均支持。使用经过尺寸调整、upscale 和镜像补边后的目标工作图，按全图坐标逐块双线性采样；强度 `0` 跳过叠加，`1` 在成功加载的小块中完全显示该参考图。不会改变选图和图库；缺失素材仍计为失败并沿用原背景。若同时开启亮度扰动，先调整亮度再叠加。分析分数仍衡量选图质量，不代表最终合成画面的相似度。

Base-image overlay is enabled by `overlayStrength`; `0.2` blends 80% tile pixels with 20% target pixels, with no separate switch. Both synchronous `/api/mosaic` and asynchronous `/api/jobs/mosaic` support it. The reference is the resized, upscaled and reflect-padded working target, sampled per tile using global coordinates and bilinear interpolation. Zero skips overlay; one fully shows the reference in successfully loaded tiles. Matching and the library remain unchanged. Missing tiles retain their failure counts and background. Optional brightness jitter runs before overlay. Analysis scores continue to describe tile matching, not final composite similarity.

示例 / Example:

```json
{
  "ok": false,
  "exitCode": 1,
  "message": "tileW must be at least 4",
  "diagnostic": {"code": "invalid_argument", "severity": "error", "field": "tileW", "message": "tileW must be at least 4"}
}
```

---

## 任务 / Jobs

### 单图格式策略 / Single-image output policy

同步 `/api/mosaic`、异步 `/api/jobs/mosaic` 和 CLI 使用相同编码规则（新 CLI 必填输出，非图片后缀必填模式）：有效的显式 `format` 优先于输出后缀，其次输入后缀，最后回退 PNG。省略 `outputPath` 默认使用无后缀的 `output/output`，无后缀文件名会追加最终后缀；显式空路径仍失败，新输出目标会按需创建父目录。支持的格式为 JPG/JPEG、PNG、WebP、TIFF/TIF，忽略大小写。输入后缀无法推导时可回退 PNG，无法读取的输入仍失败；未知输出后缀在没有显式格式时返回 HTTP 400，`diagnostic.field` 为 `outputPath`。未知显式格式的字段为 `format`。

推导的 JPG（单轴上限 65,500）或 WebP（16,383）超限时保留尺寸，改存 PNG；显式 `format: "jpg"/"webp"` 超限时缩小输出 Tile，保留格式。整数取整可能改变比例；网格任一轴的块数已超编码上限时，显式模式失败，需要 PNG/TIFF 或分块。PNG/TIFF 仍检查现有尺寸和资源上限。分块和 DeepZoom 继续使用原分片规则。

格式改变会发布 `output_format_changed` 警告，尺寸缩小会发布 `output_scaled` 警告。输入后缀回退 PNG 与显式格式调整后缀分别发布信息级 `output_format_defaulted`、`output_extension_adjusted`。兼容的后缀别名与大小写保留；冲突或回退时替换后缀。主图、预览和报告均使用实际路径。

An explicit `format` wins over the output suffix, then the input suffix, then PNG. Inferred oversized JPG/WebP switches to full-size PNG; explicit JPG/WebP reduces output tile dimensions. Empty `format` is automatic. Request restoration omits inferred formats so it does not turn an automatic choice into an explicit one.

单图计划确定后，同步结果和任务详情可包含 `runtime.outputPlan`：

| 字段 | 含义 |
| --- | --- |
| `formatSource` | `explicit`、`output_extension`、`input_extension` 或 `fallback` |
| `requestedFormat` | 尺寸适配前选定的规范格式 |
| `originalWidth`, `originalHeight` | 适配前计划尺寸，64 位整数 |
| `originalTileWidth`, `originalTileHeight` | 适配前的有效输出 Tile，已应用原有默认方向规则 |
| `format`, `width`, `height`, `tileWidth`, `tileHeight`, `path` | 计划实际输出的格式、尺寸、Tile 和文件路径 |
| `scaled`, `formatChanged` | 是否缩小 Tile、是否因尺寸超限切换格式 |

`outputPlan` 不代表文件已经保存，失败任务也可能具有计划。现有 `runtime.format/width/height` 及 `artifacts` 仍表示已保存的结果；下载只能使用已登记产物。列表摘要不携带输出计划。请求中的 `outputPath` 不改写为实际文件名；恢复参数只保留原显式 `format`。

The plan is optional and is not proof of a saved file. Existing runtime dimensions and registered artifacts describe saved output. Job summaries omit the plan; request paths retain their original meaning.

### `POST /api/jobs/mosaic`

启动异步马赛克任务 / Starts an asynchronous mosaic job.

成功提交返回 HTTP 202 和 `job` / Successful submission returns HTTP 202 with `job`.

### 新输出模式 / Output modes

马赛克请求新增 `outputMode: "image|html|deepzoom|tiles"`、`backend: "auto|cpu|cuda"`、`threads: 0..256`、`seed` 和 `overwrite`。新字段指定目录模式时 `outputPath` 为完整导出目录；未提供时旧 `tiled/deepZoom` 的文件/前缀解释保持不变。新旧模式冲突返回 400；旧字段只在入口转换，核心使用单一模式。

HTML 与显式 `format/quality/pngLevel/writeMode/generatePreview` 互斥，拒绝启用的非零亮度扰动。所有成功 create 自动记录最终排列使用情况，不依赖 analyze。固定种子在相同输入、版本和后端内复现，不承诺跨后端一致。

### 工程信息与任务 / Portable projects

工程输入接受目录、其中的 `project.json` 或 `index.html`；ZIP 先解压。HTML 是离线只读查看器，原生回导只读取权威清单和包内素材。

| 端点 | 请求 | 结果 |
| --- | --- | --- |
| `POST /api/project/info` | `projectPath` | 200，project：grid、outputTile、assetCount、appearance、seed、estimatedBytes |
| `POST /api/jobs/render` | `projectPath`, `outputPath` 及输出/外观设置 | 202，复用队列、进度、产物和历史 |
| `POST /api/jobs/project-check` | `projectPath`，可选 deep | 202，验证版本、索引、路径、大小、SHA-256 和快照；deep 增加解码/尺寸检查 |
| `POST /api/jobs/project-pack` | `projectPath`, `outputPath`，可选 overwrite/deep | 202，STORE ZIP64，包含完整顶层工程目录 |

render 示例：

```json
{
  "projectPath": "artwork/index.html",
  "outputPath": "rendered.png",
  "nativeTileW": 360,
  "nativeTileH": 640,
  "overlayStrength": 0.2,
  "brightnessJitter": 0.04,
  "seed": 42,
  "threads": 4
}
```

render 接受 outputMode、format、nativeTileW/H、quality、pngLevel、writeMode、generatePreview、overlayStrength、brightnessJitter、seed、threads、benchmark、overwrite、resourceMonitor；拒绝匹配尺寸、候选、权重、惩罚和 backend。nativeTileW/H 成对提供。已知图片后缀可推导 image，其他路径必须指定模式。默认沿用工程尺寸、外观和种子；seed 只影响外观，放大只重采样包内素材。HTML 要求 brightnessJitter 为零。

回导不访问原图库、不匹配、不 CUDA 预检、不修改源工程、不增加使用统计。派生缩略图缺失不阻止原生图片渲染，但完整性检查和打包仍报告缺失。查看器不依赖服务、CDN、模块请求或运行时 JSON 读取。

工程和 ZIP 暂存成功后发布，失败恢复旧输出。overwrite 只替换无额外文件的可识别导出目录；保护目标图、图库及源工程。入口 HTML 和工程清单的产物下载返回 409，应先提交 project-pack，再下载 archive 产物。

### `POST /api/jobs/build`

新增 `backend` 字段控制加速策略；`threads: 0` 自动，正数为应用 CPU 工作线程上限。纯归一化使用 `normalizeOnly: true`，不创建数据库，拒绝输入输出重叠。新 CLI `library build` 只建新库、`library add` 继承现有规格，API 保留已有字段。

启动异步图库构建任务 / Starts an asynchronous library build job.

默认对全库按平均颜色的 HSV 色相和明暗排序并从 `000000` 连续编号，`appendMode: true` 也会重新排列旧图片。输入与输出同目录时也更改已索引原图的文件名。图片 ID、使用统计和图片内容保持不变；`normalizeOnly: true` 不执行全库重排。
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
| `runtime` | `featureBackend/scoreBackend/fallbackReason`、实际 `format/width/height/matched/failed`、`metrics`，以及可选 `outputPlan` |
| `artifacts` | 已成功关闭的文件/目录：`id/kind/path/mime/bytes/directory`；路径为规范化绝对路径 |
| `request` | 规范请求参数，供页面恢复编辑；恢复不会自动执行 |
| `canCancel` | 仅排队任务为 `true` |

后端值为 `cpu`、`template_cuda`、`compatible_cuda` 或 `none`；未检测时为空。`metrics` 仅在采集后含数据，不能将空对象解释为耗时为零。阶段计数最多每 200ms 更新一次，阶段切换及诊断立即可见；归一化与特征处理可能并行，应分别显示，不拼接整体百分比。阶段结束、计数达到总量或主图落盘不代表任务已经成功。

异步建库和生成接受可选 `resourceMonitor: true`。`runtime.resources.samples` 最多保存最近 600 个约每秒采集的样本，含相对时间 `elapsedMs`、`cpuPct`（单核为 100%）、`rssBytes`、`gpuPct`、`deviceVramBytes` 和 `processVramBytes`。缺测值为 `null`。`summary` 汇总整个任务的可用样本峰值和均值。开启 `benchmark` 后的耗时子阶段可能重叠。

`gpuPct` 和 `deviceVramBytes` 描述运行 CUDA 的设备。进程显存以驱动可提供的数据为准，Windows WDDM 下可能缺测。 [NVIDIA NVML 进程显存文档](https://docs.nvidia.com/deploy/nvml-api/620/api/structnvmlProcessInfo__t.html)

主图成功、预览或使用统计随后失败时，任务按原规则失败，已经保存的 `main` 产物仍然可用。报告失败沿用原有软失败/异常与提交边界。

### `GET /api/jobs/{id}/events?after=0&limit=100`

返回 `sessionId`、`events`、`firstSequence`、`nextCursor`、`dropped`、`truncated`。`after` 为排他游标，`limit` 为正整数，默认及最大值均为 100。持续使用返回的 `nextCursor` 获取增量；游标超过当前尾部会收敛到尾部。

每任务最多保留 256 条、约 256 KiB 的事件（按内部记录占用计量）；消息和路径分别最多 4096 UTF-8 字节，原因码和字段最多 128 字节，在字符边界截断。旧游标已落后于缓冲时 `truncated: true`；`dropped` 是累计淘汰条数。警告总数和终止错误不随事件淘汰丢失。

### `GET /api/jobs/{id}/artifacts/{artifactId}`

只接受任务已登记的产物 ID，不接受文件路径。成功按登记 MIME 流式返回文件，支持 HEAD 与字节 Range；主图及 HTML 以附件下载，JPEG 预览和分析缩略图可内联显示。使用固定 64 KiB 缓冲，不将整图读入内存。

文件不存在或记录已清理返回 404；文件路径、大小或修改时间与登记指纹不一致返回 409（`artifact_changed`）。目录不能下载，仅在详情中提供真实路径。分块为 `tiles` 目录，DeepZoom 另有 `manifest` 和 `viewer`；本接口不打包目录，也不托管完整 DeepZoom 查看器的关联文件。

开启 `analyze` 且有评分结果时，可登记 `analysis-data`（`application/json`）：

| 字段 | 内容 |
| --- | --- |
| `grid` | 原网格 `columns`、`rows`。 |
| `scores` | 选图误差的 `min`、`median`、`p90`、`p99`、`max`，分数越高表示合成前的匹配偏差越大。 |
| `heatmap` | `columns`、`rows` 各取原网格对应轴与 256 的较小值。`bins` 按行排列，`-1` 表示区域无评分，`0..255` 表示区域最大误差在 `min..max` 内的量化值。原格坐标乘以热力图轴长再除以原网格轴长后向下取整。分数完全相同时用 `0`。 |
| `worst` | 按误差降序的最多 20 项，包含从 0 开始的 `row`、`column`，以及 `score`、`imageId`。`targetArtifactId` 和 `matchArtifactId` 分别引用已写入的目标裁块、入选素材 JPEG，最长边 256 像素。 |

分析数据和缩略图均是可选产物，可用性以任务登记及实际读取结果为准。

`scores.histogram` 为原始逐格分数在 `min..max` 内的 32 档数量，等分时计入首档。

### `DELETE /api/jobs/{id}`

取消排队的任务。运行中的任务不会被中断，返回 HTTP 409。
Cancels a queued job. Running jobs are not interrupted and return HTTP 409.

### `GET|DELETE /api/history`

`GET` 返回 `history`，按结束时间从新到旧排列，最多 100 条。每条记录包含服务会话和任务 ID 组成的 `key`、请求参数、状态、输出路径和体积，以及可用的质量、阶段计时和资源摘要。Web UI 将其保存到工作目录下的 `.mosaicraft/history.sqlite3`（独立结构版本 1），重启后可继续读取。`DELETE` 返回已清除的摘要数 `removed`。成品、图库和当前服务的任务列表均可继续使用。

`GET` lists up to 100 completed jobs with request, output, quality, performance and resource summaries. The Web UI stores records in `.mosaicraft/history.sqlite3` under its working directory. `DELETE` returns the number of removed summaries.

---

## 马赛克 / Mosaic

### `POST /api/mosaic/plan` 与 `POST /api/mosaic/sample-size`

规划接受标准生成参数，返回 `plan` 中的工作图、网格、成品尺寸、有效 Tile、实际格式、路径、文件数量上界及警告。`rawBytes` 为未压缩 RGB 体积，`matchingRam`、`matchingVram`、`writingRam`、`fileBytes` 为 `{low, high}` 字节估算。CPU 构建或强制 CPU 时显存项为空。规划不执行匹配或写入输出。

采样额外接受 `previewId` 和可选 `previewEdits`，返回 202。通过现有预览 GET/DELETE 接口查询或取消。成功状态含 `preview.fileSample`，记录 `bytes`、`pixels` 和 `regions`。最多抽取 16 个区域、编码 400 万像素。分块按 JPEG 文件数外推，DeepZoom 按各层的像素和 JPEG 文件数外推。采样成功后返回估算区间。

预估区间以平色、渐变和随机纹理样本的编码结果校准。独立的 Windows CPU 验收使用 37×65 目标图、5×5 网格、18×32 输出 Tile 和固定选图。JPG、WebP、PNG、TIFF 的实际体积均落在模型和采样区间内。采样区间中点误差分别为 0.0%、0.2%、0.1%、33.0%，样本仅覆盖这一组参数。

另一组留出样本用 73×99 目标图和 9×7 网格。分块 JPEG 和 DeepZoom 的实际体积分别为 40,506 和 12,695 字节，均落在两种区间内。采样中点误差分别为 7.5% 和 20.5%。各层 JPEG 和查看文件数与规划一致。

### `POST /api/mosaic`

同步执行马赛克请求。新 UI 代码应优先使用 `POST /api/jobs/mosaic` 处理长时间任务。
Runs a mosaic request synchronously. New UI code should prefer `POST /api/jobs/mosaic` for long-running work.

成功生成后返回 HTTP 200 和服务结果，不返回 job。同步生成和异步建库/生成共用串行执行锁，以保证特征缓存写入一致。
Returns HTTP 200 with a service result after successful generation, without a job. Synchronous generation and asynchronous build/mosaic pipelines share a serial execution lock to keep feature cache writes consistent.

两个生成端点默认允许超大输出，无需额外字段或开关。目标缩放/上采样后的工作图像最多 1 亿像素。JPEG/WebP 超限时按统一编码策略改用 PNG 或缩小输出图块；网格本身超过编码上限时失败。PNG/TIFF 可指定 `writeMode: "stream"` 降低内存占用。不兼容的旧图库必须在新目录重建，当前更新不迁移或重建已有有效图库。
Both mosaic endpoints allow large output by default, without an extra field or switch. Resized/upscaled working targets are limited to 100 million pixels. JPEG/WebP overflow follows the format policy above; a grid exceeding codec limits fails. PNG/TIFF can use `writeMode: "stream"` to reduce memory use. Unsupported libraries must be rebuilt in a new directory before generation.

两个生成端点均接受 `generatePreview: true`，默认 `false`，等价于 CLI `--thumbnail`。只支持单图；与 `tiled` 或 `deepZoom` 组合返回 400，诊断字段为 `generatePreview`。沿用宽度最多 1400、总像素最多 400 万、不放大的 JPEG 预览，质量固定为 92。同步响应只返回产物元数据，使用任务端点生成才能通过任务产物接口下载。
`generatePreview` is optional and off by default. It uses the existing bounded JPEG preview generator and is incompatible with tiled/DeepZoom output.

### 实时预览 / Interactive preview

发现 `interactive-preview` 功能后，向 `POST /api/mosaic/preview` 提交 `{"request":{"inputPath":"target.png","dbPath":"library/mosaicraft.db"},"seed":42}`。请求使用标准生成参数。返回 202 和 `preview.id`。预览采用最新请求，正式任务优先，预览不进入普通任务列表。

`GET /api/mosaic/preview/{id}` 返回 `preview.state` 和 `preview.progress`。`progress.stage` 为 `queued`、`validating`、`reading_library`、`building_index`、`matching` 或 `finalizing`；`progress.activities` 中的 `matching` 活动包含 `completed`、`total` 和 `unit`，`total` 未知时为 `null`，可用于显示匹配进度。完成后状态为 `succeeded`，包含 `columns`、`rows`、行优先排列的 `imageIds`（`-1` 表示无候选）、按素材 ID 索引的 `assetVersions` 和标识网格及图库的 `editKey`。实际匹配后端见 `featureBackend`、`scoreBackend` 和 `fallbackReason`，阶段耗时见 `metrics`。实时预览沿用生成的工作图尺寸校验，并返回完整的匹配网格。向 `GET /api/mosaic/preview/{id}/image/{asset}` 请求 JPEG，`asset` 为 `reference` 或 `<imageId>-<size>`，尺寸支持 16、32、64、128，素材 ID 必须来自该预览的图库。使用 `DELETE /api/mosaic/preview/{id}` 取消或释放预览。

`reference` 返回最长边不超过 1400 像素的目标工作图，可与选图网格共用同一视角。Web UI 在当前预览上应用替换，再用有效素材 ID 判断上下左右是否重复。

发现 `preview-editing` 后，可用 `GET /api/mosaic/preview/{id}/recommendations?position=0` 查询单格最多 60 张推荐，实际数量取决于匹配候选数和通过筛选的素材数。位置从 0 开始按行计数。`GET /api/mosaic/preview/{id}/library?q=&offset=0` 按素材 ID 顺序每页返回 24 张，可按文件名或 ID 过滤。两种列表的每项均包含 `id`、`name`、`version` 和 `available`。

`POST /api/mosaic/preview/quality` 接受 `previewId` 和最多 512 条 `positions`。每条包含零起始 `position`，手动替换额外带 `imageId/version`。`explain: true` 限单格，并返回裁块 JPEG、入选和候选的五项特征误差，以及可用的原始排名、Top-N 和惩罚。预览误差由服务重新评价，源文件变更返回 409。

发现 `preview-comparison` 后，使用 `POST /api/mosaic/preview/pin` 提交 `{ "slot": "A", "previewId": "…" }` 保留已完成的预览。槽位为 `A` 或 `B`，传入空 ID 可释放槽位。`POST /api/mosaic/preview/compare` 接受 `{ "a": { "previewId": "…" }, "b": { "previewId": "…" } }`，各侧可附上 `previewEdits`。目标、图库版本、网格和随机种子必须相同。返回 202，可通过预览 GET/DELETE 查询和取消。

完成后 `preview.comparison` 包含各侧的 `matched`、`unique`、`mean`、`p90` 和概览 JPEG（`image`），以及 `compared`、`changed`、`mapColumns`、`mapRows` 和行优先的 `changedMap`。均值和 P90 在双方均匹配且评分可用的格子上用默认五特征权重计算。

向 `POST /api/jobs/mosaic` 提交同一组匹配参数及 `previewId`，可直接使用预览中的素材排列。提交时过期、目标或图库变更、参数不匹配返回 409 `preview_stale`。执行前再次检查来源。正式任务一旦入队就持有选图结果，关闭预览不会撤销已提交的任务。

可同时提交 `previewEdits`，每项包含 `position`、`imageId` 和列表中的 `version`。每个位置最多出现一次，图片可来自整个预览图库。选中的文件变更时返回 409。手动替换不重排其他格子，开启质量分析时按实际输出的排列重算特征误差。

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

`lHistogram` 在非空图库中返回 8 档 `{lo, hi, count}`，覆盖保存的 LAB L 值 0～255。`coverageGaps` 为覆盖缺口代码。

`colors` 包含 `ab`（32×32，行按 b，列按 a）、`hue`（24 档 LAB 色相角）、`lightness`（8 档）、`gray` 和 `total`。建库任务详情 `runtime.build` 提供 `scanned/normalized/duplicates/inserted`，完成索引的任务还包含 `before/after` 色彩汇总。

`POST /api/mosaic/coverage` 接受标准生成参数。`coverage.target/library` 使用同一分桶。按补边后的目标工作图块平均 LAB L 和请求的 `lRange` 统计亮度候选。`minimumCandidates` 是每个区域的最小候选数，区域网格最多 256×256。

`POST /api/db/catalog` 接受 `dbPath`、`sort`（`color`、`usage` 或 `similarity`）和相似度参照 `anchor`（素材 ID）。使用 `GET /api/db/catalog/{id}?offset=24` 翻页。缩略图通过 `GET /api/db/catalog/{id}/image/{asset}` 读取，`asset` 为素材 ID、连字符和把冒号换为连字符的 `version`。

### `GET|POST /api/db/health`

返回健康警告和建议 / Returns health warnings and recommendations.

`missingFiles` 为缺失素材文件数，`usage` 提供已用与未用素材的数量和百分比。`hotspot.topHotspotTilePct` 是按 `tiles` 排名前 `topHotspotCount` 张素材累计占用的格数比例。

### `GET|POST /api/db/usage`

返回已记录使用统计 / Returns recorded usage statistics. 浏览、检查、打包和回导不计数。

`sort: "targets"`（默认）按独立目标数 runs 排序，`sort: "tiles"` 按累计格数排序；limit 为 0 返回全部。同一目标图仅首次统计时增加 `runs`，后续任务仍累计 `tiles`（填充格数）。

常用 JSON 字段 / Common JSON fields:

```json
{
  "dbPath": "library/mosaicraft.db",
  "limit": 50,
  "showUnused": false
}
```

### `POST /api/db/usage/export`

selection 支持 used（默认）、unused、all；复制素材，不创建数据库。未使用表示未出现在已记录使用中，不推算历史缺失数据。此端点有文件系统副作用，需要 `confirm: true`。
Exports used images ordered by total tile count. This endpoint has filesystem side effects and requires `confirm: true`.

```json
{
  "dbPath": "library/mosaicraft.db",
  "outputDir": "used-images",
  "confirm": true
}
```

### `POST /api/db/purge`

只报告或移除确认缺失文件的记录，权限错误、损坏特征、覆盖不足及未使用不作为删除依据。事务更新记录及关联统计，提交后失效缓存并清理无引用特征。默认为干运行。实际删除需要 `dryRun: false` 和 `confirm: true`。
Previews or removes orphan database records. Default is dry-run. Actual deletion requires `dryRun: false` and `confirm: true`.

```json
{
  "dbPath": "library/mosaicraft.db",
  "dryRun": true,
  "confirm": false
}
```

---

## 严格输入规则 / Strict inputs

仅接受端点元数据列出的规范字段。未知字段、旧别名、重复 JSON 键、重复查询参数、同一字段重复出现在查询和正文中，以及不支持的 null 均返回错误。整数参数只接受 JSON 整数类型，查询布尔值只接受 true/false。输入类型不会自动转换。

生成使用 topNRandom、useGpu、nativeTileW/nativeTileH；建库使用 normalizeWidth/normalizeHeight。CLI 的 --cpu、--output-tile、--normalize-size 便捷参数仍然可用。/api/run 与其环境变量开关已删除。

HTTP 边界与业务错误使用相同的 ServiceResult/Diagnostic 基础结构；成功响应的业务载荷名称及任务状态模型保持不变。
