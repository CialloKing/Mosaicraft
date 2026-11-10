# 团队协作与可搬移图库

路径由用户参数和资源所在位置决定，不绑定开发者的仓库、盘符或 SDK 安装目录。团队各自使用一份完整图库；本轮未设计共享盘上的多人写入协议。

## 图库与自动升级

新库在 `meta` 中记录 `path_format=db-relative-v1`。`images.file_path/tiny_path/hist_path` 使用 UTF-8、`/` 分隔的数据库目录相对路径；读取时解析为本机路径。素材、特征需位于数据库目录树内，例如：

```text
my-library/
  mosaicraft.db
  000000.png
  000001.png
  features/
    ....tiny
    ....hist
```

新版首次正常打开旧库时自动升级，不需要维护命令或额外确认：先识别格式并检查路径能否可靠转换，再取得独占图库锁、重新检查标记、保存并验证 SQLite backup，最后在同一事务中更新三列路径和格式标记。其他进程等待并重读状态，超时报告图库忙；成功后恢复普通共享读取，后续打开不再备份。

备份位于数据库旁的 `.mosaicraft-backups/`，文件名包含原数据库名和唯一时间标识。SQLite backup 包含 WAL 中已提交的数据。升级保留 ID、匹配特征、使用计数与其他索引数据，不改写素材和特征文件。

旧相对路径仍按原启动工作目录解释，不能猜测已消失的旧盘符或按文件名寻找其他素材。跨盘、库外资源、转换冲突会保留旧格式并报告原因；缺失的库内文件不阻止路径转换，健康检查仍会报告缺失。备份或事务失败时本次打开失败，路径事务完整回滚。

未完成的颜色重排保持现有恢复流程：普通读取提示先运行原建库操作恢复，建库在恢复完成后再升级路径。不会在恢复期间改变旧路径语义。

分发前先在原有可用位置用新版正常打开旧库，完成升级，再关闭操作、复制整个图库目录。不要只复制数据库；也不要在重排或写入过程中复制。每个成员的使用统计独立，缓存可校验后复用或重建。

旧版程序不认识新路径约定。需要退回旧版时：

1. 关闭所有使用该库的进程，另存当前完整图库；恢复旧备份会回退备份之后新增的数据库记录和使用统计。
2. 将当前数据库及同名 `-wal`、`-shm` 文件一起另存，避免新旧 WAL 混用。
3. 从 `.mosaicraft-backups/` 复制已验证的升级前备份为原数据库文件，并恢复原目录布局。旧绝对路径仍需要原位置，旧相对路径仍需要原启动目录。
4. 保留升级备份和另存副本，再用对应版本检查。程序不会自动删除升级备份。

## 开发环境

先按 [README 构建步骤](../README.md) 安装锁定版本的依赖；每人通过环境变量设置 `VCPKG_ROOT` 和 CUDA 构建所需的 `CUDAToolkit_ROOT`。依赖版本锁保持不变，CPU 构建不需要 CUDA SDK。

需要个人配置时，将 [Preset 示例](../CMakeUserPresets.example.json) 复制为仓库根目录的 `CMakeUserPresets.json`，替换占位符；该文件已被 Git 忽略。共享源码、锁文件和脚本，各自重新配置本机构建目录，不复制其他人的 CMake 缓存。

CLI 的普通输入、输出和数据库参数继续按启动工作目录解析；例如 `-d my-library/mosaicraft.db`。库内部路径则始终按数据库所在位置解析。Web UI 继续显示服务工作目录。

## 基准与公开证据

基准脚本默认位置从脚本所在仓库推导，显式参数相对调用者工作目录。检查点保存相对引用；外部图库通过 `--database` 绑定，程序通过 `--match/--quality/--cli` 绑定。相同内容的程序换目录不会被误认为换版本，程序摘要变化仍拒绝续跑。旧实验清单需要显式资源位置导入新目录，不就地改写原始证据。

快照和运行数据库位于同一私有目录，保持相对资源路径有效。完整出图会准备独立名称的素材/特征硬链接或副本；仅匹配研究只需要特征。原始证据通过只读方式处理，不使用会自动升级的生产数据库入口。

