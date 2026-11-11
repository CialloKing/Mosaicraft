# 团队协作与可搬移图库

1.15.0 只接受图库结构版本 1。旧库必须在新目录重建，没有自动升级、迁移命令或格式修补；详见 [不兼容更新说明](BREAKING_1.15.0.md)。

## 创建与分发

每个团队成员使用一份完整图库。图库以数据库所在目录为根，素材、Tiny 和 LBP 路径使用 UTF-8、数据库目录相对路径及 / 分隔符。

~~~text
my-library/
  mosaicraft.db
  000000.png
  features/
    ....tiny
    ....lbp
~~~

关闭所有使用该库的程序后复制整个目录，不能只复制数据库，也不能在建库或重排期间复制。可以整体移动到中文、空格或其他盘符目录，并从任意工作目录启动新版程序。普通 CLI 输入、输出和数据库参数仍按调用者工作目录解析。

新版不会更改旧图库或删除已有升级备份。重建会重新分配素材 ID、计算特征，并从零开始使用统计。回退时恢复旧程序与对应旧库，保留原目录及旧备份，不修改版本标记冒充新版。

## 开发环境

通过 VCPKG_ROOT、CUDA 构建所需的 CUDAToolkit_ROOT 配置本机依赖。CPU 构建不需要 CUDA SDK，依赖锁文件保持原版本。

将 [个人 Preset 示例](../CMakeUserPresets.example.json) 复制为根目录 CMakeUserPresets.json 并填入本机位置；该文件被 Git 忽略。各自配置构建目录，不复制其他人的 CMake 缓存。版本由 contracts.json 统一生成。

## 基准工具

基准默认目录从脚本位置推导；显式参数按调用者工作目录解析。检查点使用 schema=1、checkpoint_paths=1 和 payload，内部资源采用相对引用，程序和图库通过本机参数重绑定。

检查点验证格式、必需字段、输入与二进制摘要、后端及配置。改变目录不改变内容身份；改变配置或程序内容需要新输出目录。旧检查点、旧库和公开脱敏报告不能作为新版运行清单。

私有快照复制 SQLite 索引，准备独立资源引用，缓存写入实验目录。原始历史证据只读保留。CPU/CUDA 可分别选择；无显存采样工具时记录 null；图表不依赖个人字体目录。

合成验证无需私有图库：

~~~powershell
python scripts/selection_study.py --match build/windows-cpu/Release/bin/mosaicraft_match_benchmark.exe --quality build/windows-cpu/Release/bin/mosaicraft_quality_benchmark.exe --cli build/windows-cpu/Release/bin/mosaicraft.exe --phase synthetic --backends cpu --output-dir build/team-smoke
~~~

发布流程通过 CTest 和 Web UI smoke 验收，覆盖旧库拒绝、WAL 数据保护、并发读取、搬移和脚本检查。 [1.15.0 验收数据](performance/reset-1.15.0.json)
