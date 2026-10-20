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
`--analysis-mode normal|analysis` 分别记录普通和分析模式，`--indices` 保存逐块选图。
保留小预算比较；`--skip-small` 用于五个场景的基线/默认实现性能验收。
大样本差异需固定特征回放定位，不调整评分容差或平分规则。

验收门槛：CUDA 大样本 `Features` 中位数至少降低 20%，各代表场景进程墙钟
回退不超过 5%，实际容量符合预算。未达门槛的优化不启用为默认实现。

异步传输使用锁页内存和任务专属非默认 stream；生命周期依据
[CUDA 异步执行文档](https://docs.nvidia.com/cuda/cuda-programming-guide/02-basics/asynchronous-execution.html)。
实测结果在完成回归后补充。
