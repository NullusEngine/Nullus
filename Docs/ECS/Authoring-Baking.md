# ECS 持久化、Prefab 与 Baking 使用指南

当前已实现字段 codec、版本化 JSON 存档、Scene 数据加载、Prefab 覆盖和引用重映射、
Full/Incremental Baking、内存缓存、独立 CPU Preview 和 Editor Live 工作流。
传统 Scene/Prefab 格式继续走 ObjectGraph；Scene 保存与加载现在保留实际对象 ID。
原生资产/Cook 与异步 SubScene 已接入，详见 [资产流程](Asset-Pipeline.md) 和
[Editor Baking](Editor-Baking.md)。完整子系统适配仍在 [剩余任务](Remaining.md) 中。

## 注册字段 codec

```cpp
#include <ECS/Serialization/WorldArchive.h>
using namespace NLS::ECS;
struct Target { std::string label; Entity entity; };

world.RegisterComponent<Target>("Game.Target", ComponentKind::Data, 1);
RegisterComponentCodec<Target>(world.GetRegistry(),
    [](const Target& value, const EntityWriteContext& context)
    {
        return ComponentJson{{"label", value.label},
                             {"entity", context.EntityId(value.entity)}};
    },
    [](Target& value, const ComponentJson& json, const EntityReadContext& context)
    {
        value.label = json.at("label").get<std::string>();
        value.entity = context.Resolve(json.at("entity").get<std::string>());
    });
```

codec 的目标值已经默认构造；按正常 C++ 赋值规则设置字段。不能在活对象上直接
placement-new，也不能保存组件的原始字节、地址或进程 type_index。Tag 自动保存
存在性，字段必须为空；其余持久组件必须显式注册 codec。临时/派生组件通过
`registry.RegisterTransient(typeId)` 明确排除；未知持久化策略会报错。

codec 注册后 serializable 元数据为 true。已注册 codec 不可替换；热重载前仍需结束
引用旧模块回调的 World。组件版本变化时，RegisterComponentCodec 的第四个参数
可提供 `(uint32_t sourceVersion, const ComponentJson&) -> ComponentJson` 迁移函数。
旧版本没有迁移函数、未来版本、类型名称/ID/存储种类不匹配都会被拒绝。

## 保存、加载与 Scene 归属

```cpp
const std::array records{
    PersistentEntity{player, "player", playerObjectId.GetGuid().ToString()},
    PersistentEntity{weapon, "weapon", weaponObjectId.GetGuid().ToString()}};
auto document = WorldArchive::Capture(world, records, DocumentKind::Scene);
WorldArchive::WriteFile("Data/gameplay.ecs.json", document);

auto parsed = WorldArchive::ReadFile("Data/gameplay.ecs.json");
auto loaded = WorldArchive::Instantiate(runtimeWorld, parsed, {.sceneOwner = 42});
auto runtimePlayer = loaded.Find("player");
runtimeWorld.UnloadScene(42);
```

文档局部 ID 由调用方定义；CaptureAll 为方便使用生成 e1/e2 等文件内局部标识。
它们不是运行时句柄，也不替代外部资产的持久 GUID。引用必须位于本次保存的集合中，
空引用使用空字符串；跨文档/跨分块实体引用暂未实现，不能静默保存悬空引用。
authoringId 在一个文档内唯一，实例的 byAuthoringId 提供反查；多个 Prefab 实例各有
自己的映射，不把模板作者身份当成全 World 唯一的实例身份。

格式包含 Nullus.ECS 标记、格式版本 1、稳定类型 ID 的 16 位十六进制字符串、组件
版本、规范化 Archetype 表、启用位、字段数据和资源依赖。完整性值使用规范 JSON
数据的 FNV-1a 64 位摘要，用于检测内容损坏，不提供真实性认证。Parse 拒绝重复键、
不一致的布局、非法整数、重复实体/组件、非有限数及超过限制的输入。

默认上限为 256 MiB 文本、200 万实体、每实体 1024 个组件和 128 层 JSON；可通过
ArchiveLimits 显式调整。WriteFile 先编码并验证，再写同一文件系统的独占临时目录，
原子替换目标文件；编码或写入失败不会先截断旧文件。它没有提供跨机器事务或完整
断电持久性保证。

加载先检查类型/版本/资源，创建全部目标实体，再解析引用、准备组件值并提交。
失败回滚本次新建实体，不修改之前存在的实体；空 Archetype/slot 容量可能保留复用。
加载是追加操作；替换场景时先成功加载候选，再销毁旧 ArchiveInstance，避免失败
时提前删除旧内容。owner 0 为未归属实体；分块调用方应为各块分配明确的 owner。

Parse/ReadFile 是不接触 World 的纯数据操作。SceneStreaming 已使用现有 BackgroundJobQueue
执行它们；Instantiate、实体替换和卸载在 World 所属线程的安全点进行。请求替换、
取消、卸载、失败保留与 RuntimeWorld 更新接入见 [Scene-Streaming](Scene-Streaming.md)。
codec 执行期间持有 World 只读借用，捕获 World 后尝试结构修改或越权写入会被拒绝。

资源字段使用 EntityWriteContext.Resource(guid, hashOrVersion) 声明依赖，解码通过
EntityReadContext.Resource 校验已声明 GUID。ArchiveLoadOptions.resolveResource 在
创建实体前验证依赖；资源缺失或版本不匹配会报错。Core 不直接依赖 AssetDatabase，
默认引擎解析、原生资产发布与 Cook 已接入，详见 [Asset-Pipeline](Asset-Pipeline.md)。
原生适配的固定版本使用目标 artifact.contentHash，GUID-only 引用由 Cook 解析当前目标。

