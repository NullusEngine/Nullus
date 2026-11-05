# GameObject 与 ECS 混合开发

接入代码在 `Runtime/Engine/ECS/RuntimeWorld.*` 和
`Runtime/Scripting/RuntimePlaySession.*`。保留原有 Scene、Component、Prefab、
资源、物理、动画与脚本生命周期；默认 PlaySession 不创建 ECS World。

在 Begin 之前调用 `session.EnableECS(setup, config)`。setup
在每次 Begin 创建新 World 后执行，可以注册游戏组件/System、创建 Entity，并通过
`runtime.Link(entity, sceneObject, options)` 显式关联。已有项目不调用 EnableECS 即
继续走原流程。`session.GetECS()` 在未启用或结束后返回 nullptr。

应用初始化共享 JobSystem 后，纯数据 System 和大批量查询使用现有 Jobs。ECS 不管理
全局服务的启动/关闭；未初始化时 System 串行运行。异步分块入口为
`runtime.Streaming().RequestLoad(chunk, path, options)`，无需另建更新循环：RuntimeWorld
在 Initialization/PreFixedUpdate/Update 的安全点轮询提交，每次最多一个就绪分块；
Stop 和旧 Scene 替换会取消待处理请求并卸载分块。详见 [Scene-Streaming](Scene-Streaming.md)。

使用已导入或已打包的数据时，在 setup 中调用
`runtime.ConfigureAssets(runtimeDatabase, contentRoot)`，再通过
`runtime.RequestSceneAsset(chunk, {guid, "ecs-scene:main"})` 请求场景，或通过
`InstantiatePrefabAsset` 实例化 Prefab。该入口复用同一分块安全点，并默认提供
GUID/内容版本解析；目录与 Npak 都经现有 ContentSource 读取。Player 自动 ECS 启动
与注册约定见 [Player-Startup](Player-Startup.md)，资源边界见 [Asset-Pipeline](Asset-Pipeline.md)。

WorldPose 是混合桥接的世界空间 TRS 值，使用固定 float 数组，避免把旧 Math 类型
的 ABI 或潜在抛异常复制嵌入 Chunk。它不等同于完整的 ECS LocalTransform/Parent
层级系统。原生 ECS LocalTransform/Parent/WorldTransform 已实现，见
[Transform](Transform.md)；它没有自动写入这个 WorldPose 运行时桥接。

| TransformAuthority | 写入来源 | 同步行为 |
| --- | --- | --- |
| None | 双方独立 | 只关联身份，不同步 Transform |
| GameObject | Scene/传统组件/物理 | 输入安全阶段复制 GameObject 世界 pose 到 ECS；不写回 GameObject |
| Entity | ECS System | 输出阶段通过既有 SetWorldScale/Rotation/Position 写入 GameObject；不从 GameObject 覆盖 ECS |

动态 Rigidbody/Rigidbody2D 由物理系统决定 pose，Entity authority 会被拒绝。将刚体改为动态前
应先明确改变桥接 authority。Kinematic/静态桥接继续通过现有 Scene 物理同步路径
提交；没有新的求解器，也没有 worker 对物理世界的任意写入。

传统组件和 ECS 同时写入 Entity-authoritative GameObject 的 Transform 属于错误
的业务所有权设计，不能期待双向合并。GameObject-authoritative 的 ECS 数据应作为
输入使用；写它不会把修改传回 GameObject。持续同步方向始终唯一。

## 生命周期与身份

桥接保存 Entity 值句柄和 Object InstanceID，每次使用重新解析。GameObject 销毁
通知只标记链接，ECS 结构清理在下一安全点执行；默认销毁其 Entity。Entity 销毁
默认解除链接，保留传统对象。只有明确设置 destroyObjectWithEntity 的表现对象才
跟随 Entity 删除，避免删除旧 Player、Camera 或 Authoring 对象。

Link 自动记录对象所属 Scene；替换活动 Scene 会卸载旧 Scene 所属的 ECS Entity。
未归属实体保留，同一 World 可以容纳显式指定多个 scene owner 的实体。
authoringId 是调用者提供的持久身份字符串，支持反向查找；编辑器 UUID/资源 GUID
到该字段的自动 Baking 接入尚未实现。不要传 Runtime EntityHandle 代替持久身份。

## PlaySession 阶段顺序

第一次有效 Tick/Step 执行 Initialization，并启动 System。之后：

1. 每个已有固定步：ECS PreFixedUpdate → 传统 FixedUpdate → ECS FixedSimulation →
   既有 Physics Step/事件 → ECS PostPhysics。
2. 传统 Update → ECS Update → 既有 Animation → 传统 LateUpdate → ECS LateUpdate。
3. 既有延迟销毁和垃圾回收 → ECS Presentation → ECS Cleanup。

输入同步在 Initialization/PreFixedUpdate/FixedSimulation/PostPhysics/Update；输出
同步在 Initialization/FixedSimulation/LateUpdate/Presentation。所有工作由同一个
RuntimePlaySession 驱动，使用其 FixedStepClock、Time Scale 和单步结果。
Pause 不增加 ECS 模拟帧；Step 复用现有单步路径；Stop 先停止 ECS System 并释放
World，再停止传统 Scene。下次 Begin 产生新的 World 身份，旧句柄不会恢复有效。

集成自动测试在 `Tests/ECS/HybridTests.cpp`。通过 CPU 测试不代表已完成 Editor
鼠标交互、GPU Mesh/Sprite 绘制、原生 ECS PhysX/2D actor 适配或脚本 ECS API 验收。

`Samples/ECS/Hybrid.cpp` 是可运行的无窗口混合示例：真实传统 Lua Player、Camera、
ECS Enemy 和受控表现对象在同一 PlaySession 更新。正常仓库构建目标为
`NullusECSHybridExample`，输出位于 `build/runtime/<配置>/`。它验证 CPU 工作流，
没有创建 GPU 绘制窗口。
