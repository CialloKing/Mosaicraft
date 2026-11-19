# 性能指标说明

`create`、`render` 和 `library add` 在 `--benchmark` 下记录任务级聚合指标。CLI `--json`、API 任务详情和 Web UI 诊断里的 `runtime.metrics` 可直接读取，无需解析控制台文字。以下时间均为毫秒，`Bytes` 为字节；未执行或未采集的新增耗时不返回。已有键（包括原先会返回零的字段）保持兼容。`totalMs` 仍是原有的服务内部区间，不是包含进程启动和退出的外部墙钟。

## 生成和工程渲染

| 指标 | 口径 |
| --- | --- |
| `threadsRequested`；`featureWorkerSlots`、`annWorkerSlots`、`cpuBaseScoreWorkerSlots`、`matchingPrefetchWorkerSlots`、`outputWorkerSlots` | 请求值 `0` 表示自动。槽位是各阶段实际配置的工作名额，不是活跃线程数；共享池不可重复相加。CPU `fast` 的分派看 `runtime.kernelVersion`，实际后端和回退另看 `featureBackend`、`scoreBackend` 和 `fallbackReason`。 |
| `placementMs`、`encodingMs` | 放置和写出区间。流式放置和编码可以并行；Deep Zoom 的放置包含构建金字塔，`deepZoomLevel<N>Ms` 是子区间。 |
| `decodeWorkerMs`、`resizeWorkerMs`、`outputLoadingWorkerMs` | 前两个只统计单图管线实际加载方的工时，不包含等其他线程填充缓存；`outputLoadingWorkerMs` 覆盖缓存访问，包含等待。均是可重叠的工作线程工时。 |
| `codecWriteMs`、`outputColorMs`、`outputPreviewMs` | 已启用的写出子阶段。`codecWriteMs` 只计行编码调用；整体编码看 `encodingMs`，预览已经计入其中。 |
| `outputProducerWaitMs`、`outputConsumerWaitMs` | 双缓冲的生产者等空条带、消费者等就绪条带。未启用流水线时不返回。 |
| `cacheHits`、`cacheDecodes`、`cacheSharedLoads`、`cachePeakBytes` | 输出素材缓存计数及其内部峰值；不是进程工作集。另有叠加素材时提供叠加工作线程工时和缓冲预算。 |
| `cpuFeatureResizeWorkerMs`、`cpuLabConvertWorkerMs`、`cpuGridMeanWorkerMs` 等 | CPU 特征算子的累计工时，不是各自占用的进程墙钟。 |
| `libraryGpu*`、`featureGpu*`、`scoreGpu*`；`featureCudaEvent*`、`scoreCudaEvent*` | 对应已采集的 CUDA 主机侧操作、等待、传输/缓冲字节与设备事件。设备事件耗时不是主机等待；同步路径未采集的设备事件不返回。 |

## Web UI 交互预览

`POST /api/mosaic/preview` 的 `request.benchmark: true` 在成功结果的 `preview.metrics` 中增加缓存诊断。`previewScoresReused`、`previewTileFeaturesReused` 和 `previewCandidatesReused` 保留原口径：直接重放评分时也会报候选已复用（不访问原始 ANN 缓存），不报告目标特征查找结果。

| 指标 | 含义 |
| --- | --- |
| `previewCacheTier` | `0`：内存余量不足，不保留这三类缓存；`1`：标准；`2`：允许扩容。不知道可用内存时使用标准档，不自动扩容。 |
| `previewCacheBudgetBytes`、`previewCacheReservedBytes` | 本任务可用的合计上限、汇总时进程内三类缓存的预约。仍被 A/B 或推荐列表持有的评分表算在其中；共享引用不重复计费。 |
| `previewScoresBytes`、`previewTileFeaturesBytes`、`previewCandidatesBytes` | 当前评分对象、工作区内目标特征和原始候选的预约。分配按对象和容器容量核算，不是 RSS，也不包含分配器元数据、图库索引、目标位图和临时匹配缓冲。 |
| `previewScoresMissReason`、`previewTileFeaturesMissReason`、`previewCandidatesMissReason` | `0` 命中；`1` 物理内存余量不足；`2` 活对象占用进程预算；`3` 单项超限；`4` 特征源版本不可用或变化；`5` 首次/键不同，正常计算；`6` 可选缓存分配失败。不查的缓存不报告原因。 |
| `previewCacheEvicted` | 本次从工作区移除的三个缓存槽位计数，不保证对象已释放：旧预览可能仍然引用评分表。 |
| `previewFeatureSourceVersionMs`、`previewFeatureSourceVerifyMs` | 本次查验图库特征源版本的墙钟，以及构建新评分表后复验的墙钟。分别属于准备/匹配的子区间，不与总时长相加。 |
| `previewReferenceEncodeMs` | 在最终整理阶段缩放并编码参考 JPEG 的墙钟。 |

`totalMs` 不包含客户端轮询、HTTP 响应传输和画布绘制。基准应单列 API 完成时间和浏览器首次绘制，不把两个指标混用。

## 图库追加

| 指标 | 口径 |
| --- | --- |
| `normalizeWorkerMs` | 素材读取、解码、归一化及其编码和写入子区间的工作线程工时。 |
| `assetEncodeWorkerMs`、`pngCompactWorkerMs`、`assetWriteWorkerMs` | 新编码、同尺寸 PNG 重压缩**尝试**、素材文件写入，三项分开。直接复用同目录素材不计编码或素材写入。 |
| `pixelHashMs`、`sqlLookupMs`、`batchDedupMs` | 依次为像素哈希、SQLite 查重调用、批内集合查重；SQL 已命中的素材不进入批内集合。 |
| `sqlLookupCount`、`sqlDuplicateCount`、`batchDuplicateCount`、`committedBatchCount` | SQL 查询与命中、本批重复、成功提交的批次数。无对应事件时计数可以为零。 |
| `batchTransactionMs`、`batchCommitMs` | 批事务从 `BEGIN` 到结束；`COMMIT` 是其中的子区间。两者均不包含全库重排的其它事务。 |
| `normalizeQueueProducerWaitWorkerMs`、`normalizeQueueConsumerWaitMs` | 模板 CUDA 管线中，生产者等队列腾位的累计工时、消费者等输入的耗时。只计发生等待的调用，不能解读成 GPU 空闲。 |
| `normalizeWorkerSlots`、`libraryFeatureWorkerSlots` | 归一化及特征所在的主机工作名额；模板 CUDA 的特征阶段是一个主机消费者。 |

追加成功后才发布完整诊断；失败看结构化错误。`normalizeMs` 仍是原来的粗粒度指标；并行归一化、特征与等待相互重叠，不要把它们加到 `totalMs`。要判断是否值得优化 SQL，用同一次诊断运行的 `sqlLookupMs / 外部进程墙钟毫秒`，后端与重复率分别计算；这不是不带诊断的性能基准。`--resource-monitor` 独立于 `--benchmark`，其峰值来自采样，不是全过程精确峰值。
