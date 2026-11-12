# 运行诊断与 Web UI 验收

> 历史归档：本文保留当时的命令、版本与测量结果，不作为当前操作指南。当前用法请从[文档中心](../../README.md)进入。


2026-11-09。产品版本保持 v1.14.8，特征 v6、图库格式、评分算法与预算保持不变；API 增量升级为 2.1，主兼容版本仍为 2。实现前构建为 `7de7051`，本机为 Windows x64、MSVC 19.51、OpenCV 4.12.0、CUDA 13.5、RTX 4060 Laptop（sm_89）。

## 检查结果

| 检查 | 结果 |
| --- | --- |
| Windows CPU/CUDA 产品、两个测试程序与匹配基准目标构建 | 通过 |
| CPU Preset 完整 CTest | 5/5 通过；62 个核心用例、32 个回归用例 |
| CUDA Preset 完整 CTest，要求真实 GPU | 5/5 通过；94 个用例、110858 个断言 |
| CPU/CUDA Web UI HTTP smoke（PowerShell 7） | 通过 |
| Windows PowerShell 5.1 smoke | 通过；请求显式编码 UTF-8，Range 使用 AddRange |
| CLI、同步 API、异步任务的同一缺失输入 | 均为 `input_unreadable`；CLI 退出 1 且只打印一次终止错误，同步 HTTP 400，异步查询成功但任务失败 |
| 8 组跨版本对照 | 与实现前构建完全一致 |
| JavaScript 语法检查与真实 Edge 页面交互 | 通过；最终页面无 JavaScript error 日志 |

构建与测试沿用 README Preset 入口：

```powershell
cmake --build --preset windows-cpu --target mosaicraft MosaicraftWebUI mosaicraft_tests mosaicraft_regression_tests mosaicraft_match_benchmark --parallel 4
ctest --preset windows-cpu
pwsh -NoProfile -File tests/webui_smoke.ps1 -WebUiExe build/windows-cpu/Release/bin/MosaicraftWebUI.exe

cmake --build --preset windows-cuda --target mosaicraft MosaicraftWebUI mosaicraft_tests mosaicraft_regression_tests mosaicraft_match_benchmark --parallel 4
$env:MOSAICRAFT_REQUIRE_CUDA = '1'
ctest --preset windows-cuda
pwsh -NoProfile -File tests/webui_smoke.ps1 -WebUiExe build/windows-cuda/Release/bin/MosaicraftWebUI.exe
```

## 结果和边界

对照复用上一轮的 24 张合成素材、两份图库和数据库快照。模板 180×320、自定义 72×128，分别执行 CPU/CUDA、分析开启/关闭共 8 组。Top-N 为 1，关闭亮度随机调整，叠加 0.25；每轮恢复相同数据库快照。选图索引文本、主图像素与编码字节、报告、`images.use_count`、`usage_stats`、`target_runs` 全部一致。实现前的结构化解析器没有预览选项，对照显式关闭预览并移除测试目录中上一轮额外预览；预览开启行为由既有逐像素回归与真实 HTTP/browser 生成验证。

模板 CUDA 对梯度输入的分析浮点尾数存在既有 A/A 差异；本次继续使用每块恒色目标保证严格对照，不宣称消除了该既有差异。固定种子、不同批次/流水线、空候选、缺失特征、预算回退和 CUDA 故障复用原测试。批次对照还分别使用控制台诊断开启和关闭的运行上下文，选图和输出保持一致。

仅新增有界诊断/UTF-8 截断、会话与完成任务保留、实际产物验证的精简用例；在原报告提交边界和预览失败用例中补充断言，覆盖报告目录受阻、HTML 打开失败、主图关闭后预览失败、使用统计事务失败。确认原来的继续执行、提前返回和提交时点不变，主图登记不会被后续失败覆盖。缺失特征去重使用任务级位数组，候选评分未增加逐候选堆分配。

HTTP smoke 验证新元数据、服务会话/工作目录、请求恢复、阶段及产物、增量事件游标、实际 PNG 后缀、下载哈希、32 字节 Range/206、文件改写 409、删除 404 和清理完成任务。服务使用固定 64 KiB 文件读取缓冲，HTML 报告以附件发送；不会主动读取整个主图。

## 真实浏览器

在本机 Edge 使用实际启动的 CPU 服务交互，未用 HTML 字符串匹配代替操作：

- 必填输入错误阻止复制和提交；中文、空格、单引号、美元符号、反引号路径生成正确的 PowerShell 引用。浏览器生成的命令经 PowerShell 解析为字面量参数后实际运行 CLI，输出成功。
- 刷新恢复表单及同会话选中任务；断线保留最后快照，服务重启提示旧会话不可恢复；切换任务及功能页继续跟踪。
- 160000 个小块任务显示真实匹配计数和阶段运行时间；排队取消成功，运行时取消按钮禁用并说明原因。为稳定观察排队窗口，临时限制测试服务 CPU 亲和性，观察后已恢复。
- 建库后“使用此图库”填入生成与查询表单；生成预览默认关闭，分块模式清除并禁用预览。
- `.jpg` 请求实际生成 PNG，页面显示真实路径、尺寸与匹配统计，加载有界预览；CPU-only 服务的 CUDA 回退以警告显示且成功状态保留。
- 预览失败明确显示“主图已保存”及下载入口；主图改写后下载按钮显示文件失效原因。
- 复制诊断获得含会话、任务、事件及截断标记的有效 JSON；已点击导出按钮，但受控浏览器未返回下载完成事件，下载管理页面又受浏览器 URL 策略限制，因此未自动确认诊断 JSON 在系统下载目录的落盘。文件产物下载已独立通过真实 HTTP 哈希验证。

本机证据保存在忽略目录 `build/runtime-audit/`：`final-*-build.log`、`final-*-ctest.log`、`final-*-smoke.log`、`final-baseline.log`、`before-runtime-baseline.log`、`source-error-parity.json`、`browser-generated-command.txt`、`browser-diagnostic.json` 和 `webui-final.png`。基线数据位于 `build/refactor-audit/`，不提交合成素材和生成产物。

Linux/macOS 未在本机执行，继续由原生 CI/打包流程验收；没有推送、发布或升级产品版本。