## Prefab 与 ECB

Capture 使用 DocumentKind::Prefab 创建模板；InstantiatePrefab 或 InstantiatePrefabs
生成一个或一批实例。每个实例的实体引用独立重映射。批量失败会清理该批之前已生成
的实例；已有 World 内容保留。

PrefabOverride 支持 SetField、SetComponent、AddComponent、RemoveComponent、SetEnabled。
目标使用局部实体 ID 和稳定类型 ID；SetField 使用指向既有字段的 JSON pointer。
所有覆盖先施加于文档副本，随后走同一加载和校验路径，不改写模板。

```cpp
auto pending = context.commands.InstantiatePrefab(prefabDocument, sceneOwner);
context.commands.SetEnabled<Visible>(pending.Find("sprite"), true);
// 阶段末回放；CommandPlaybackResult.Resolve 可取回实际 Entity。
```

ECB 拥有模板/覆盖/加载选项的副本；回放前可释放原模板。Prefab 子实体临时引用包含
buffer/epoch/writer/sequence/element，后续命令可引用其中任意实体。实例失败作为一条
命令诊断，不泄漏半个实例，后续命令仍按既定顺序执行。普通任意组件 payload 中的
延迟 EntityReference 自动修正尚未提供；Prefab 内部引用使用上述字段 codec。

## Baker、缓存和 Preview

```cpp
#include <Engine/ECS/Baking.h>
#include <Engine/ECS/TransformHierarchy.h>
using namespace NLS::Engine::ECSIntegration;

World schemas;
RegisterTransformComponents(schemas);
BakingPipeline pipeline(schemas.GetRegistryLifetime());
RegisterTransformBaker(pipeline);
const auto sources = CaptureSceneBakingSources(authoringScene);

BakingOptions options;
options.outputPath = "Data/level.ecs.json";
auto result = pipeline.Bake(sources, options);
if (!result.succeeded) { /* 保留旧产物，显示 result.diagnostics */ }

BakingPreview preview(schemas.GetRegistryLifetime());
RegisterTransformSystem(preview.GetWorld());
preview.Apply(result);
preview.GetWorld().Systems().RunPhase(SystemPhase::LateUpdate, {});
auto selected = preview.FindAuthoring(sources.front().id);
```

CaptureSceneBakingSources 从 PersistentManager 的 Scene 作用域映射取得真实
ObjectGraph ID。新对象使用 Guid::New，Scene 序列化会写出相同 ID；加载后恢复文件
中的对象、组件和文档身份。改名、改 tag、重排和重新挂接不再改变 Scene 对象身份。
InstanceID 只用于所属线程解析活对象，不参与持久 ID 的生成。低层 BakingSource
仍允许外部提供既有 Serialize::ObjectId。

Prefab 实例沿用实例化器已经生成的 sourceToInstance 身份；多个实例互不冲突，
保存 Scene 后能再次恢复。Hierarchy 的 Undo 快照显式保留 Scene ID，Undo 恢复
会重绑原 ID，Duplicate 则先产生新 ID，并在之后的 Undo/Redo 中保留副本身份。
普通 Prefab 模板写出仍沿用现有模板序列化规则；Prefab Stage 的完整编辑流程尚待接入。

RegisterBaker 注册稳定 ID、版本、Capture 和 Bake 回调。Capture 在主线程读取真实
GameObject，返回包含全部 Bake 输入的 JSON 快照；返回 nullopt 会移除旧贡献。
DependsOnResource 使用现有 Guid 和 resourceRevision 回调记录资源版本。Bake 只消费
不可变输入，通过 BakingOutput.Primary、CreateAdditionalEntity、AddComponent 生成
数据。额外实体使用 ObjectId、Baker ID、局部 key 的确定性 GUID；多个 Baker 可以给
同一主实体贡献不同组件，争用同一组件会报错。

Incremental 模式每次重新 Capture 输入，比较完整规范输入、Baker 版本和依赖版本，
只重跑发生变化的 Baker。删除对象、移除组件、不再匹配的 Baker、删除额外实体和
移除 Baker 都会从候选产物移除对应贡献。缓存目前只驻留内存。

候选数据先在独立真实 World 中解码并验证原生 Transform 层级。增量缓存产物验证
失败时可以尝试 Full 回退；失败保留上一个成功 artifact、缓存和磁盘文件。Full 模式
或 InvalidateCache 可强制重新构建；ReplaceBaker 要求版本递增。

Preview.Apply 显式应用成功结果，内容未变时保留原实体句柄；内容改变时先加载新
实体，再销毁旧的拥有实体。因此改变后必须通过持久 ID 重新解析选择。Preview 和
Play World 的生命周期独立。Editor 的事件/Undo 自动触发和 Baking UI 已接入；
原位实体增量补丁和磁盘 Baking Cache 尚未实现，参见 [Editor Baking](Editor-Baking.md)。

完整可运行示例为 Samples/ECS/Baking.cpp，目标 NullusECSBakingExample：真实
GameObject → Full/Incremental Baking 文件 → Authoring 销毁 → Runtime World 加载。
实际输出和测试见 [Validation](Validation.md)，阶段状态见 [Phase5-Plan](Phase5-Plan.md)。
