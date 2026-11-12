# CI 与打包历史采样

> 历史归档：保留当时的版本、命令与样本，不能视为当前操作或性能结论。返回[历史档案](../README.md)。

## 性能采样

依赖日志及 `install-summary.json` 记录安装时间和二进制恢复数量，打包目录的 `release-timings.json` 记录配置、编译、测试、smoke、打包和解压验证时间。GitHub step summary 展示相同阶段数据。

使用 `CI` 的 `cache-namespace` 输入，针对同一提交选择一个全新命名空间后连续手动运行两次，可得到 Windows 冷、热缓存对照。冷日志应没有 Protobuf、DNN 等依赖及 Debug 构建，热日志应恢复依赖。下方 Linux 数据来自日常 CI 仍使用 vcpkg 的时期。

2026-11-08 对提交 `874503e` 使用独立缓存命名空间，各测一次冷、热缓存；[两次运行](https://github.com/CialloKing/Mosaicraft/actions/runs/42479756921)均通过。依赖列包含固定工具检出、引导、缓存及安装，不包含产品编译。

| 平台 | 冷缓存依赖步骤 | 热缓存依赖步骤 | 冷缓存 job | 热缓存 job | 热恢复数量 |
| --- | --- | --- | --- | --- | --- |
| Windows x64 | 596 秒 | 39 秒 | 12 分 11 秒 | 2 分 52 秒 | 11/11 |
| Linux x64 | 324 秒 | 17 秒 | 6 分 28 秒 | 1 分 15 秒 | 11/11 |

两个热缓存 job 均达到目标，未重新编译 OpenCV；冷日志确认没有 Protobuf、Abseil、FlatBuffers 或 Debug 依赖构建。热缓存中 vcpkg 安装本身分别为 8.42 秒和 3.27 秒。产品编译、CTest、smoke 的分项时间和实际工具链见[原始数据](../../performance/ci-packaging.json)。这是单次对照，不是长期耗时保证。

独立的热缓存 CPU 打包样本（提交 `babaae3`）中，Windows ZIP 打包 1.26 秒、解压验收 7.28 秒；Linux tar.gz 打包 3.50 秒、解压验收 7.58 秒。它与上述日常 CI 样本分开记录。

本机 CUDA Required 验收通过 88 个用例、110540 个断言和包内 Web UI/API smoke；隐藏 GPU 时 Required 明确失败。与已发布 v1.14.6 对照的私有固定输入涵盖 180×320、72×128 两种尺寸及 CPU/CUDA：按像素哈希对齐的特征、主图与预览像素、选图使用计数和评分摘要均一致。生成对照使用同一图库快照，Top-N 固定为 1，关闭亮度调整，叠加强度 0.25。

本轮保持 CLI、特征 v6、API 合约 2.0 和图库格式不变；不发布或修改 v1.14.6。

多架构配置、CPU 回退和实测记录见 [CUDA 多架构支持](../../development/cuda.md)。
