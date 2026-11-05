# 异步 Scene/SubScene 分块

`ECS::SceneStreaming` 复用 Base 的 `BackgroundJobQueue`。后台只读取/校验 JSON 和构造
WorldDocument；资源解析、codec 解码和实体提交均在 World 所属线程执行。它不创建
线程池，不启动或关闭共享 JobSystem。输入文件必须是 `DocumentKind::Scene`，可由
Baking 的成功产物直接提供。

```cpp
// 应用启动时初始化 Base::Jobs；在应用退出、其他使用者结束后统一关闭。
Base::Jobs::InitializeJobSystem();
World world;
RegisterGameComponentsAndCodecs(world); // 游戏提供实际注册函数。
SceneStreaming streaming(world);
ArchiveLoadOptions options;
options.sceneOwner = 1001;
options.resolveResource = ResolveGameResource; // 游戏接入已有资源服务。
auto request = streaming.RequestLoad(1, "Assets/Baked/terrain.ecs.json", options);

// World 所属线程，每帧安全点：不等待仍在运行的解析任务。
streaming.Pump(1);
auto info = streaming.GetInfo(request);
if (info.status == SceneLoadStatus::Loaded) {
    auto terrain = streaming.Find(1, "terrain");
    // 使用 terrain 前检查有效性；成功替换后重新解析。
}

// 不再需要此分块时，取消待处理替换并删除该实例仍存活的实体。
streaming.Unload(1);
streaming.Forget(request); // 释放终态诊断；不会卸载已加载实例。
```

真实可运行示例见 `Samples/ECS/SceneStreaming.cpp`。混合运行用
`runtime.Streaming()`，RuntimeWorld 已在 Initialization/PreFixedUpdate/Update 开始前
轮询一次；Scene 切换和 Stop 自动取消/卸载。SceneStreaming 自身析构仅取消后台
结果发布，单独使用时需显式 Unload/UnloadAll，或由 World 最终释放实体。

每个非零 chunk key 同时保留一个活动实例和一个当前请求。新请求被队列接受后才
取消同 key 的旧请求，旧活动实体一直保留到新实例成功加载。文件、类型、版本、
资源或 codec 错误只删除候选实体。成功替换后，旧 Entity 句柄失效；调用方应再次
通过 Find 解析。卸载只删除这个实例记录的 Entity，保留同 owner 的其他实体。

请求句柄包含服务身份和递增请求号，不能跨 World/服务混用。Cancel 是协作取消，
不会中断正在进行的任意文件读取/JSON 解析；取消的结果不会提交。解析数据不捕获
World、SceneStreaming、资源解析器或 GameObject，因此 World/服务可以先销毁。
正常运行的取消结果在 Pump 后回收，Forget 可以提前释放终态记录；默认最多记住
256 个请求，达到上限会报错，避免诊断历史无限累积。

Pump 每次最多提交指定数量的成功分块，0 表示全部。一个分块整体解码/提交；大文件
仍可能在主线程花费较长时间，应用应按适合帧预算的块大小烘焙。`Wait(request)` 是
显式同步工具/测试入口，不用于普通帧；禁止同步等待的 Jobs scope 会拒绝它。
加载请求、提交和卸载检查拥有线程与安全点，Query/System 迭代中应把这些操作留到
安全点。Cancel 只撤销结果发布，允许在主线程资源回调内调用；Forget 拒绝在提交
回调期间删除请求记录。

Native ECS 资产可通过 `RuntimeWorld::ConfigureAssets` / RequestSceneAsset 读取目录或
`.npak`，详见 [Asset-Pipeline](Asset-Pipeline.md)。Core 的 RequestLoadSource 则接收
遵循独立所有权和并发读取约定的 IWorldDocumentSource，保持 Core 不依赖资源数据库。

每个文件中的实体引用独立重映射。当前支持多个自包含分块；尚未提供跨分块实体引用、
软引用重绑定、按实体时间预算提交、自动距离/空间流送、Editor SubScene UI 或原生
完整 Player 自动启动流程。全量任务边界见 [Remaining](Remaining.md)，当前测试见
[Validation](Validation.md)。
