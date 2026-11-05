# ECS 剩余任务与验收清单

本文件保持完整任务的范围，不能把已通过的 Core 测试当作整个 ECS 交付完成。

## 已有可执行代码

- Entity 的 World/Index/Generation、耗尽退役、批量生命周期和场景归属。
- 稳定注册、Data/Tag/Buffer/Shared/Enableable、Resource、非平凡值生命周期。
- 对齐 SoA Chunk、规范化 Archetype、迁移/Swap-Back、空 Chunk 复用和回收。
- 缓存 Query、高级过滤、Chunk 访问、写版本、借用/结构安全检查。
- 有序多 writer ECB、临时命令目标、部分提交诊断。
- System 生命周期、阶段、显式依赖、读写冲突、稳定拓扑与环检测。
- 真实 JobSystem 的系统/Chunk/层级并行、批次粒度、单线程模式、协作取消和 join。
- BackgroundJobQueue 异步 Scene 分块解析、owner 提交、替换/取消/卸载和 RuntimeWorld 接入。
- 可选 RuntimeWorld、单向 GameObject Transform 桥接、PlaySession 阶段接入代码。
- Debug/Release/ASan 各 122/122 独立测试（87 Core/序列化/Jobs/分块 + 25 TaskGraph + 10 Transform）；
  最新原生 DLL 167/167（含 8 身份、12 Editor Baking、13 资产、3 Editor 导入、
  11 Player 启动）通过，共 192 个不同 ECS/TaskGraph 用例；另有 80/80 旧序列化、
  PPtr 与 Undo 回归通过。重复计数、构建限制和未验收范围详见 [Validation.md](Validation.md)。
- Basics/MassSimulation/真实 Lua Hybrid、Authoring→Baking 文件→运行时加载和异步分块示例。
- ECS Scene/Prefab 原生资产注册、Editor 导入、Cook 依赖与版本失效、运行时资源解析，
  以及既有构建闭包、RuntimeAssetManifest、目录/Npak 数据源和打包加载示例。
- 版本化字段存档、Scene 归属、Prefab 覆盖/引用映射与 ECB 延迟实例化。
- 原生 Transform 层级、按层 Job、Full/Incremental Baking 和独立 CPU Preview。
- Scene 真实 ObjectGraph 身份保存/加载、Prefab 实例区分、Hierarchy Undo 恢复，
  Editor 的 Full/Incremental/Live Baking、CPU 实体/组件检查和原生资产导出。

## Phase 4 尚需完整验收

CPU 级混合接入已通过当前引擎原生构建与 15 项 Hybrid 测试，见 Validation.md。
完整 Editor 的 Play/Pause/Step/Stop、Scene 切换、旧 Scene/Prefab/脚本回归和可见
鼠标交互尚需独立执行。原生 ECS Transform 层级已实现，WorldPose 继续服务旧的显式
GameObject 桥接；原生矩阵到引擎子系统的专用适配仍需完成。

## Phase 5：Baking 与资源

- [x] 显式字段 codec、类型版本迁移、完整性校验、未知/损坏数据诊断。
- [x] 文档局部实体引用、Prefab 重映射/覆盖/批量回滚及 ECB InstantiatePrefab。
- [x] ECS Scene 数据保存/加载、纯数据解析、所属线程提交与 owner 卸载。
- [x] 原生 LocalTransform/Parent/WorldTransform、dirty 传播、环检测与失效父清理。
- [x] Baker 注册、持久 ObjectId 输入、确定性额外实体、Full/Incremental 与内存缓存。
- [x] 删除组件/对象/额外实体、资源版本失效、失败保留旧产物与 Full 验证回退。
- [x] 显式应用成功产物到独立 Preview World，内容不变时保留句柄，Play 生命周期独立。
- [x] Scene 持久对象身份的自动映射、保存/加载保留、Prefab 实例和 Hierarchy Undo/Redo。
- [x] Live Baking 的 Editor 事件/Undo 触发、直接字段/资源版本轮询、事务/Play 隔离和 UI。
- [ ] Prefab Stage 的持久身份、Authoring 启用/禁用等完整工作流，
      原位预览增量补丁和磁盘 Baking Cache。参见 [Editor Baking](Editor-Baking.md)。
