# CUDA 传输流水线验证

本轮只优化生成阶段的传输、特征复用和 CPU Canny 重叠。版本、特征 v6、
评分公式、ANN 和顺序选图规则保持不变。基线为 v1.14.4（`3d163db`）。

## 计时与预算口径

- `Features` 包括 CPU 缩放、GPU 特征提取和 CPU Canny，保持原来的阶段墙钟口径。
- `Matching wall` 是匹配阶段墙钟，预取重叠的各阶段时间不能相加当作总耗时。
- 原 `GPU upload/compute/download` 是同步路径的主机计时；异步路径单独输出
  `CUDA event upload/compute/download`，不会把提交耗时冒充设备计算时间。
- `Feature CPU Canny` 是灰度转换和 Canny 线程池任务的墙钟，`GPU wait` 是调用线程等待
  异步完成的时间。异步设备时间与 Canny 可能重叠。
- 传输字节数按有效元素累计；锁页、驻留及临时缓冲按实际分配容量记录。
  主机匹配预算包括锁页输入和下载、评分暂存；256 MiB 评分预算包括目标驻留特征。
  图库显存及原输入像素工作区独立统计，评分预算不是整个进程的显存上限。

## 可复现的比较

保存基线 CUDA、CPU-only 程序和 SHA-256。数据库使用私有备份，独立特征文件复制到
私有目录；每次运行从同一快照恢复使用计数。用 `mosaicraft_features_benchmark`
的 `--build-cache` 参数构建并校验私有缓存，不复用未经校验的旧包。

复用 `scripts/benchmark_bounded.py`：每版预热一次，再轮换正式运行三次。
`--analysis-mode normal|analysis` 分别记录普通和分析模式，`--indices` 保存每次运行的逐块选图，
`--pixels` 在计时结束后计算解码像素 SHA-256。
保留小预算比较；`--skip-small` 用于五个场景的基线/默认实现性能验收。
大样本差异需固定特征回放定位，不调整评分容差或平分规则。

验收门槛：CUDA 大样本 `Features` 中位数至少降低 20%，各代表场景进程墙钟
回退不超过 5%，实际容量符合预算。未达门槛的优化不启用为默认实现。

