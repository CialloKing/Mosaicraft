# CLI、HTML 工程与回导验收记录

> 本文保留实施过程中的历史验收数据。当前工程已取消格式版本及旧总览/缩略图兼容逻辑，现行契约见 `CLI_PROJECTS.md`。

日期：2026-11-12。软件版本仍为 1.15.4，API 能力版本为 1.4、主版本为 1，工程格式为 1。图库、特征、ANN 及去重版本没有变化。本次未推送、打标签或发布 Release。

## 已实现范围

- 新 CLI 命令树及统一选项定义；移除旧 CLI 别名、交互补参和结束等待。
- 统一输出模式、后端策略、线程预算、种子、日志及 JSON 结果。
- 图库报告与检查共用统计口径；追加继承规格，纯归一化不建库，清理仅删除确认缺失的记录。
- 成功 create 自动记录最终排列使用情况，预览、回导、检查和打包不计数。
- HTML 便携工程保存唯一原素材副本、无损目标、最终排列、浏览缩略图、总览及离线只读查看器。
- 固定排列原生回导；不依赖原图库，不重新匹配，不执行 CUDA 预检。
- 工程信息、完整性检查、ZIP64 打包及对应异步任务、产物和历史。
- Web UI 新增 HTML 输出和工程页，合并图库报告与健康入口，支持工程任务恢复及历史筛选。
- 原有单图、分块、Deep Zoom、实时预览与手动替换继续保留。

## 本机验证

| 项目 | 结果 | 证据 |
| --- | --- | --- |
| Windows CPU Release 构建、CTest | 8/8，通过，45.90 秒 | `build/cli-project-build.log`、`build/cli-project-ctest.log` |
| Windows CUDA Release 构建、CTest | 设置 MOSAICRAFT_REQUIRE_CUDA=1，8/8，通过，50.79 秒 | `build/cli-project-cuda-build.log`、`build/cli-project-cuda-ctest.log` |
| 最终 Web UI 资源构建 | CPU/CUDA 均通过 | `build/cli-project-ui-build.log`、`build/cli-project-ui-cuda-build.log` |
| CPU/CUDA 安装目录 | cmake --install 成功，依赖 DLL 及 miniz/PicoSHA2 许可证、来源文件存在 | `build/cli-project-install-cpu.log`、`build/cli-project-install-cuda.log` |
| 脱离开发 PATH 的工程回导 | CPU/CUDA 均通过 | `build/installed-cpu-project.log`、`build/installed-cuda-project.log` |
| 安装版 HTTP/API smoke | CPU/CUDA 均通过 | `build/installed-webui-smoke.log`、`build/installed-cuda-webui-smoke.log` |
| Chromium/Firefox 离线浏览及 Web UI | 均通过 | `build/browser-project-final.log` |
| 严格 CUDA 新 CLI | 模板、通用尺寸建库和匹配均实际使用 CUDA；CPU 包显式请求 CUDA 返回退出码 4 | `build/final-acceptance-20261112/strict-cuda-results.json` |

GPU 为 NVIDIA GeForce RTX 4060 Laptop GPU，驱动 621.14。严格 CUDA 检查分别使用 180×320 模板和 18×32 通用尺寸、threads=1；记录的后端分别为 template_cuda、compatible_cuda，未发生回退。

受限 PATH 只保留 Windows 与 System32；使用绝对 Python/PowerShell 路径运行验证，没有向 PATH 添加开发依赖。验证对象为 `build/cli-project-install/cpu` 和 `build/cli-project-install/cuda` 安装目录，不代表已制作或发布正式平台归档。

所有建库、损坏和覆盖测试使用合成图片及可丢弃图库，未操作项目的真实图库和用户原图。build 下的日志、合成数据和截图是本地验收产物，不提交到 Git。

## 行为覆盖

`tests/portable_project.py` 在两个安装版上通过，覆盖：

- 每个新命令独立 --help/--help-all、旧别名拒绝、重定向无控制字符、单 JSON 结果、quiet 机器结果。
- 移动工程、使原图库不可用后继续检查和原生渲染。
- 同种子跨 1/4 线程、目标叠加与亮度扰动的 PNG 字节一致；回导前后图库使用记录不增加。
- 无分析 create 记录使用、已有有效图库拒绝 build、实际编码后缀不能覆盖目标图。
- HTML 非零亮度扰动拒绝，缺少派生缩略图仍可原生渲染，但完整性检查失败。
- 未知工程版本、非法索引、越界路径、摘要错误、截断素材及非整数维度明确失败。
- ZIP 解压后 deep check 和回导通过，回导结果与打包前一致。
- 单图分析报告及 HTML/Deep Zoom/分块目录覆盖；失败保留已有结果，有无关文件的目录拒绝覆盖。