- [x] JobSystem/BackgroundJobQueue 异步 Scene/SubScene 请求管理、自包含分块与安全点提交。
- [ ] 跨块实体引用、卸载后的引用修复、按实体时间预算的增量提交与空间流式策略。
- [x] 既有 AssetDatabase/ArtifactDB/Cook 原生注册、依赖闭包、资源解析默认适配、
      RuntimeAssetManifest/Npak 打包与无窗口 RuntimeWorld 加载。
- [x] Player 的 `.ecsscene` 启动配置、原生构建根、GUID/子资产身份、C++ 注册回调、
      初始加载失败回滚与 Stop/Begin 重启，以及真实 Player 的包自检入口。
- [ ] 完整 Player 可见运行和 GPU/游戏交互验收；当前启动与内容验证不能替代
      原生 ECS 渲染/物理组件适配。参见 [Player-Startup](Player-Startup.md)。
- [ ] 普通 ECB 任意 payload 中的延迟实体引用修正；当前 Prefab 内部引用已有 codec 重映射。

## Phase 6：子系统和脚本

- [ ] Mesh/Sprite 原生 ECS render component、资源引用、快照提取、proxy 生命周期，
      实际 GPU 绘制与删除后资源清理。当前仅提供显式传统表现对象的桥接。
- [ ] 原生 ECS 3D/2D Rigidbody/Collider/actor handle、速度和碰撞事件适配；
      Fixed Step 前后同步、动态/静态/kinematic 权威与后台安全性。
- [ ] Animator/Particle Effect 句柄与批量控制；AI 行为示例。
- [ ] C# 值拷贝 ABI、World/Entity 有效性、类型注册/封送/Query/System 和热重载。
- [ ] Lua 受控值代理和安全阶段 API；保持传统生命周期，避免逻辑重复执行。

## Phase 7：Editor

- [ ] World/Entity/Archetype/System 面板、依赖错误、占用率和性能视图。
- [x] Scene 对象真实 UUID 到 authoringId，预览通过持久 ID 重新解析 Entity。
- [x] Full/Incremental/Live Baking UI、CPU 实体/组件字段检查和 `.ecsscene` 原生导出。
- [x] 真实 Hierarchy 改名/重挂接/删除/复制的 Undo/Redo 驱动安全点 Baking，
      活跃编辑/事务、Pending 和 Play 域期间不捕获半完成状态。
- [ ] Scene View 双向选择、通用 Authoring Inspector/组件增删、Prefab Stage，
      以及可见窗口、鼠标与 GPU 交互验收。

## Phase 8：完整质量验收

- [x] Scene 捕获/编码/解析/文件读写/加载/卸载与 Prefab 批量生成/销毁基准：
      1,000/10,000/100,000/1,000,000 Entity，共 36 条成功实测。
- [ ] 更紧凑的 Cook/二进制产物、分块规模优化及传统 Component 对照；JSON 当前仍有
      明显的内存和加载时间开销，不能用 Core 移动更新速度推导完整场景性能。
- [ ] ECS Rendering 与 Baking 演示项目，以及带 GPU/编辑器的完整 Hybrid 场景；
      Basics/MassSimulation/无窗口 Lua Hybrid/Baking 文件示例已提供，运行证据见 Validation。
- [ ] 完整 Editor/旧项目/GPU/物理/动画/C#/Lua 集成回归。
- [ ] macOS、Linux 构建运行，适用平台上的 UBSan/TSan；ASan 结果见 Validation。
- [ ] World 级完整资源/动态容器内存统计、每帧分配追踪和更多 Job/调度计时。
- [ ] 热路径专用批量创建/迁移、共享分组索引和调度流水化等后续性能优化。

继续顺序：原生子系统/脚本 → Prefab Stage 与完整 Editor 工具 → 完整回归。每项需要
实际代码、构建和测试结果，不能通过添加空类或 TODO 宣布完成。
