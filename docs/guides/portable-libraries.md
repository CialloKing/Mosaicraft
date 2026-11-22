# 团队协作与可搬移图库

从 **1.15.12** 起，`mosaicraft.db` 持久化全部图像特征，不再有 `features/`。尽管图库版本号仍为 1，新版不读取 1.15.11 及更早的文件式布局。

## 从旧图库重建

关闭使用旧库的程序，保留旧库及原始素材。用原始素材建到**新目录**，然后检查：

```sh
mosaicraft library build original-photos -o library-blob --size 180x320
mosaicraft library report library-blob --check
```

如只剩旧库内的归一化素材，可把旧库目录作为普通图片输入，**不要**加 `--recursive`，输出仍指向新目录。重建会重新分配素材 ID、计算特征，并从零开始使用统计；不迁移旧数据库。需要回退时，使用旧程序和原目录，不更改版本标记。

## 创建与分发

图库以数据库所在目录为根，素材路径为 UTF-8 相对路径。可选的 ANN 位于 `index/`，不必随库分发。

```text
library-blob/
  mosaicraft.db
  000000.png
  index/
    lib.ann196
    lib.ann196.meta
```

关闭所有使用该库的程序后复制整个目录，不能只复制数据库，也不要在建库、重排或恢复期间搬库。可以搬到中文、空格或其他盘符目录。普通 CLI 输入、输出和数据库参数仍按调用者工作目录解析。

若提示追加或重排未完成，保留恢复日志和暂存目录，在原位置运行 `mosaicraft library add <已有空目录> -l library-blob`。该命令先恢复，再扫描输入；若报告冲突，不要手动清理恢复记录。

历史测量与旧环境说明见[图库搬移与研究环境历史约定](../archive/studies/portable-paths.md)。