异步传输使用锁页内存和任务专属非默认 stream；生命周期依据
[CUDA 异步执行文档](https://docs.nvidia.com/cuda/cuda-programming-guide/02-basics/asynchronous-execution.html)。

## 实现与兼容性

- 缩放直接写入可复用锁页输入，替代原来的 RGB vector；GPU 上传、原特征 kernel 和结果下载
  进入任务的非默认 stream，CPU 线程池同时执行原灰度转换及 Canny（60/120）。
- 每次最多提取 256 张，外层最多 1024 张。Grid/TinyImage/LBP 保留到本批评分结束，
  原评分 kernel 直接引用这些数组；LAB、Canny、候选及权重按原值传入。
- 主机总是接收 LAB/Grid；普通模式不下载 TinyImage/LBP，自适应模式增加 LBP，分析模式下载全部。
  CPU 完整特征路径保持不变。完成事件之后才读取下载暂存或复用锁页输入。
- 先评分当前批，再预取下一批特征和 ANN，与当前批 CPU 顺序选图重叠。只有一套 GPU
  特征工作区，两套 CPU 批次缓冲；设备视图用弱引用、批次代号和范围检查防止过期访问。
- 所有可选资源在首批前准备。分配不足会释放部分资源并使用同步 CUDA；复制、kernel、event
  或 CPU 任务运行期错误向上传播，退出时收束本任务的工作，不中途重算或产生空成功结果。

现有 doctest 已覆盖四种尺寸，256/257/1024/7 张边界、工作区复用、预算收紧、锁页/显存
分配失败、上传后及等待前退出、Canny 任务异常、视图失效，以及固定张量的新旧评分逐位相等。
同步/异步生成另比较固定种子 Top-N、自适应权重、分析、缺失特征、镜像补边、基础图叠加，
检查逐块选择、编码字节和解码像素。未改变 LAB/Grid 原有浮点原子累加限制或选图平分规则。

2026-10-21 本机 CUDA 和 CPU-only Release 各通过 54 项单元测试、21 项回归测试；
两套 Web UI/API smoke 均通过。最终性能结果见下文。

## 实测结果

2026-10-21，Windows 10 19045 x64、Ryzen 9 7940H（8 核 16 线程）、RTX 4060 Laptop
（8188 MiB）、驱动 616.92、CUDA 13.3.33、Visual Studio 18 2026 Release、OpenCV 4.12。
本机固定样本满足性能门槛，流水线作为默认 CUDA 生成路径启用；分配不足时保留同步回退。
这些数字只代表本机、图库和输入，不是所有 GPU 的收益保证。

图库快照包含 45,625 条 v6 记录，特征尺寸 180×320。常规场景为 1,600 块，大样本为
160,000 块；输入小块均为 9×16，输出均为 7200×12800。JPG 使用 batch，PNG 使用 stream。
固定 Top-N=1，关闭颜色调整，普通和分析模式独立采样。每个场景、模式、版本预热一次，
再交替正式运行三次，共 80 次；下表为正式运行进程墙钟中位数。没有剔除慢样本。

| 场景 | 普通基线 / 新版（秒） | 变化 | 分析基线 / 新版（秒） | 变化 |
|---|---:|---:|---:|---:|
| CPU JPG，1,600 块 | 3.344 / 3.384 | +1.20% | 4.382 / 4.375 | -0.16% |
| CUDA JPG，1,600 块 | 3.317 / 3.237 | -2.43% | 4.672 / 4.629 | -0.92% |
| CUDA PNG，1,600 块 | 8.235 / 7.975 | -3.16% | 9.277 / 9.246 | -0.33% |
| CPU PNG，160,000 块 | 53.814 / 54.754 | +1.75% | 61.070 / 60.568 | -0.82% |
| CUDA PNG，160,000 块 | 30.969 / 29.103 | -6.02% | 49.657 / 46.290 | -6.78% |

| CUDA 大样本阶段 | 普通基线 / 新版（毫秒） | 分析基线 / 新版（毫秒） |
|---|---:|---:|
| Features | 13091.1 / 7310.0（-44.16%） | 14342.1 / 7976.3（-44.39%） |
| Matching wall | 21747.8 / 19739.6 | 31255.3 / 28335.8 |
| GPU scoring（主机阶段墙钟） | 1357.0 / 1135.4 | 1906.3 / 1426.4 |

所有十组场景的总耗时回退均小于 5%，大样本 Features 降幅超过 20%。当前普通模式
Canny 墙钟中位数 3477.6 ms、GPU 等待 1734.8 ms；设备 event 上传/计算/下载分别为
2203.1 / 3006.4 / 88.6 ms。这些时间互有重叠，不能求和代替 Features。
分析模式对应 Canny 3488.4 ms、等待 2375.0 ms、event 2224.9 / 3580.4 / 131.0 ms。

## 容量与传输核验

| CUDA 大样本容量 | 普通模式 | 分析模式 |
|---|---:|---:|
| 原版匹配缓冲峰值 | 98,160,328 B | 98,602,696 B |
| 新版默认匹配缓冲峰值（含两套 CPU 数组和全部锁页暂存） | 56,043,208 B | 56,813,256 B |
| 特征锁页输入及下载暂存 | 44,439,552 B | 44,767,232 B |
| 评分锁页暂存 | 1,916,928 B | 1,916,928 B |
| 64 MiB 配置匹配缓冲峰值 | 47,343,304 B | 47,781,576 B |
| 目标驻留特征显存 | 2,097,152 B | 2,097,152 B |
| 评分显存及目标驻留特征合计 | 4,014,080 B | 4,014,080 B |
| 原输入像素显存（单独统计） | 44,236,800 B | 44,236,800 B |
| 图库显存（单独统计） | 95,173,750 B | 95,173,750 B |

默认外层批次为 1024，小预算为 256；两者都保留 256 张提取粒度。共享锁页输入后，
本样本在 64 MiB 下也能容纳跨批预取。小预算普通和分析模式各预热一次、正式运行一次，
用于核验容量及分批兼容，不作为性能门槛的三次采样数据。

普通模式特征下载由 332,800,000 B 降至 126,720,000 B（-61.92%），评分上传由
435,200,000 B 降至 107,520,000 B（-75.29%）。分析模式下载完整目标特征，特征下载为
331,520,000 B；不再下载最终会被 CPU Canny 覆盖的 GPU edge 值。图像上传仍为
27,648,000,000 B，评分下载仍为 192,000,000 B。原版字节数依据同步入口的有效元素数，
并以第一阶段带统计的基线采样核对；性能表始终使用未修改的 `3d163db` 二进制。

匹配预算不是进程 RSS 上限：目标图、图库记录、ANN、CPU 特征缓存、Canny/OpenCV 工作区、
输出缓存和编码另占内存。普通 CUDA 大样本正式运行最大 RSS 为基线 415.61 MiB、新版
378.8 MiB；分析模式为 443.07 / 407.5 MiB。RSS 使用 Windows PeakWorkingSetSize，
每 25 ms 查询一次，不含显存。256 MiB 评分预算也不是总显存上限。

## 正确性及原有 CUDA 波动

常规 CPU/CUDA 场景和 CPU 大样本在两种模式下，逐块索引、输出 SHA-256、解码像素
SHA-256 均完全一致。测试还覆盖固定种子 Top-N=3、自适应权重、分析、缺失特征、镜像补边
及 0.35 基础图叠加。64 MiB 和默认预算的 CUDA 大样本只出现下面已定位的两类结果。

**CUDA 大样本不能宣称跨次运行逐字节一致。** 原版和新版二进制各自在重复运行中产生
同样的两组索引、输出与像素摘要，差异仅为 `42823、42824、42840、43624、43625` 五块。
两类输出 SHA-256 为：

- `99e0d65bbb733126dbdbf0b5c2a6002e4347ca402fd461b6348807b3a5b5b7ec`
- `74f930f92d280dd4a48c597c625ee94aee48c93667d17f098e6445f7ecb0cb6a`

使用 `mosaicraft_cuda_replay` 重放首个分歧块 42823：固定输入像素和之前的选择历史，
同步/流水线入口交替提取 64 次，Canny/TinyImage/LBP 始终完全一致；LAB 的 L 值在
`35.107219696044922` 和 `35.107223510742188` 间变化。沿用原评分和邻域规则，
候选 12 / 40 的得分约 `540.2077028`，会发生胜者翻转。

在每次流水线提取后，分别用同步上传入口和驻留入口对**同一份**特征评分，累计 4,800 个
binary64 评分用 `memcmp` 比较，全部逐位一致。现有测试另用同一份张量验证 1/256/1024
评分分批、256/257/1024/7 张特征批次和正偏移视图。实际内核、运算顺序及 `<` 平分规则
没有修改，也没有扩大 LAB/Grid 既有误差界限。证据将大样本差异定位为原有浮点原子累加
波动及其后续邻域影响；需要确定性输出时，应另行评估归约实现，不属于本轮范围。

## 复测与交付记录

[实测 JSON](performance/cuda-transfer.json) 保存环境、基线/输入/程序摘要、80 次性能运行
摘要、8 次预算核验和固定特征回放数据。私有图片、数据库、完整日志和基线程序留在
`build/cuda-transfer-validation/`；原图库数据库 SHA-256 已核对未变化。

```powershell
cmake --build build --config Release --target mosaicraft_match_benchmark mosaicraft_cuda_replay
cmake --build build_nocuda --config Release --target mosaicraft_match_benchmark
python scripts/benchmark_bounded.py --exe build/Release/bin/mosaicraft_match_benchmark.exe --baseline-exe build/cuda-transfer-validation/baseline-cuda/mosaicraft_match_benchmark.exe --snapshot build/cuda-transfer-validation/library/snapshot.db --request build/cuda-transfer-validation/cuda_large.json --output-dir build/cuda-transfer-recheck --analysis-mode normal --runs 3 --skip-small --indices --pixels
build/Release/bin/mosaicraft_cuda_replay.exe test_input_photos/target5.png build/cuda-transfer-validation/library/snapshot.db build/cuda-transfer-validation/acceptance/cuda_large-normal/baseline-0.indices.bin 42823
```

分析模式将 `--analysis-mode` 改为 `analysis`；预算核验去掉 `--baseline-exe` 和
`--skip-small`，使用新输出目录。其余四场景使用对应请求和 CPU/CUDA 程序。
JSON 中的摘要用于确认输入身份；私有图库不随仓库分发。

分支 `codex/cuda-transfer-pipeline`，前三步提交为 `de43a1b`（基线统计）、`b2e3d23`
（锁页及 Canny 重叠）、`78aceb0`（设备特征复用）；本报告与回放工具纳入第四步兼容及性能
验收提交。软件仍为 v1.14.4，特征仍为 v6，数据库与 HTTP API 不变，本轮没有发布版本。

## 复审后修复：评分显存余量

2026-10-21 修复评分缓冲已分配后再次折半空闲显存的问题。准备阶段允许缓冲使用分配前
空闲显存的一半；评分前查询的空闲量已经扣除了该缓冲，原检查可能拒绝刚准备成功的工作区。
现在按同一口径检查：缓冲容量不超过当前空闲量，等价于不超过“当前空闲量加已分配容量”
的一半，也避免了容量相加溢出。256 MiB 总评分预算和目标驻留特征扣减仍单独检查。

复用内部故障注入，在真实 CUDA 评分中模拟“剩余空闲量等于缓冲容量”和“少 1 字节”，
不通过占满设备制造压力。新增回归先在旧条件下失败，修复后成功边界与同步评分逐位一致，
不足边界仍返回失败。覆盖 256/257/1024/7 张批次。CUDA、CPU-only Release 各 54 项单元、
21 项回归及两套 Web UI/API smoke 均重新通过；日志保存在 `build/cuda-transfer-validation/low-memory-*`。

上文 80 次性能及 8 次预算采样对应 `2044de4`，本次预算检查修复没有重新运行这组性能采样。
