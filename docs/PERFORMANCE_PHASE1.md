# 第一轮缓存与建库优化验收

测量日期：2026-10-13。软件版本保持 **1.14.2**，匹配特征版本保持 **v6**，HTTP API 合约保持 **2.0**。本轮没有发布 Release。

## 结果

CPU 建库、颜色检查与缓存复用、CUDA 目标特征阶段均提速。五个代表场景的总耗时变化均在计划的 5% 回退上限以内；CUDA JPG 总耗时增加 4.7%，其全库准备阶段包含新增的来源校验成本。逐次数据、阶段统计、输入及二进制摘要见 [JSON 记录](performance/phase1.json)。

| 场景 | 基线中位数 | 更新后中位数 | 总耗时变化 | 基线 / 更新后峰值内存 |
|---|---:|---:|---:|---:|
| CPU 新建库，1,024 张 | 7.076 s | 4.527 s | -36.0% | 28.8 / 77.7 MiB |
| 无新增追加，45,625 张 | 18.487 s | 8.833 s | -52.2% | 240.4 / 263.5 MiB |
| CUDA JPG，batch | 2.783 s | 2.913 s | +4.7% | 585.2 / 585.3 MiB |
| CPU JPG，batch | 3.529 s | 2.786 s | -21.1% | 482.7 / 483.0 MiB |
| CUDA PNG，stream | 7.675 s | 7.669 s | -0.1% | 274.3 / 274.8 MiB |

峰值取三次正式运行中最大的 Windows `PeakWorkingSetSize`，只表示主进程的主机内存，未测显存。本轮 CPU 并行建库增加了在途图像和工作区，因而峰值内存上升。64 MiB 是待处理像素预算；特征工作区、数据库、归一化和缓存也占内存。

## 测量条件

- 基线为更新前提交 `54a249c` 的独立 CPU/CUDA 二进制；最终计算实现包含至 `77d00df`。采样时包含新的分阶段日志；`05f4955` 最后修正了零工作量阶段的显示和总计时终点，计算流程未改变。
- Windows 10 19045 x64、Ryzen 9 7940H、RTX 4060 Laptop、驱动 616.92、CUDA 13.3.33、Visual Studio 18 2026 Release。可见内存约 31.22 GiB。
- 每个场景每个版本预热一次，之后各运行三次，按基线/更新后交替执行并在下一轮反转顺序，取进程墙钟中位数。采样轮询间隔 25 ms；本机固定样本的结果不代表所有图库。
- 图库为 45,625 张、归一化尺寸 180×320 的数据库快照。图片与独立特征使用私有目录中的硬链接，数据库和合并缓存独立保存；正式图库数据库 SHA256 与测试前一致，源文件字节未改动。
- CPU 新建库使用其中 1,024 张图片、`--threads 8`，每次创建新的输出目录。无新增追加使用空输入目录，颜色顺序和两种缓存均已预热。
- 生成使用 `test_input_photos/target5.png`、`--out-w 360 --out-h 640 --topn-random 1`，共 1,600 个 tile，输出 7200×12800。颜色调整关闭；每次恢复相同数据库及使用计数。正式性能采样关闭分析，另行比较分析模式的选图使用统计和解码像素。

读取正式旧 v2 包时发现 TinyImage 和 LBP 均有 45,625 条载荷与对应独立特征不同。因此私有基线包先从独立文件重建，使性能和输出比较使用有效的 v6 数据。错误缓存的亮暗样本单独验收。正式旧包未修改；新版生成回退独立文件，下次建库会发布带来源校验的新代次。

## 阶段收益与计时范围

| 阶段 | 基线 | 更新后 | 说明 |
|---|---:|---:|---|
| CPU 建库特征处理 | 3.3 s | 0.9 s | 含读取、去重、计算和 SQL；旧日志精度为 0.1 s |
| 无新增追加颜色检查与排序 | 15.0 s | 5.3 s | 新版路径及颜色键校验中位数 5190.6 ms，排序 36.8 ms，写入 0 ms |
| 无新增追加缓存来源校验 | 未单列 | 254.8 ms | 复用同一清单及代次，缓存写入 0 ms；数据库记录加载另计 |
| CUDA JPG 目标特征准备与提取 | 241.9 ms | 131.0 ms | 降低 45.8%，缩放及 Canny 复用线程池 |
| CUDA PNG 目标特征准备与提取 | 263.7 ms | 139.1 ms | 降低 47.3% |
| CPU JPG 候选特征加载 | 783.4 ms | 131.5 ms | 共用来源校验及包读取 |

