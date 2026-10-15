# 引用、工件与缓存

资产系统要回答三个问题：谁引用了谁、运行时读哪份数据、什么时候可以重建。本页说明这三件事在 Nullus 里的落点。

## 引用

引擎有两类引用，形状相同但含义不同：

| 类型 | 条件 | 用途 |
| --- | --- | --- |
| 场景内引用 | 非零 `fileID`、空 `guid`、`type = NonAssetType` | 指向同一文档内的对象 |
| 资产引用 | 非零 `fileID`、有效资产 `guid`、资产类型 | 指向磁盘资产及其子资源 |

`filePath` 只是提示字段，解析以 GUID 与子资源键为准。这样可以安全地重命名或移动文件。

生命周期归属用单独的 `$owned` 关系表达：场景拥有 GameObject、GameObject 拥有组件。不要用资产引用表达归属关系。

## 工件清单

每个源资产导入后生成一份清单：

| 字段 | 说明 |
| --- | --- |
| `sourceAssetId` | 源资产 ID |
| `importerId` / `importerVersion` | 执行导入的导入器与版本 |
| `targetPlatform` | 工件面向的平台 |
| `primarySubAssetKey` | 主工件键 |
| `subAssets` | 每个工件的类型、加载器、路径、内容哈希与显示名 |
| `dependencies` | 依赖记录列表 |

工件类型覆盖 `Model`、`Mesh`、`Material`、`Texture`、`Skeleton`、`Skin`、`AnimationClip`、`MorphTarget`、`Prefab`、`Scene`、`Shader`、`Audio`、`UIDocument`、`UIStyleSheet`、`Font` 与 `LocalizationTable`。

## 依赖种类

依赖记录用种类加值的形式表达，涵盖：

- 源文件哈希、源资产 GUID、路径到 GUID 映射；
- 导入器版本、后处理器版本、构建目标；
- 已导入工件、原始包文件；
- Prefab 基底、嵌套 Prefab、Prefab Override 目标与 Prefab 校验；
- 运行时组件能力。

前几类决定“什么时候必须重新导入”，最后一类决定“这份工件在目标平台上是否可用”。

## 重新导入的触发条件

工件在以下情况失效：

- 源文件内容哈希变化；
- 导入器或后处理器版本变化；
- 导入设置变化；
- 构建目标变化；
- 依赖的资产 GUID 或 Prefab 基底变化；
- 运行时组件能力不再满足。

编辑器只重新导入受影响的部分；依赖链通过清单回表，而不是扫描整个工程。

## 运行时加载

运行时不解析源文件，只加载工件。加载路径经过授权检查：只有被登记为运行时授权的工件路径可以被游戏进程读取，未授权路径会被拒绝并记录。

加载过程带遥测，可以统计每次加载的类型、耗时与命中情况，用于定位启动瓶颈。

## 缓存

可重建的中间数据都放在工程的 `Library/` 目录下：

| 内容 | 说明 |
| --- | --- |
| 工件与索引 | 导入结果与资产数据库 |
| 缩略图 | 编辑器预览缓存 |
| IDE 工作区 | 调试器与编辑器生成的工程文件 |
| `Library/ScriptApi/ScriptApi.json` | 合并后的脚本 API 清单 |

`Library/` 可以整体删除并重新生成，因此不纳入版本控制。需要长期保留的只有 `Assets/`、`ProjectSettings/` 与项目文件。

## 下一步

- [反射与序列化](../architecture/reflection-serialization.md)：对象图与引用格式的完整规则。
- [Game 运行时与资产清单](../platform-build/runtime.md)：运行时如何消费这些工件。
