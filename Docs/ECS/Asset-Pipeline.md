# ECS 资产与 Player 内容链路

本阶段接通烘焙数据的原生资产链路，复用 AssetMeta、ArtifactWriter/ArtifactDB、
RuntimeAssetManifest、ContentSource 和 Npak，不引入另一套资源数据库或打包格式。

实施范围：

1. `.ecsscene` / `.ecsprefab` 注册为独立资产类型，保留旧 Scene/Prefab 类型和编号。
2. 共享编译入口校验 ECS 文档，生成规范数据，解析目标平台的资源 GUID/固定内容版本，
   并记录原生依赖边；Editor 导入与无窗口 Cook 复用该入口。
3. 运行时从 RuntimeAssetDatabase 获取资产，校验原生容器类型、版本、身份和内容摘要。
   资源解析默认检查 GUID、版本与内容可用性；具体资源构造使用既有 loader。
4. 分块后台读取支持不可变数据源，使目录和 `.npak` 内容均能进入现有 Jobs 加载路径；
   World 解码、资源检查、替换和卸载继续使用所属线程的安全点。
5. 用真实 Cook、ArtifactDB、依赖收集、Npak 和 RuntimeWorld 验证从烘焙数据到运行时实体。

`hashOrVersion` 的默认引擎适配语义是原生 artifact 的 `contentHash`。固定版本需要
针对目标平台的资产快照烘焙；只记录 GUID 的文档可由 Cook 解析目标平台的当前产物。
不把 Editor 的 GPU 产物摘要自动当作其他平台的同一版本。自定义版本方案仍可显式
提供资源解析回调。

全量 Editor Live/Undo、跨分块实体软引用、原生渲染/物理组件和脚本 ECS API 的状态
单独保留在 [Remaining](Remaining.md)。实际验证见 [Validation](Validation.md)。

## 烘焙、导入与 Cook

BakingOptions.outputPath 使用 `Assets/World.ecsscene`。Prefab 文档使用 `.ecsprefab`。
旧 `.scene` / `.prefab` 继续表示传统 ObjectGraph 格式，两组类型的旧编号没有改变。
现有 Editor SourceAssetDatabase 会识别新扩展名并管理 `.meta`；无窗口代码首次生成
文件时，需用现有 `AssetMeta::CreateForAsset(path).Save(GetAssetMetaPath(path))` 创建
GUID sidecar，后续保存保留该 GUID。

Editor 的 Import/Reimport、缓存快照、原生容器发布和失败回滚已加入 ECS 分支。
批量导入资源后再导入引用它的 ECS 文件，会从同一 Facade 的未刷盘 ArtifactDB 缓存
获取版本。跨操作使用新的快照，以便看到资源重导入。

共享 `AssetCooker::CookAssets` / CookProject 也识别新类型。普通资源沿既有顺序
处理，ECS 文件再按资源依赖拓扑排序，因此冷 Cook 可接收顺序相反的 ECS 文件。
环、缺失资源、错误平台、Stale 记录和固定版本不匹配都会返回失败诊断。

共享编译入口 `CompileECSAsset` 校验格式、大小、完整性与文档种类，输出规范 JSON，
由 ArtifactWriter 封装为现有原生容器并存入内容寻址目录。主子资产键固定为
`ecs-scene:main` / `ecs-prefab:main`，不随文件名改变。Cook 无需加载游戏 C++ 组件；
组件版本迁移、codec 约束与未知类型检查在游戏 World 解码时执行。完整游戏语义仍需
游戏注册的组件/System 验证。

每个资源记录两种既有依赖：ImportedArtifact 指定 GUID/子资产键，用于构建依赖闭包；
SourceAssetGuid 保存本次解析的内容摘要，用于 Cook 缓存失效。GUID-only 引用在资源
变化后重发 ECS 产物；固定版本失配时保留上一份产物并报告失败。ECS 文件不能用
Editor 的目标相关摘要代替其他平台的已烹制版本。自定义版本方案可使用底层
WorldArchive 的显式解析回调；原生 Cook 的固定版本语义为 artifact.contentHash。

## 运行时加载

应用先启动既有 JobSystem；RuntimeWorld 复用该服务，不负责其初始化和关闭。
通常在现有 PlaySession 的 ECS setup 中配置以下内容：

```cpp
using namespace NLS::Engine::ECSIntegration;
RuntimeWorld runtime;
RegisterTransformSystem(runtime.GetWorld());
// 游戏在这里注册自己的组件与 codec。
runtime.ConfigureAssets(runtimeDatabase, playerDataRoot);

auto request = runtime.RequestSceneAsset(1, {sceneAssetId, "ecs-scene:main"});
// 普通帧由 RuntimeWorld 的既有阶段安全点轮询；工具也可以显式调用 Pump。
runtime.Streaming().Pump();
auto prefab = runtime.InstantiatePrefabAsset({prefabAssetId, "ecs-prefab:main"});
```

ConfigureAssets 默认提供 GUID、可用性与固定版本解析，通常无需游戏逐次拼接 lambda。
需要自己的策略时，可在 RequestSceneAsset 的 ArchiveLoadOptions 中显式提供
resolveResource。DescribeResource 返回指定 RuntimeAssetRef 的 GUID/内容摘要，
可用于目标相关的 Baking 资源版本快照。

运行时只需 RuntimeAssetDatabase 与 ContentSource，不读取开发期 ArtifactDB。
开发工具可用 `ECSAssetLoader::FromArtifactDatabase` 创建现有目标的不可变快照。
`CreateSource` 支持目录和已挂载 `.npak`；SceneStreaming.RequestLoadSource 在既有
BackgroundJobQueue 读取纯数据，World 提交仍在拥有线程。内容源实现须遵守
IWorldDocumentSource 的并发读取和独立所有权约定，不能捕获 World、GameObject 或 VM。

读取检查现有运行时授权、存储键与 SHA-256、原生类型/版本/身份/目标，以及内部
ECS 格式/完整性。资源解析检查清单版本和可用性；实际纹理/模型等的解码、创建与
GPU 上传继续由原有资源 loader 负责。一个分块的 payload 默认最多 256 MiB，容器
元数据另限制为 16 MiB；可在 ArchiveLoadOptions.limits 显式调整数据限制。

## 内容包与示例

现有 BuildProfile.keepAssetPaths 或 assetPackSelections 可将 ECS 数据纳入现有
BuildDependencyCollector / RuntimeManifestBuilder 闭包。内容仍由既有 NpakWriter
写入 `.npak`，没有 ECS 专用的包格式。RuntimeManifestSerialization 保留新类型。

`NullusECSAssetPipelineExample` 使用实际 GameObject 和 Transform Baker，烘焙 Scene，
生成 ECS Prefab，创建原生元数据、Cook、收集清单并写入内容包，再销毁开发缓存并
通过 RuntimeWorld 读取内容包。源码在 `Samples/ECS/AssetPipeline.cpp`。

Player 的 `.ecsscene` 启动入口和构建身份配置见 [Player-Startup](Player-Startup.md)。
游戏自定义组件须在解码前注册 codec；其他分块仍可通过 ECS setup 中的 ConfigureAssets
与请求接口加载。完整 Player 可见运行验收独立跟踪，不由无窗口测试替代。
