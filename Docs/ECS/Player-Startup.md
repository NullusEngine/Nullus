# Player 的 ECS 启动场景

构建配置的 `startScene` 可指定 `.ecsscene`，支持 `World.ecsscene` 和
`Assets/World.ecsscene`。构建器将它作为已 Cook 的原生资产根收集，沿 ImportedArtifact
依赖边生成内容闭包；不把 ECS 文档交给传统 ObjectGraph 解析器。
Scene 的 `.meta`、目标平台的当前 ECS 产物或其依赖缺失时，构建失败。
写出 Game.ini、Player.info 和传统 loose scene 时，启动路径统一为 Assets 内的相对路径。

构建器在 `Data/User/Game.ini` 写入持久资产身份：

```ini
start_scene=World.ecsscene
start_scene_asset=<scene asset GUID>
start_scene_subasset=ecs-scene:main
```

默认分发内容只需要原生容器、清单与 Npak，启动无需 `.ecsscene` 源文件、`.meta`
或开发期 Library。切回传统启动场景时会清除旧的 ECS 身份配置。打包校验也检查启动
身份能在清单中解析为 ECS Scene。

## 启动与生命周期

Player 按扩展名选择启动入口。ECS 场景使用现有传统宿主 Scene 承载相机、灯光和
传统子系统生命周期，PlaySession 创建可选 RuntimeWorld。配置入口先注册内置
Transform 组件、codec 和层级系统，配置只读资产快照，再调用游戏注册回调。
后台读取与纯数据解析继续使用现有 BackgroundJobQueue；所属线程完成初始分块提交后，
PlaySession.Begin 才返回成功。普通帧保持现有安全点轮询，启动不会另建线程池。

初始分块使用应用 chunk id `1`。游戏注册回调应注册类型、codec、资源或 System，
不要占用这个分块。后续可通过 RuntimeWorld 的普通分块接口加载其他场景。
失败不会发布半加载的 RuntimeWorld；Stop 释放它，下一次 Begin 使用新的 World 身份。
损坏容器、错误 GUID/目标、缺少 codec 或资源失败都会报告启动错误。

开发运行可以在没有显式 `start_scene_asset` 时，从指定源文件的既有 `.meta` 解析身份。
打包运行不使用此回退；仍需由应用启动共享 JobSystem，并提供已加载的运行时清单。

## 游戏组件注册

内置 Player 能加载已注册的内置组件。包含游戏自定义 C++ 组件的源构建入口，可向
`Game::Core::Application` 的可选 `registerECS` 参数传入回调：

```cpp
struct Health { int value = 100; };
NLS::Game::Core::Application application({}, {}, projectPath, {},
    [](NLS::Engine::ECSIntegration::RuntimeWorld& runtime,
       NLS::Engine::SceneSystem::Scene&)
    {
        auto& world = runtime.GetWorld();
        world.RegisterComponent<Health>("MyGame.Health");
        NLS::ECS::RegisterComponentCodec<Health>(world.GetRegistry(),
            [](const Health& value, const NLS::ECS::EntityWriteContext&)
            { return NLS::ECS::ComponentJson{{"value", value.value}}; },
            [](Health& value, const NLS::ECS::ComponentJson& json,
               const NLS::ECS::EntityReadContext&)
            { value.value = json.at("value").get<int>(); });
        // 在此注册游戏 System；不重复注册内置 Transform 系统。
    });
application.Run();
```

类型名和 codec 版本必须与 Baking 产物一致。嵌入式宿主也可直接使用
`Project/Game/ECSSceneStartup.h` 的 ConfigureECSStartScene 配置现有 PlaySession。
注册回调及其模块须活到 PlaySession 结束；标准 Application 先销毁 Game 和 World，
再关闭宿主模块。当前回调是显式 C++ 入口，并非 C#/Lua ECS ABI 或动态插件自动注册。

## 验证边界

`Tests/ECS/PlayerStartupTests.cpp` 调用真实 Cook、构建依赖收集、staging DLL、Npak、
Player 清单/授权与 PlaySession。正常 CMake 目标为 `NullusECSPlayerTests`，依赖
GameProject。当前构建环境、实际运行结果与命令记录在 [Validation](Validation.md)。

实际 `Game.exe --package-self-test` 已识别 ECS 启动身份，并读取 Npak 内的原生容器，
检查类型、内容校验和资源版本，无需 loose ECS 文档。它不加载未知游戏组件的 C++ codec；
完整解码检查在 PlaySession.Begin 中执行。AssetPipeline 示例现在同时写出 Player.info、
Data/User/Game.ini 和运行时清单，便于检查该内容布局。

此启动链路不提供原生 ECS render proxy、物理 actor 或脚本 ECS API。传统宿主相机
能走既有 Renderer；原生 ECS Transform 数据的实际绘制仍待子系统适配。可见 Player、
完整游戏交互与 GPU 验收独立跟踪在 [Remaining](Remaining.md)。
