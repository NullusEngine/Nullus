# Nullus ECS 开发指南

这是分阶段交付记录。当前代码提供原生 Archetype/Chunk ECS、查询、延迟命令和
System/JobSystem 调度，以及原生 Transform、Scene/Prefab 存档、异步分块、CPU Baking，
原生资产导入/Cook 与 Npak 内容加载。完整生产验收尚未完成，原生 ECS 渲染/物理适配、
可见 Player 验收、C#/Lua ECS API 和编辑器
工作流的状态见 [剩余任务](Remaining.md)。

- [架构与仓库审查](Architecture.md)
- [System、Job 与命令编写](Systems-Jobs.md)
- [GameObject 混合开发](Hybrid.md)
- [原生 Transform 层级](Transform.md)
- [Authoring、Baking 与资源边界](Authoring-Baking.md)
- [Editor 持久身份、Undo 与 Live Baking](Editor-Baking.md)
- [异步 Scene/SubScene 分块](Scene-Streaming.md)
- [ECS 资产、Cook 与 Player 内容包](Asset-Pipeline.md)
- [Player 的 ECS 启动场景](Player-Startup.md)
- [性能测量与优化](Performance.md)
- [真实构建与测试记录](Validation.md)
- [阶段交付报告](Delivery.md)
- [Phase 5 实施与验收](Phase5-Plan.md)

## 注册与访问

```cpp
#include <ECS/Core/World.h>
using namespace NLS::ECS;

struct Position { float x = 0, y = 0, z = 0; };
struct Velocity { float x = 0, y = 0, z = 0; };

World world;
world.RegisterComponent<Position>("MyGame.Position");
world.RegisterComponent<Velocity>("MyGame.Velocity");
auto entity = world.CreateEntity();
world.AddComponent<Position>(entity);
world.AddComponent<Velocity>(entity, {1, 0, 0});

world.Query<Write<Position>, Read<Velocity>>()
    .Each([](Entity, Position& position, const Velocity& velocity)
    {
        position.x += velocity.x * (1.0f / 60.0f);
    });

world.DestroyEntity(entity);
// world.IsAlive(entity) == false; reuse increments the generation.
```

稳定名称决定默认的 64 位类型 ID，不能随意改名；名称/ID/原生类型/版本冲突会报错。
进程内 `type_index` 只用于 C++ 类型查找，不作为持久化 ID。组件序列化
codec 需要显式注册；注册后 `serializable` 元数据为 true。Tag 自动保存存在性，
派生数据可以显式注册为 transient。持久化使用方式见 Authoring-Baking.md。

组件必须可默认构造，移动构造和析构必须为 `noexcept`。`std::string`、`std::vector`
和正确实现移动的独占所有权类型可以使用。移动可能抛异常的类型会收到编译期诊断；
请使用满足条件的拥有型间接数据，或定义经验证的移动操作。现有 Math 值类型并非
全部声明了 noexcept 移动，不能假定任意 Math 包装组件都满足这一约束。
组件的构造、移动、比较和析构管理自身拥有的数据；涉及 World 的逻辑放在 System
生命周期或命令中执行。

实体句柄仅包含 World 身份、Index、Generation，不持有内存。不要持久化它，也不要
把它当成 GameObject 指针或 Scene/Prefab UUID。World 关闭、实体销毁、跨 World
访问均有检查，Generation 耗尽的 slot 不再复用。
Entity.IsValid 只检查句柄字段是否完整；存活状态必须用 World.IsAlive 检查。

## 类型类别

| 类别 | 注册/访问方式 | 存储和修改规则 |
| --- | --- | --- |
| Data | 默认注册，ReadComponent/WriteComponent | Chunk 中的连续列 |
| Tag | `ComponentKind::Tag`，`HasComponent` 或 Query.All | 必须为空类型，没有数据列 |
| Buffer | `DynamicBuffer<T>` + `ComponentKind::Buffer` | 每个值拥有自己的 vector，按正常 C++ 生命周期迁移 |
| Shared | `ComponentKind::Shared`，Read 或 Chunk.Shared | 需要复制和相等比较；SetComponent 通过迁移改变分组，不能获取可写列 |
| Enableable | `ComponentKind::Enableable`，SetEnabled | 独立启用位，不改变 Archetype |
| Resource | 注册值类型后 SetResource/ReadResource/WriteResource | World 级所有权，依赖声明与同类型实体组件相互区分 |