CUDA JPG 全库准备为 1394.3 → 1653.0 ms，包含数据库、ANN、来源验证及 GPU 上传；目标准备的收益被这部分校验开销部分抵消。Windows 大图库通过目录批量枚举获取特征属性，失败、大小写别名或未找到项仍回退原属性查询，最终总耗时回退控制在 5% 内。

归一化和 GPU 特征处理存在重叠；输出加载是工作线程时间之和，贴图、编码和等待也重叠。父阶段包含子阶段，各项中位数可能来自不同次运行，不能直接求和还原总耗时。总耗时以进程墙钟为准。JSON 保留采样时的日志；无新增追加中的旧 normalize/features 字段包含启动准备耗时，最终日志已将这些零工作量阶段显示为 0。

## 正确性与故障验收

| 项目 | 结果 |
|---|---|
| CUDA 构建与 CTest | 49 个核心用例、16 个服务回归用例全部通过，CTest 2/2 |
| CPU-only 构建与 CTest | 同上，CTest 2/2 |
| Web UI/API smoke | 两种构建均通过；保留 API 2.0 及原参数 |
| 独立特征基准工具 | 两种构建均编译通过，不加入默认测试负担 |
| CPU 新建库顺序与特征 | 预热及三次运行的图片 ID、像素哈希、平均 LAB、Grid 数据前后逐条相同 |
| 正常生成 | 三种场景所有新旧输出 SHA256 相同；同一后端解码像素最大差为 0 |
| 分析模式选图 | CPU/CUDA 各自的新旧图片 ID 使用直方图一致，解码像素最大差为 0 |
| 亮暗错误缓存样本 | 修复前 CPU 选亮图 ID 2、GPU 误选暗图 ID 1；修复后均选 ID 2，显示评分 0 |
| 缺失 TinyImage/LBP | CUDA 缺失项各贡献距离 1，与 CPU 惩罚一致；不把零向量视为有效特征 |
| 缓存来源与发布 | 覆盖中文路径、旧 v2 刷新、源文件变化、包损坏/截断、缺失特征、未发布代次、Windows 发布被文件占用阻止 |
| 无新增追加 | 缓存清单及当前代次的时间戳、大小不变，日志显示 reused 和 0 ms 写入 |
| 建库事务与重编号 | 覆盖追加、重复图片、同目录建库、路径冲突和已有恢复机制；SQL 故障回滚当前 CPU 批次 |

本轮运行验收范围为 Windows CPU/CUDA，未进行 Linux 实机验收。缓存故障用例复用现有 doctest；没有增加大型测试框架。

## 复测命令与后续重点

使用私有图库快照，并在每次生成前恢复数据库使用计数。以下生成命令分别由基线和更新后的独立二进制运行；CPU 加 `--cpu`，PNG 改为 `--format png --write-mode stream`。

```powershell
mosaicraft build -i cpu-input -o fresh-output -d fresh-output/mosaicraft.db --threads 8
mosaicraft build -i empty-input -o private-library -d private-library/mosaicraft.db --append
mosaicraft mosaic -i test_input_photos/target5.png -d private-library/mosaicraft.db -o result.jpg --format jpg --write-mode batch --out-w 360 --out-h 640 --topn-random 1 --benchmark

ctest --test-dir build -C Release --output-on-failure
ctest --test-dir build_nocuda -C Release --output-on-failure
cmake --build build --config Release --target mosaicraft_webui_smoke
cmake --build build_nocuda --config Release --target mosaicraft_webui_smoke
```

CPU 特征批次最多 256 张、待处理像素最多 64 MiB；超预算图片独占批次。同目录输入的尺寸未知，采用单张批次以保持此边界。该预算没有覆盖整个匹配流程。本轮未改变评分公式、HSV 编号行为、PNG 编码策略或任务接口。

PNG 编码中位数仍约 5.1 s，是本次 PNG 样本的主要耗时；CUDA JPG 全库准备及重复建库颜色路径验证也值得继续测量。全流程内存分批、运行中取消与任务进度、PNG 编码优化按后续独立计划进行。
