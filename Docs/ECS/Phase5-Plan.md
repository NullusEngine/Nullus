# Phase 5 实施与验收

本阶段复用既有 ECS 和 Base Jobs，逐项实现 Transform、持久化、Prefab 和 Baking。
完整 Editor、GPU 与 Player 启动验收仍分别跟踪，不通过接口占位宣布完成。

| 任务 | 当前状态 | 验证路径 |
| --- | --- | --- |
| 显式字段 codec、版本迁移、资源依赖 | 已实现并通过最终验证 | SerializationTests |
| 格式/类型/组件版本、局部引用、Archetype 表、完整性检查 | 已实现并通过最终验证 | SerializationTests |
| 原子文件保存、失败回滚 | 仓库临时目录的实际文件读写已通过 | SerializationTests |
| Prefab 覆盖、批量实例化、引用重映射、ECB InstantiatePrefab | 已实现并通过最终验证 | SerializationTests |
| 原生 LocalTransform/Parent/WorldTransform、dirty、循环与父引用清理 | Debug/Release/ASan/原生 DLL 均 10/10 通过 | TransformTests |
| 按层批量 JobSystem 计算、完成后统一提交 | 迁移后按 Validation.md 验证 | 宽层级、深层级、单线程用例 |
| Full/Incremental Baking、内存缓存、资源失效、失败保留与 Full 回退 | 原生 DLL 通过 | BakingTests |
| 显式 Preview 应用与 Play 隔离 | 原生 DLL 通过 | BakingTests |
| JSON 解析规模检查与优化 | 修复回调 DOM 的父数组重复扫描；四规模 36/36 测量成功 | ArchiveBenchmark、SAX 回归 |
| Scene 持久身份映射、事件/Undo、Full/Incremental/Live Baking UI | 身份 8/8、Editor 12/12；另有旧回归 80/80 通过 | AuthoringIdentityTests、EditorBakingTests |
| 异步 Scene/SubScene 自包含分块、替换/取消/卸载 | 已实现；验证见 Validation.md | StreamingTests、HybridTests |
| 原生 AssetMeta/ArtifactDB 注册、Editor 导入、共享 Cook 与资源失效 | 已实现；验证见 Validation.md | AssetPipelineTests |
| 构建依赖闭包、RuntimeAssetManifest/Npak、RuntimeWorld 资产加载 | 已实现；验证见 Validation.md | AssetPipelineTests、AssetPipeline 示例 |
| Player 自动 ECS 启动与 C++ 注册约定 | 已实现；验证见 Validation.md | PlayerStartupTests、Game 包自检 |
| 可见 Player/GPU、跨块实体引用、按实体时间预算提交 | 未完成 | 后续 Player/场景实施 |

Scene 身份现通过 PersistentManager 复用真实 ObjectGraph ID，新对象获得随机 GUID，
保存和加载保留实际对象、组件和文档身份。CaptureSceneBakingSources 自动生成输入，
InstanceID 只负责活对象解析。Hierarchy Undo 恢复原 ID，复制保留副本自己的新 ID。
Prefab Stage、Scene View 选择及通用 Authoring Inspector 仍需后续实施。

Codec 只读写定义好的字段。实体引用通过文档内字符串 ID 映射；资源依赖记录既有
GUID 和内容版本。加载创建全部目标实体后再解码引用，失败只回滚新实体。运行时
World/Index/Generation 和 C++ 对象布局不进入存档。完整性值用于内容损坏检测，
不作为真实性认证。

Baker 分两步：主线程 Capture 读取真实 GameObject 并声明资源依赖，Bake 只消费
不可变 JSON 输入。缓存比较完整规范输入、Baker 版本及资源版本；删除或不再匹配
的贡献从候选产物移除。产物先在独立真实 World 中解码、校验层级，再原子写入文件
并发布缓存。Preview 只应用成功产物，改变时替换拥有的实体；它不复用 Play World。
Editor 现订阅 Undo 事件并有界轮询直接字段变更，阻止未提交编辑/事务和 Play Capture；
面板与导出使用相同管线。持久磁盘缓存和细粒度预览实体补丁尚未实现，详见
[Editor Baking](Editor-Baking.md)。

原生资产/内容源扩展后的独立套件 122 项：87 Core/序列化/Jobs/分块、25 既有 TaskGraph、
10 Transform。Baking、Hybrid、资产与 Editor 导入另在真实引擎上运行；
不将重复 DLL 验证累计成新用例。新增链路与使用方式见 [Asset-Pipeline](Asset-Pipeline.md)。
所有当前结果、命令、构建条件和未验收范围以 Validation.md 为准。