Tag 只表示存在性，不承载逐实体的对象构造/析构行为。

`CreateEntity(span<ComponentId>)` 一次确定布局。`CreateEntities` 返回拥有值句柄的
vector；失败会回滚该批已创建实体。`DestroyEntities` 跳过过期/重复句柄并返回实际
销毁数。`SetScene(entity, nonzeroOwner)` 和 `UnloadScene(owner)` 管理同一 World
内的场景归属；未归属实体使用 owner 0。

## 查询规则

查询支持 All、Any、None、OptionalRead/OptionalWrite、Enabled、Disabled、Changed、
WithEntities 和 Chunk 遍历。All/Any 默认考虑 enable 位；None 按组件的结构存在性
排除。Disabled 可覆盖对应必需组件的默认启用条件；IncludeDisabled 取消默认筛选，
显式 Enabled/Disabled 条件仍然有效。

查询缓存 Archetype，不缓存组件地址；新 Archetype 会增量加入匹配缓存。Changed
过滤器使用 Chunk 版本，可能返回没有逐个写入的同 Chunk 实体。Count 和只读访问
不会推进写版本。重复设置同类型 Changed 游标会替换旧游标，不会持续累积过滤器。

游标必须在安全点捕获，所有写借用和 Job 已结束时使用 `world.GetChangeToken()`。
System 内使用 `context.lastUpdate`，它只在上次成功且已完成所有工作后推进。
不要在可写引用仍用于后续写入时捕获游标；不要跨游标边界、结构变更或任务结束保留
组件引用、span 或指针。ChunkView 不可复制。

`EachChunk` 提供整列 span；实体过滤和 enable 条件仍需逐行检查 `chunk.Matches(row)`。
普通 `Each` 会自动检查。组件的 const 访问不能替你约束共享指针指向的外部可变对象；
外部共享状态必须按 Resource 或其他明确同步协议管理。

## 运行示例与测试

不需要创建窗口或加载 Editor：

```powershell
cmake -S Tests/ECS/Standalone -B build/ecs-headless
cmake --build build/ecs-headless --config Debug --target NullusECSTests NullusECSTransformTests -- /m:1
ctest --test-dir build/ecs-headless -C Debug --output-on-failure
cmake --build build/ecs-headless --config Release --target NullusECSBasics NullusECSMassSimulation NullusECSBenchmark NullusECSStreamingExample -- /m:1
build/ecs-headless/Release/NullusECSBasics.exe
build/ecs-headless/Release/NullusECSMassSimulation.exe 100000 --single
build/ecs-headless/Release/NullusECSMassSimulation.exe 100000
build/ecs-headless/Release/NullusECSStreamingExample.exe
```

独立目标编译真实的 ECS、Base Jobs/BackgroundJobQueue/Profiler，以及 TaskGraph 回归源码，不包含窗口、GPU、
反射生成、编辑器或托管构建依赖。正常仓库构建提供 `NLS_ECS`；开启 NLS_BUILD_TESTS
后，ECS 用例统一加入 `NullusUnitTests`，用 `--gtest_filter="ECS*.*"` 运行。
Hybrid 测试验证真实 Scene/GameObject/PlaySession 接入，Editor 资产用例使用真实
Editor 资产 Facade，不需要启动可见窗口。`NullusECSHybridExample`、
`NullusECSBakingExample`、`NullusECSAssetPipelineExample` 仍作为独立示例提供。
当前独立 Debug/Release/ASan 各 122 项通过；资产与原生集成结果和边界见
[Validation](Validation.md) 与 [Asset-Pipeline](Asset-Pipeline.md)。
