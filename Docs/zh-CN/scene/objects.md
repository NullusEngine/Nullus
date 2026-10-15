# 场景、GameObject 与组件

场景是加载、保存与运行的单位；GameObject 是场景中的实体；组件是挂在 GameObject 上的功能单元。这三层关系决定了编辑器里几乎每一次操作的落点。

## 场景

`Scene` 负责：

- 持有 GameObject 列表并维护父子层级；
- 在对象加入时注册其组件，在移除时解除注册；
- 调用运行态回调 `OnAwake`、`OnStart`、`OnUpdate`；
- 在停止时按 `OnDisable`、`OnDestroy` 收尾；
- 回收已销毁对象。

创建场景对象有两个入口：`CreateGameObject` 走正常创建流程并广播创建事件；`CreateEditorTransientGameObject` 用于 Gizmo、辅助网格等只应存在于编辑器、不应被保存进场景的对象，这类对象带“编辑器临时”标记。

场景之间的切换由 `SceneManager` 管理，语义见[生命周期与激活状态](lifecycle.md)。

## GameObject

每个 GameObject 至少有：

| 成员 | 说明 |
| --- | --- |
| `name` | 显示名，可重复 |
| `tag` | 分类标签，取值来自项目设置 |
| `active` | 自身激活状态 |
| `components` | 组件列表，按添加顺序保存 |

创建 GameObject 时会自动添加一个 `TransformComponent`，并且它只能有一个：再次请求添加 Transform 会返回已有实例而不是新建。

脚本侧可用成员：

| 成员 | 说明 |
| --- | --- |
| `GetName()` / `SetName(string)` | 读写名字 |
| `GetTag()` / `SetTag(string)` | 读写标签 |
| `GetActive()` / `IsSelfActive()` | 自身激活状态 |
| `SetActive(bool)` | 切换激活状态 |
| `GetTransform()` | 取 Transform 组件 |
| `CreatePrimitive(string)` | 在当前场景创建一个图元子对象，返回对象由场景持有 |

`CreatePrimitive` 返回的对象归场景所有，脚本不要手动释放。

## 组件

引擎内置组件：

| 组件 | 作用 | 主要字段 |
| --- | --- | --- |
| `TransformComponent` | 层级变换 | 局部与全局位置、旋转、缩放 |
| `CameraComponent` | 画面输出 | `Fov`、`Near`、`Far`、`Size`、投影模式、清屏颜色 |
| `LightComponent` | 光源 | 类型、颜色、强度、`Range`、`OuterCutoff` |
| `MeshFilter` | 网格句柄 | 网格资源引用 |
| `MeshRenderer` | 网格绘制 | 材质引用与渲染开关 |
| `SkyBoxComponent` | 天空盒 | 天空盒资源 |
| `UICanvasComponent` | 屏幕或世界空间 UI | 渲染模式、排序、DPI、文档引用 |

组件的通用语义：

- **启用状态**：组件自身有 `enabled` 与 `IsActiveAndEnabled()`。只有自身启用且宿主 GameObject 在层级上处于激活状态时，才算真正生效。
- **顺序**：组件按添加顺序执行更新回调，可用 `MoveComponent` 调整。
- **销毁**：`DestroyFromOwner()` 会在对象仍激活且启用时先触发 `OnDisable`，再触发 `OnDestroy`，并且只执行一次。
- **序列化扩展**：状态无法用反射字段表达的组件可以重写 `SerializeObjectGraphProperties` / `DeserializeObjectGraphProperties`，由组件自己决定载荷格式，引擎负责图结构。

## 添加与移除组件

编辑器：在 Inspector 中通过组件选择器添加，列表顺序可在 Inspector 中调整。

脚本：C# 侧使用泛型接口，Lua 侧通过成员函数访问。可添加的组件类型由反射注册决定，未注册的类型不会出现在选择器里。

## 组件与渲染

组件改动会影响渲染状态时，通过 `MarkRenderStateChanged()` 把变更传播到宿主场景，避免组件直接依赖场景层头文件。渲染线程只接收增量命令，不读取组件对象本身。

渲染侧的持久数据与每帧载荷见[渲染概览](../rendering/overview.md)。

## 下一步

- [生命周期与激活状态](lifecycle.md)：搞清回调顺序。
- [Prefab 工作流](../prefab/workflow.md)：把对象层级变成可复用资产。