使用 `--backends cpu|cuda|both` 选择后端，切换后端组合需使用新的输出目录；CUDA 单后端不会混入 CPU 阈值敏感性任务。缺少 `nvidia-smi` 时采样记录为不可用，不写成“显存为零”。图表使用 Pillow 自带字体，不要求 Windows 安装 Arial。

合成验证不依赖私有图库和旧实验目录：

```powershell
python scripts/selection_study.py `
  --match build/windows-cpu/Release/bin/mosaicraft_match_benchmark.exe `
  --quality build/windows-cpu/Release/bin/mosaicraft_quality_benchmark.exe `
  --cli build/windows-cpu/Release/bin/mosaicraft.exe `
  --phase synthetic --backends cpu --output-dir build/team-smoke
```

当前公开实验 JSON 已脱敏，资源名称是证据位置说明，不是本机运行配置。测量值、随机种子、输入与程序摘要保留；原始 JSON 和证据包在仓库外备份。公开 ZIP 重新打包但保留选图、诊断文件及其字节摘要。此次不再重写 Git 历史。

## 验证

核心回归包含首次打开、重复打开、备份故障、事务回滚、缺失文件、库外资源，以及搬库后从另一工作目录生成、追加、检查和导出。`portable_library` 另验证四个进程竞争时只升级一次，并验证备份包含 WAL 使用统计；`study_paths` 验证目录与程序重绑定、私有快照关闭句柄及公开数据不改变测量摘要。

这些检查注册在现有 Windows/Linux CPU CI 的 CTest 中。本次本机验证为 Windows；本机没有可用 Linux/容器环境，未执行远端 Linux CI，不将注册测试等同于远端已通过。

本机验收命令（构建目录可替换）：

```powershell
cmake --build build/quality-20261110 --config Release `
  --target mosaicraft mosaicraft_tests mosaicraft_regression_tests `
  mosaicraft_match_benchmark mosaicraft_quality_benchmark
ctest --test-dir build/quality-20261110 -C Release --output-on-failure
```

2026-11-10 验证结果见 [完整 JSON](performance/portable-paths-20261110.json)：

- CTest 7/7 通过，包含路径归一冲突、Windows 无盘符根路径拒绝和恢复场景。
- 当前 45,625 条记录已通过正常打开自动升级，非路径字段、特征 v6、素材 ID 和历史使用统计与升级前备份一致。六类目标、三个种子、CPU/CUDA 共 36 组选图与历史基线完全相同。
- 搬移特征库副本并从第三个目录启动，种子 42、六类目标、CPU/CUDA 的另外 12 组选图一致。两组都仅匹配特征，不拼接原图库照片。
- 完整图库副本的 136,875 个素材和特征文件共 2,023,244,857 字节；自动升级并搬移前后的逐文件内容聚合 SHA-256 一致。此项只读取不透明字节，不解码照片。
- 独立合成库完成 CPU 的 20 个策略出图及 4 个重跑，重跑选图和像素一致；另一安全合成库完成 72 个主实验、6 个重跑和 16 个性能预热/测量任务。实验目录与工具目录分别搬移后可续跑。缺少显存工具的异常分支通过模拟工具不可用验证，结果为 `null`。
- 17 份历史公开 JSON 的 113,901 个数值、布尔值与空值及 3,680 个内容摘要保持原值；两个脱敏 ZIP 均通过 CRC 与选图、诊断文件摘要检查。

逐次匹配复核使用现有基准入口，例如：

```powershell
& $match request.json --strategy L --seed 42 --match-only
Get-FileHash selection.indices.bin -Algorithm SHA256
```

`request.json` 的 `input/db/output` 绑定本机目标、数据库与选图输出前缀，`cpu` 选择后端；其余配置沿用原基线。运行前关闭写入并使用相同统计快照。完整 36+12 个选图摘要保存在验证 JSON 中，比较时分别保持平台、后端和种子一致。

脱敏后的本地附件为 `build/quality-study-20261110/quality-study-evidence.zip` 和 `build/selection-study-20261110/selection-study-evidence.zip`，大小与新 SHA-256 均记录于验证 JSON。旧提交、输入、二进制摘要仍代表当时实验，未改写为本轮工具摘要。