原有 CTest 继续覆盖请求校验、统计/资源检查、归一化、线程预算、后端和输出回归。ZIP64 测试使用计数流验证 65,536 项以及超过 4 GiB 的偏移，不生成数 GiB 垃圾数据。该测试与小工程实际打包、解压互为补充，不等同于实写超过 4 GiB 的归档测试。

`tests/webui_smoke.ps1` 验证原 API、实时预览及手动替换后导出 HTML。包内第一个格子的文件 SHA-256 与被选原素材字节直接比较；工程检查、回导、打包及完整 archive 下载均通过。入口 HTML 单独下载返回 409。

`tests/browser_project.cjs` 在 Chromium 和 Firefox 上验证：

- file:// 离线打开，仅加载包内相对资源；无外部服务、CDN 或 JSON 请求。
- 缩放、适应窗口、目标参考、素材查看及只读边界。
- 合成百万格总览回退、放大后的可视窗口加载、64 MiB 缓存淘汰；未一次请求全部素材。
- Web UI 预览尺寸调整、追加/纯归一化命令生成、工程信息及实际回导提交。
- 工程回导历史筛选与参数恢复。

百万格用浏览快照合成，只用于查看器行为和请求数量验证，不宣称完成百万格完整匹配或原生输出性能验收。

## 小样本性能对照

使用旧 CLI 的 1.15.4 可执行文件作基线，与当前 CPU 安装版交错执行。合成图库 128 张、归一化 180×320，目标 144×256，默认 9×16 匹配格、180×320 输出格，256 格，输出 2880×5120 JPEG。random-top=1、无叠加和亮度扰动，每组预热 1 次、记录 5 次。

| 模式 | 基线端到端中位数 | 当前端到端中位数 | 基线引擎中位数 | 当前引擎中位数 |
| --- | ---: | ---: | ---: | ---: |
| 无分析报告 | 246.9 ms | 239.0 ms | 217.1 ms | 208.0 ms |
| 有分析报告 | 340.1 ms | 426.3 ms | 323.2 ms | 328.0 ms |

两个模式下基线与当前最终 JPEG 字节均一致。Windows 峰值工作集的运行中位数：无分析 68.07 → 66.88 MiB，有分析 68.22 → 67.01 MiB。

这是短时合成 smoke 对照，不能据此宣称大图库性能提升。分析报告模式端到端增加约 86 ms；引擎计时在原子发布之前结束，不包含全部目录验证、替换和旧产物清理成本。端到端数据保留该成本，没有用引擎时间代替完整耗时。后台采样内存/显存，主线程等待进程退出计时，避免把 200 ms 采样间隔算入短任务耗时。

原始命令、可执行文件和输入摘要、每次测量及日志位于 `build/final-acceptance-20261112/performance-recorded/`，控制台摘要在 `build/cli-project-performance.log`。

## 尚需平台验收

| 平台 | 当前状态 |
| --- | --- |
| Windows CPU | 本机构建、安装及运行通过 |
| Windows CUDA | 本机构建、安装、运行及 RTX 4060 实机验证通过 |
| Linux CPU | 未在本次环境构建、安装或运行 |
| Linux CUDA | 未在本次环境构建、安装或 GPU 运行 |
| macOS | 未在本次环境构建、安装或运行 |

本机没有可用的 Linux/macOS 运行环境，本次也不推送触发远端 CI。因此五个平台包全部构建及运行的发布门槛仍未满足；不能用 Windows 结果推断其他平台通过。Linux CUDA 还需要真实 GPU 验证，编译覆盖须单独记录。

## 使用和后续

新命令及工程契约见 [CLI 与工程说明](CLI_PROJECTS.md)，接口见 [API](API.md)。后续平台验收及正式发版遵循 [发布检查表](RELEASE_CHECKLIST.md)，不要求重建现有有效图库。

项目内 45,613 张真实图库与 target.png 的追加集成验证见 [HTML 真实数据验收](HTML_PROJECT_REAL_DATA_VERIFICATION.md)，包含实际工程、浏览截图、字节一致性及解压回导结果。
