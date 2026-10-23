# v1.14.5 性能与兼容说明

测量日期：2026-10-23。两项通用 CUDA 能力合并后的直接对比使用 Windows x64、RTX 4060 Laptop GPU、驱动 616.92。最终发布提交以包内 `BUILD_INFO.txt` 为准；测量实现为 `4959d58`，版本整理不修改计算代码。

## 更新范围

- 四种模板沿用原内核算术，复用锁页缓冲和设备特征，使 CUDA 传输与 CPU Canny 重叠。阶段记录见 [传输流水线验证](https://github.com/CialloKing/Mosaicraft/blob/v1.14.5/docs/CUDA_TRANSFER_PIPELINE.md)。
- 自定义尺寸默认尝试通用 CUDA 特征统计与 CPU 兼容 CUDA 评分。提取和评分独立准备、独立回退，`--cpu` 保持纯 CPU。统计、评分以及原 CPU 选图顺序均经过精确比较。
- 特征 v6、数据库和 HTTP API 合约 2.0 不变，现有 v6 图库无需重建。不承诺跨构建/OpenCV 版本一致。

## 直接对比方法

原版为动态 CUDA 功能加入前保留的 CUDA 构建，分支起点 `ea4cd98`；它是当时已有的二进制，本次未重新构建。日志确认自定义尺寸使用 CPU 提取和 CPU 评分。当前版日志确认同时运行通用 CUDA 提取和兼容 CUDA 评分。该基线已经包含模板传输优化，因此下表衡量两项自定义尺寸能力的合并收益，不衡量模板传输更新相对 v1.14.4 的收益。

每组两版各预热一次、交替测量三次，取进程墙钟中位数，包含启动、特征提取、选图、拼接和保存。复用 `scripts/benchmark_bounded.py`，每次恢复相同私有数据库快照，Top-N=1、关闭随机颜色调整、启用自适应权重和 25% 基础图叠加。

图库各 96 张；代表任务为 25,600 块、150 候选槽位、1280×1280 PNG，短任务为 1600 块、40 候选槽位、320×320 PNG。候选不足时保留无效槽位。输入、快照和程序摘要，以及 64 次运行的完整样本见 [原始数据](performance/v1.14.5.json)。

## 完整生成结果

| 特征尺寸 / 模式 | 原版 s | 当前 s | 耗时变化 |
| --- | ---: | ---: | ---: |
| 256×256 / 普通 | 4.0246 | 3.1592 | -21.50% |
| 256×256 / 分析 | 4.4845 | 3.6674 | -18.22% |
| 512×512 / 普通 | 10.6886 | 9.8347 | -7.99% |
| 512×512 / 分析 | 12.6162 | 12.6001 | -0.13%，基本持平 |

| 短任务特征尺寸 / 模式 | 原版 s | 当前 s | 耗时变化 |
| --- | ---: | ---: | ---: |
| 256×256 / 普通 | 0.3988 | 0.4530 | +13.60% |
| 256×256 / 分析 | 0.7567 | 0.8343 | +10.26% |
| 512×512 / 普通 | 1.1083 | 1.0602 | -4.34% |
| 512×512 / 分析 | 2.0566 | 2.0614 | +0.23%，基本持平 |

这些是相同输入直接交替测得的合并收益，不是两轮历史基准百分比的相加或相乘。全部 64 次运行的逐块选择、输出文件和解码像素均一致，原始私有数据库快照 SHA-256 未变化。样本来自 96 张测试图库，不能据此推算真实大图库的完整生成收益，也不承诺所有短任务提速。

## 分阶段数据与兼容边界

[独立评分验证](https://github.com/CialloKing/Mosaicraft/blob/v1.14.5/docs/INDEPENDENT_CUDA_SCORING.md) 中的评分阶段速度为 CPU 的 9.44～19.19 倍，计入打包、传输和结果复制，但常驻图库准备单列；45,625 张真实图库的准备约 0.79 s。不能将该倍数当作整条生成命令的加速倍数。

[动态特征验证](https://github.com/CialloKing/Mosaicraft/blob/v1.14.5/docs/DYNAMIC_CUDA_FEATURES.md) 保留 CPU OpenCV 预处理、TinyImage、Canny 和收尾；生成和建库的特征阶段收益分别测量。建库不执行候选评分，因此不享受评分加速收益。

实现验证包括 CUDA/CPU-only CTest、两套 Web UI/API smoke、真实 GPU 的精确分数和端到端回归，以及 Compute Sanitizer 0 内存错误。覆盖缺失特征、Top-N、亮度过滤、邻域处理、叠加、预算缩批、独立分配回退和运行期失败。最终发布另按 [发布清单](RELEASE_CHECKLIST.md) 执行同提交 CPU-only CI、正式包及公开附件验收。

> **EN**: Direct alternating measurements compare the saved pre-generic CUDA build (custom sizes use CPU extraction and scoring) with both compatible CUDA stages enabled. The 25,600-tile samples reduce total time by 18.2–21.5% at 256×256 and 8.0% for normal generation at 512×512; 512×512 analysis is unchanged. Short 256×256 tasks regress by 10.3–13.6%. All selections, output bytes and decoded pixels match. These results are specific to the 96-image fixtures and do not imply a universal or large-library speedup. Existing v6 libraries remain compatible.
