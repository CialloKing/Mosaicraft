# 边界与一致性修复验证

验证日期：2026-10-11。基线：`bc132cf`（v1.14.1）。本轮修复用户提交的 14 项编号问题和 15 项 P3 审查问题，随 v1.15.0 发布；下表保留该轮验证时的用例数量。

## 编号问题

| 编号 | 修复行为 | 验证 |
|---|---|---|
| 1 | 重复建库使用新文件名；仅删除本轮生成的重复文件；输入等于输出时保留原图并正常索引 | CPU/CUDA 重复与同目录建库，文件哈希保持不变 |
| 2 | 在 GPU 上传和分析前拒绝旧版本、NULL、非 192 维或非有限 grid | NULL/48 维库在两后端正常返回失败，无访问冲突 |
| 3 | 5 个维护/检查子命令拒绝未知参数 | 五条 CLI 错误路径均返回非零 |
| 4 | 建库与维护使用不同数据库打开模式；维护不创建文件，验证列与 SQLite 数据页完整性 | 缺失 DB 未创建；损坏 DB 返回错误 |
| 5 | CPU/CUDA 均以落盘图像解码后的像素计算哈希；GPU 批内也去重 | 跨后端追加相同图片保持 5 行 |
| 6 | 共享尺寸上界校验，使用 64 位计算；服务捕获分配异常；校验 DB 尺寸元数据 | 巨大 upscale 和损坏尺寸元数据被拒绝 |
| 7 | 格式大小写归一，拒绝未知格式；单轴缩放有效、0 保持自动；不再隐式切换格式或缩小输出；超大输出默认允许 | PNG 签名、单轴/0 尺寸、JPEG 超限及超大输出进入编码器的检查 |
| 8 | GPU 建库补齐标准差、饱和度与颜色方差，复用 CPU 统计函数 | 同批 CPU/CUDA 三列一致且非零 |
| 9 | 建库和查询统一使用 OpenCV Canny 边缘密度 | CPU/CUDA 统计一致，真实 CUDA 单测对照 Canny |
| 10 | CPU/CUDA 均按 lRange 过滤 ANN 候选 | 宽范围可生成，零范围无候选时返回错误 |
| 11 | 统一距离尺度、归一化权重和 usePenalty | 评分代码核对，真实 CUDA 分批与惩罚断言 |
| 12 | CPU 8×8 grid 使用与 CUDA 相同的 ceil 分区，覆盖尾部像素 | 核心特征测试与两后端构建/生成 |
| 13 | CLI、API、直接服务拒绝 NaN/Infinity 与非法权重 | 非有限 penalty 拒绝；服务回归覆盖 |
| 14 | README 中尺寸示例统一为单 token WxH | 帮助、README、命令生成器语法一致 |

## P3 审查问题

| 问题 | 修复与验证 |
|---|---|
| 同步 `/api/mosaic` 违约 | 完成生成后返回 200 服务结果；异步端点仍返回 202 job，HTTP 实测通过 |
| 跨站维护请求 | 校验 Host/Origin/Sec-Fetch-Site/请求体类型；purge 仅 POST，usage export 保持 POST；HTTP 403/415 和 GET 拒绝实测通过 |
| 非法 UTF-8 空体 500 | query 先校验 UTF-8/NUL，错误使用 JSON；`%FF` 实测为 400 JSON |
| 取消任务竞态 | 出队与置 Running 在同一锁内完成；执行入口要求 Running；排队取消单测通过 |
| FeaturePack ID 未校验 | 缺失、重复和不匹配 ID 均拒绝缓存；单测通过 |
| usage 孤立行/错误百分比 | 启用外键，兼容旧表的事务删除，统计 JOIN 当前图片；purge 后百分比实测在 0..100 |
| 并发统计静默丢弃 | busy_timeout=5s，BEGIN IMMEDIATE 后检查目标哈希，事务结果上报；双连接并发测试累计 tile=4、独立目标=1 |
| 0 字节图库文件导致整体失败 | 空文件/解码错误返回空图，输出计入 loadFail；合成空文件实测通过 |
| 截断 PNG 被当成成功 | close 检查完整行数、libpng 和 fclose 状态；只写 1000/1001 行的 PNG 单测返回失败 |
| DeepZoom 失败被吞掉 | 传播目录/图像/manifest/viewer 写入失败；成功前保留 `_source_tiles`，manifest 失败实测可恢复源 tile |
| brute 删除失效迭代器 | erase 前保存位置，正确处理末元素；删除与标签迁移单测通过 |
| 中文 tiny/hist 路径 | 文件流使用 filesystem UTF-8 路径，并上报写入失败；中文特征文件实测存在 |
| 中文 ANN 路径 | 索引文件流使用 filesystem UTF-8 路径；中文保存/加载单测和第二次命中实测通过 |
| 空 indices 残留评分 | CUDA 与 CPU-only stub 先初始化 DBL_MAX；两种构建单测通过 |
| TIFF 输出元数据不同 | 两构造路径共用 DPI 和压缩策略，仅 strip 布局不同；两模式 TIFF 标签及像素验证通过 |

同步 HTTP 生成与异步建库/生成共用管线锁，防止非重入特征缓存写入重叠。GPU 建库线程的异常会通知生产者，避免满队列等待无法结束。

## 最终验证

| 构建 | 核心单测 | 服务回归 | Web UI/API smoke |
|---|---|---|---|
| CUDA Release | 48/48，551 条断言 | 11/11，28149 条断言 | 通过 |
| CPU-only Release | 48/48，535 条断言 | 11/11，28149 条断言 | 通过 |

另完成 **77 项独立 CLI/SQLite/图像/HTTP 检查**，包含实际 CUDA 建库和生成。Web UI 的 CLI 命令生成、JSON 请求字段、0 尺寸与 API 主版本经 JavaScript 执行验证。超大输出检查使用不可写路径，验证其进入编码器后返回 I/O 错误，避免实际写入巨型文件。全部文件操作使用合成图库和临时目录。

复现入口：`ctest --test-dir build -C Release --output-on-failure`、`ctest --test-dir build_nocuda -C Release --output-on-failure`；两种构建均执行 `mosaicraft_webui_smoke` 目标。独立检查脚本与日志保留于本地忽略目录 `output/bug-audit-20261009/`，最新检查目录由 `latest-fix-run.txt` 指示。

## 使用变化

- 旧图库需再次运行 `build --append` 刷新为 **v6**，图片 ID、文件和使用统计保留。缺失文件需先恢复或通过 purge 清理；非空图库改变归一化尺寸需新目录和数据库。
- 超大输出默认允许，不设置额外体积门槛。目标与单 tile 最多 1 亿像素；JPEG/WebP 超限返回错误。
- HTTP API 合约为 **2.0**；维护清理仅 POST，结构化请求体使用 JSON，同源界面已同步升级。
- v6 修正了特征和评分语义，选图可能与旧版不同；CUDA 路径增加 CPU Canny 步骤。本轮未重跑大型图库性能基准，也不保证 CPU/CUDA 浮点特征或最终选图逐位一致。
