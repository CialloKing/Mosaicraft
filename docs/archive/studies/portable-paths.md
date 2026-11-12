# 图库搬移与研究环境历史约定

> 历史归档：保留当时的版本、命令与样本，不能视为当前操作或性能结论。返回[历史档案](../README.md)。

## 开发环境

通过 VCPKG_ROOT、CUDA 构建所需的 CUDAToolkit_ROOT 配置本机依赖。CPU 构建不需要 CUDA SDK，依赖锁文件保持原版本。

将 [个人 Preset 示例](../../../CMakeUserPresets.example.json) 复制为根目录 CMakeUserPresets.json 并填入本机位置；该文件被 Git 忽略。各自配置构建目录，不复制其他人的 CMake 缓存。版本由 contracts.json 统一生成。

## 基准工具

基准默认目录从脚本位置推导；显式参数按调用者工作目录解析。检查点使用 schema=1、checkpoint_paths=1 和 payload，内部资源采用相对引用，程序和图库通过本机参数重绑定。

检查点验证格式、必需字段、输入与二进制摘要、后端及配置。改变目录不改变内容身份；改变配置或程序内容需要新输出目录。旧检查点、旧库和公开脱敏报告不能作为新版运行清单。

私有快照复制 SQLite 索引，准备独立资源引用，缓存写入实验目录。原始历史证据只读保留。CPU/CUDA 可分别选择；无显存采样工具时记录 null；图表不依赖个人字体目录。

合成验证无需私有图库：

~~~powershell
python scripts/selection_study.py --match build/windows-cpu/Release/bin/mosaicraft_match_benchmark.exe --quality build/windows-cpu/Release/bin/mosaicraft_quality_benchmark.exe --cli build/windows-cpu/Release/bin/mosaicraft.exe --phase synthetic --backends cpu --output-dir build/team-smoke
~~~

发布流程通过 CTest 和 Web UI smoke 验收，覆盖旧库拒绝、WAL 数据保护、并发读取、搬移和脚本检查。 [1.15.0 验收数据](../../performance/reset-1.15.0.json)
