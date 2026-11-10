# 输出格式与超限适配验收

验证日期：2026-11-10。修改前基线：`61f2418`。实现验收时产品版本为 1.14.10，本功能随 1.14.11 交付；API 合约为 2.2，主兼容版本保持 2，特征 v6、图库格式和评分参数不变。

## 输出规则

单图格式按显式 `--format` / API `format`、输出文件后缀、输入文件后缀、PNG 的优先级确定。输入后缀无法推导时回退 PNG，未知输出后缀在没有显式格式时失败。显式格式覆盖冲突后缀；兼容的别名和大小写保留原文件名。

自动推导的 JPG/WebP 超限时转为 PNG，保留尺寸；显式选择时通过统一比例缩小输出 Tile。整数取整可能改变比例；网格任一轴的块数本身超出编码上限时，显式模式失败。分块与 DeepZoom 不按整图上限适配。

输出规划在匹配前完成，不改变输入网格、特征尺寸或随机调用顺序。编码、报告和产物登记共用实际格式与尺寸；文件关闭后才登记实际结果。计划存在不能作为文件已经保存的依据。

## 本机验证结果

| 检查 | 结果 |
| --- | --- |
| Windows CPU Preset 构建及 CTest | 5/5 通过 |
| Windows CUDA Preset 构建及 CTest | 5/5 通过 |
| CPU / CUDA Web UI 与 API smoke | 两套均通过，覆盖同步生成、异步任务和真实文件 |
| GPU 必需预检 | RTX 4060 Laptop GPU，sm_89，驱动及运行时报告 13050，通过 |
| 模板和自定义尺寸八组跨版本对照 | 选图索引、编码文件、像素、报告及使用统计完全一致 |
| CLI 端到端检查 | 13 个场景通过，含默认命名、输入推导、冲突覆盖、别名、错误及特殊字符路径 |
| JavaScript 语法 | `node --check` 通过 |
| 真实浏览器交互 | 自动转 PNG、显式 JPG 缩小、参数恢复及重提、刷新恢复、后缀错误、帮助页和主图下载通过 |

八组基线分别使用 180×320 模板图库和 72×128 自定义图库，交叉 CPU/CUDA 与分析开关；每轮恢复同一图库快照，Top-N 固定为 1、亮度扰动关闭。额外回归用例使用固定种子 123 和 Top-N 3，对照完整 PNG 与缩小后的 JPG/WebP，验证各后端的选图及提交的使用统计相同。现有批次、流水线、缺失特征、输出模式及 CUDA 故障测试继续执行。

### 格式与尺寸边界

- 参数化单测覆盖 JPG 65,500/65,501、WebP 16,383/16,384，以及宽、高、双轴超限、细长图、整数取整、网格不可适配和 64 位尺寸。
- 真实窄条编码覆盖自动 JPG `66000×1 → PNG 66000×1`、显式 JPG `66000×1 → JPG 65500×1`、自动 WebP `16400×1 → PNG 16400×1`、显式 WebP `16400×1 → WebP 16382×1`。
- 自动换格式只发布一次 `output_format_changed`；显式缩小只发布一次 `output_scaled`。计划、保存后的尺寸、文件签名、预览和报告路径一致。
- API smoke 在独立临时工作目录验证省略输出路径：BMP 输入回退 `output/output.png`，JPG 输入推导为 `output/output.jpg`。恢复参数不会补充推导格式。
- 写入失败的回归用例保留输出计划，但没有保存后的格式、尺寸或产物；现有预览/报告/使用统计失败测试保留已保存主图及原提交边界。

### 浏览器检查

通过页面表单实际提交任务，确认自动模式生成的命令不包含 `--format`；运行中展示计划而不提供未保存文件的下载。完成后显示实际 PNG 路径和尺寸，下载文件 SHA-256 与登记产物一致。

显式选择 JPG 并提供 `.png` 请求后缀时，页面显示 `66000×1 → 65500×1`、Tile `33000×1 → 32750×1`，实际产物为 `.jpg`。刷新后任务及表单恢复；修改格式再恢复参数，仍保留原显式选择，重新提交后结果一致。帮助页可展开阅读新规则；不支持的自动输出后缀会标记输出路径字段。

本地忽略目录 `build/output-policy-audit` 保存修改前构建、图库快照、八组对照数据、CLI 结果、完整日志及 `browser-result.png`。这些是本机证据，不是源码构建的前置依赖。

## 复现入口与平台边界

```powershell
cmake --build --preset windows-cpu --target mosaicraft MosaicraftWebUI mosaicraft_tests mosaicraft_regression_tests
ctest --preset windows-cpu
cmake --build --preset windows-cpu --target mosaicraft_webui_smoke

cmake --build --preset windows-cuda --target mosaicraft MosaicraftWebUI mosaicraft_tests mosaicraft_regression_tests
ctest --preset windows-cuda
cmake --build --preset windows-cuda --target mosaicraft_webui_smoke
```

依赖准备和配置使用 README 的构建入口。编码集成检查采用窄条图，覆盖真实编码器路径，不生成巨型整图。Linux/macOS 的原生检查由现有 CI 或打包流程执行；本轮保留本地提交，未执行这些平台的原生验收，也不将 Windows 结果视作其通过证明。
