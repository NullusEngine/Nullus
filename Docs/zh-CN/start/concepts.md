# 核心概念与术语

本页给出 Nullus 的基本对象模型，并把它与 Unity、Unreal 的常用概念对齐。术语不追求一一对应，只说明“在 Nullus 里，同样的目标应该找谁”。

## 对象模型

| 概念 | 说明 | 生命周期 |
| --- | --- | --- |
| `Scene` | 一组 GameObject 的容器，是加载、保存与运行的单位 | 由 `SceneManager` 拥有 |
| `GameObject` | 场景中的实体，持有名字、标签、激活状态与组件列表 | 归属场景 |
| `Component` | 挂在 GameObject 上的行为或数据单元，每个 GameObject 自带一个 `TransformComponent` | 归属 GameObject |
| `Object` | 引擎对象基类，提供实例 ID 与句柄查找 | 见[对象标识](#对象标识与句柄) |
| `Asset` | 磁盘上的可导入内容，带 GUID、类型与子资源 | 由资产数据库管理 |

组件不是手写注册表，而是由 MetaParser 生成的反射元数据驱动：类型注册、Inspector 绘制、序列化字段与脚本 API 清单都来自同一份声明。

## 对象标识与句柄

引擎对象在序列化时使用“对象标识”而不是裸指针：

- 场景内引用使用非零 `fileID`、空 `guid` 与 `type = NonAssetType`；
- 资产引用使用非零 `fileID`、有效资产 `guid` 与资产类型，`filePath` 只是提示。

脚本侧使用句柄而非指针。C# 侧是 `NativeObjectHandle`，Lua 侧通过 `UICanvasComponent` 的名字接口间接访问 UI 元素，原因是仓库用 `LUA_USE_C89` 编译 Lua，整数只有 32 位，64 位句柄不能跨脚本边界。

字段与语义细节见[反射与序列化](../architecture/reflection-serialization.md)。

## 帧与线程

Nullus 采用命令式多线程渲染模型，三条逻辑通道分别是 Game、Render 与 RHI：

- **Game Thread** 产生命令，不直接触碰渲染资源；
- **Render Thread** 消费命令并构建帧；
- **RHI 通道**把命令翻译成原生命令缓冲并按序提交。

判断当前所在通道用 `IsInGameThread()`、`IsInRenderingThread()`、`IsInRhiThread()`。项目设置 `rhi_thread_mode` 可选 `none`、`dedicated_thread` 与 `tasks`。完整线程模型见[任务系统与线程模型](../architecture/jobs-threading.md)。

## 与 Unity 的对照

| 目标 | Unity | Nullus |
| --- | --- | --- |
| 场景实体 | `GameObject` | `GameObject` |
| 数据单元 | `Component` / `MonoBehaviour` | `Component` / 脚本组件 |
| 资源 | `Asset` | `Asset`（带 GUID 与子资源） |
| 资源浏览器 | Project 窗口 | Asset Browser |
| 预设体 | Prefab | Prefab（含 Stage 与 Variant） |
| 材质 | Shader + Material | ShaderLab + `.mat` 材质 |
| 界面 | uGUI / UI Toolkit | UIDocument + `UICanvasComponent` |
| 脚本 | C# | C# 与 Lua |
| 序列化 | YAML | 反射驱动的对象图 JSON |

`Object`、`Component`、`Behaviour`、`GameObject`、`Transform`、`Camera`、`Light` 的脚本侧命名与 Unity 接近，生命周期回调同样叫 `Awake`、`OnEnable`、`Start`、`Update`、`OnDisable`、`OnDestroy`，降低迁移成本。C# 侧不提供 MonoBehaviour 兼容类型。

## 与 Unreal 的对照

| 目标 | Unreal | Nullus |
| --- | --- | --- |
| 场景实体 | `Actor` | `GameObject` |
| 功能单元 | `Component` | `Component` |
| 资源浏览器 | Content Browser | Asset Browser |
| 关卡 | Level | Scene |
| 蓝图类 | Blueprint | 无（用 C#/Lua 脚本组件代替） |
| 材质 | Material + Material Instance | ShaderLab + `.mat` 材质 |
| 反射 | UHT 生成 | MetaParser 生成 |
| 任务调度 | TaskGraph | TaskGraph 与 JobSystem |
| 渲染线程 | Game / Render / RHI | Game / Render / RHI |

Nullus 的渲染线程模型刻意对齐 Unreal 的分工方式，但实现是独立的，不能据此推断存在 Unreal 的 API 或编辑器行为。

## 术语表

| 术语 | 含义 |
| --- | --- |
| MetaParser | 从 C++ 声明生成反射元数据、注册代码与脚本 API 清单的工具 |
| 脚本 API 清单 | `ScriptApi.json`，C# 与 Lua 共用的成员 ID、签名与适配器来源 |
| 工件（artifact） | 导入后生成的运行时数据，带平台、编码器等构建元数据 |
| Manifest | 工件清单，描述一个资产生成了哪些运行时数据 |
| Prefab Stage | 单独打开 Prefab 进行编辑的模式 |
| Prefab Variant | 以另一个 Prefab 为基底的派生 Prefab |
| FrameGraph | 描述渲染 pass 依赖与资源状态转换的图 |
| RHI | 渲染硬件接口层，屏蔽 DX12 / Vulkan / Metal 差异 |

## 下一步

- [编辑器工作区](../editor/workspace.md)：把概念对应到实际界面。
- [游戏逻辑与脚本](../scripting/overview.md)：用脚本把这些对象动起来。
- [模块结构](../architecture/modules.md)：查看每个概念落在哪个模块。
