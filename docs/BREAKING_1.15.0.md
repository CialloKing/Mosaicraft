# Mosaicraft 1.15.0 不兼容更新

本版保留选图数学定义、默认参数及 CPU/CUDA 能力，清理旧图库和旧 API 的兼容逻辑。软件继续使用 1.x；内部版本统一从 1 开始，以后分别递增。

## 版本与图库

版本唯一来源是根目录 `contracts.json`，程序、页面、Python 工具和发布清单共用。没有额外的契约族、世代、epoch、版本 URL 前缀或版本请求头。

图库结构使用 SQLite `user_version=1`，特征版本为 1。新库一次建立完整的素材、元数据、颜色排序和使用统计表；读取不会补表、补列或升级。

**所有旧库都需要在新目录重新建库，包括先前已经改为相对路径的图库。** 新版拒绝不受支持的版本、缺失的表结构、混合特征版本、错误的路径与特征布局。`--yes`、`--force` 和追加不能绕过检查。

```powershell
mosaicraft build -i photos -o library-new -d library-new/mosaicraft.db --yes
mosaicraft db-health -d library-new/mosaicraft.db
mosaicraft mosaic -i target.png -d library-new/mosaicraft.db -o result.png
```

旧图库、原素材和已有 `.mosaicraft-backups` 不会被自动删除或改写；新版不再创建升级备份，也没有迁移命令。新库重新分配 ID、提取特征并从零统计使用次数。回退时使用能读取原库的旧程序打开原库，不能用新版替换旧库版本标记。

## 特征、缓存与恢复

- 新特征 v1 与原 v6 使用相同的 8×8 LAB Grid、16×16 TinyImage、256 桶 LBP 及统计定义。
- SQLite 列和内部成员统一为 `grid_lab/gridLab`、`lbp_path/lbpPath`，不再保留旧名称。
- Grid 使用 192 个小端 IEEE float32；Tiny/LBP 源文件具有 16 字节头及固定载荷。
- 文件头：8 字节文件类型、2 字节版本、2 字节 Tiny/LBP 类型、4 字节元素数，数值均为小端；元素数为 256，Tiny 总长 272 字节，LBP 总长 1040 字节。
- 缺失、截断、类型或版本不符、非有限浮点特征不能用于匹配。健康检查可以列出资源问题，不会自动提取或修复。
- ANN 和特征包版本为 1，额外验证图库版本、特征版本、维度、数量及摘要。无效派生缓存可以重建，不改变源特征和图库版本。
- 颜色排序缓存和重排清单版本为 1。恢复清单只保存数据库目录相对路径并核验记录及文件，不读取旧启动目录语义；不支持的清单保留并报错。

## API 1.0

路由和现有业务方法仍为 `/api/*`。`/api/info` 报告软件版本、API 1.0 及内部版本摘要；客户端按 API 主版本和必需端点发现能力，不识别历史世代。

`/api/run` 及 `MOSAICRAFT_ENABLE_LEGACY_RUN` 已删除，环境变量不能重新启用命令执行。

| 删除的请求形式 | 当前形式 |
|---|---|
| `input/db/output` 等别名 | 对应端点的 `inputPath/dbPath/outputPath` 或目录字段 |
| `topNrandom` | `topNRandom` |
| `cpu` | `useGpu` |
| `outputTile` | `nativeTileW/nativeTileH` |
| `normalizeSize` | `normalizeWidth/normalizeHeight` |
| `jpegQuality/outputFormat` | `quality/format` |
| 查询 `db/unused/output` | `dbPath/showUnused/outputDir` |

未知字段、重复 JSON 键、重复标量查询参数、同一参数重复出现在查询与正文中、错误类型和不支持的 `null` 均报错。整数不接受浮点 JSON 类型；查询布尔值只接受 `true/false`。

错误统一包含 `ok:false`、`exitCode`、`message` 和 `diagnostic`，HTTP 边界错误也一样。任务查询成功与任务执行失败仍是两个状态。CLI 保留短参数和 `--output-tile/--normalize-size/--cpu` 等便捷参数。

## 工具与交付

实验检查点使用 `schema=1`、`checkpoint_paths=1` 和 `payload`，验证输入、二进制、后端和配置。旧检查点拒绝续跑；相同内容的二进制换目录可以重绑定。C++ 即时请求和结果 JSON 不属于检查点。

公开历史报告、测量数据和证据包保持原样，不重新编号，也不导入为新版检查点。UI 沿用固定存储键和状态版本 1，忽略不符合当前字段结构的数据，不迁移历史状态。

发布包携带 `contracts.json` 和构建信息中的内部版本摘要。仓库 HTML 是构建模板，离线使用构建或发布目录中的 `index.html`。

验证使用合成图库，不重新处理当前私有图库。验证结果和未执行平台见 [验收数据](performance/reset-1.15.0.json)。
