# 脚本 API 参考

本页说明公开脚本接口。共享的原生 API 元数据位于 `Library/ScriptApi/ScriptApi.json`，当前 schema 版本为 `2`；C# 将其投影为托管类型，Lua 则安装自己的运行时表和 userdata 适配器。存在元数据不代表两种语言的 API 完全一致。

## 语言范围与完整签名

下方成员表主要描述 C#。签名与可空性以 [GeneratedBindings.cs](../../../Managed/Nullus.Managed/GeneratedBindings.cs) 为准；组件、场景、输入和时间封装还位于 [Behaviour.cs](../../../Managed/Nullus.Managed/Behaviour.cs)。以下是部分完整 C# 签名：

| 签名 | 关键语义 |
| --- | --- |
| `public static void Destroy(Object? target, float delay = 0.0f)` | null 或已销毁目标无操作；托管包装立即失效 |
| `public static GameObject Instantiate(GameObject original)` | 原对象必须非 null 且仍存活；克隆对象层级 |
| `public T? AddComponent<T>() where T : Component` | GameObject 实例方法；检查可空返回值 |
| `public T? GetComponent<T>() where T : Component` | GameObject 实例方法；不存在时返回 null |
| `public static GameObject? Find(string objectName)` | 找不到名称时返回 null |
| `public void Translate(Vector3 translation, Space relativeTo)` | 单参数重载默认 `Space.Self` |
| `public void Rotate(Vector3 eulerAngles, Space relativeTo)` | 角度单位为度；单参数重载默认 `Space.Self` |
| `public Vector3 InverseTransformPoint(Vector3 point)` | 逆变换位置、旋转和缩放；零缩放分量返回零 |

Lua 的实际适配器位于 [LuaScriptBackend.cpp](../../../Runtime/Scripting/LuaScriptBackend.cpp)。使用 `Nullus.Find(name)`、`object:GetComponent(typeName)` 与 `object:CreatePrimitive("Cube")`；Lua 没有 C# 泛型语法或 `new GameObject(...)` 构造器。Lua 向量是带 `x/y/z` 字段的表，修改 Transform 时应读取、修改并赋回属性。`Transform.Rotate` 等托管便利方法不会自动安装到 Lua userdata 上。

UI 在 C# 中使用下方导出的元素句柄成员。Lua 用元素名调用 `canvas:SetText(name, text)`、`GetText(name)`、`SetValue(name, value)`、`GetValue(name)`、`SetChecked(name, checked)`、`GetChecked(name)` 与 `Focus(name)`，也提供 `GetDocumentGeneration()`。不要向 Lua 传 C# 元素句柄，或假设整张 C# 配置成员表都可在 Lua 中调用。

## Object

所有引擎对象的基类。

| 成员 | 说明 |
| --- | --- |
| `name` | 对象名 |
| `GetInstanceID()` | 实例 ID |
| `Destroy(Object, delay = 0)` | 请求销毁，可延迟秒数 |
| `Instantiate(GameObject)` | 克隆对象层级 |
| 相等性比较 | 按句柄比较，已销毁对象视为空 |

失效或已销毁对象继续访问会抛出失效引用错误，不要缓存并复用已销毁对象。

## Component

| 成员 | 说明 |
| --- | --- |
| `gameObject` | 宿主对象 |
| `transform` | 宿主对象的 Transform |
| `tag` | 宿主对象标签 |
| `CompareTag(string)` | 标签比较 |
| `GetComponent<T>()` | 取同对象上的组件 |

## Behaviour

脚本组件基类。

| 成员 | 说明 |
| --- | --- |
| `enabled` | 组件自身启用状态 |
| `isActiveAndEnabled` | 自身启用且宿主在层级上激活 |

## GameObject

| 成员 | 说明 |
| --- | --- |
| `GameObject(string name = "New GameObject", string tag = "")` | 新建对象 |
| `name` | 读写名字 |
| `tag` | 读写标签 |
| `layer` | 读写层 |
| `activeSelf` | 自身激活状态 |
| `activeInHierarchy` | 层级激活状态 |
| `transform` | Transform 组件 |
| `SetActive(bool)` | 切换激活状态 |
| `AddComponent<T>()` | 添加组件 |
| `GetComponent<T>()` | 取组件 |
| `GameObject.Find(string)` | 按名字查找 |
| `GameObject.FindWithTag(string)` | 按标签查找 |
| `GameObject CreatePrimitive(string primitiveType = "Cube")` | 实例方法；在调用对象的场景中创建图元，并把调用对象设为父对象 |

```csharp
var root = new GameObject("Example Root");
GameObject cube = root.CreatePrimitive("Cube");
cube.transform.localPosition = new Vector3(0.0f, 0.0f, 5.0f);
```

这里的 `CreatePrimitive` 通过实例调用，例如 `root.CreatePrimitive("Cube")`。签名来自[当前托管绑定](../../../Managed/Nullus.Managed/GeneratedBindings.cs)，不是 Unity 的静态工厂。调用对象必须属于有效场景；不支持的图元类型或失效对象会导致创建失败。

## Transform

| 成员 | 说明 |
| --- | --- |
| `position` / `rotation` | 世界空间位置与旋转 |
| `localPosition` / `localRotation` | 局部空间位置与旋转 |
| `eulerAngles` / `localEulerAngles` | 欧拉角读写 |
| `localScale` / `lossyScale` | 缩放 |
| `parent` / `root` | 父子关系 |
| `forward` / `right` / `up` | 朝向向量 |
| `Translate(Vector3)` | 平移，默认相对自身 |
| `Rotate(Vector3 eulerAngles)` | 旋转，默认相对自身 |
| `LookAt(Vector3)` / `LookAt(Transform)` | 朝向目标 |
| `TransformPoint` / `InverseTransformPoint` | 点空间转换 |
| `TransformDirection` / `InverseTransformDirection` | 方向空间转换 |

角度使用度数；`Quaternion` 使用 `(x, y, z, w)` 分量。

## Camera

| 成员 | 说明 |
| --- | --- |
| `fieldOfView` | 透视视场角 |
| `orthographic` | 是否正交投影 |
| `orthographicSize` | 正交尺寸 |
| `nearClipPlane` / `farClipPlane` | 裁剪面 |
| `clearColor` | 清屏颜色 |

## Light

| 成员 | 说明 |
| --- | --- |
| `color` | 颜色 |
| `intensity` | 强度 |
| `range` | 影响范围 |
| `spotAngle` | 外锥角 |
| `type` | 光源类型 |

## UICanvasComponent

UI 元素按名字寻址。当前脚本成员：

| 成员 | 说明 |
| --- | --- |
| `LoadDocumentScript(string)` | 加载 UI 文档 |
| `GetDocumentGeneration()` | 取文档代次 |
| `GetElementHandle(string)` | 由名字取元素句柄 |
| `GetElementText(h)` / `SetElementText(h, string)` | 读写文本 |
| `GetElementValue(h)` / `SetElementValue(h, float)` | 读写数值 |
| `GetElementChecked(h)` / `SetElementChecked(h, bool)` | 读写勾选状态 |
| `FocusElement(h)` | 设置焦点 |
| `GetRenderModeValue()` / `SetRenderModeValue(uint)` | 渲染模式 |
| `GetWorldRenderPathValue()` / `SetWorldRenderPathValue(uint)` | 世界空间渲染路径 |
| `GetSortOrderScript()` / `SetSortOrderScript(int)` | 排序序号 |
| `GetSortLayer()` / `SetSortLayer(int)` | 排序层 |
| `GetCameraId()` / `SetCameraId(uint64)` | 关联相机 |
| `GetDpiMatch()` / `SetDpiMatch(float)` | DPI 匹配 |
| `GetPixelsPerUnit()` / `SetPixelsPerUnit(float)` | 像素密度 |
| `GetDepthTest()` / `SetDepthTest(bool)` | 深度测试 |
| `GetDepthWrite()` / `SetDepthWrite(bool)` | 深度写入 |
| `GetAlwaysOnTop()` / `SetAlwaysOnTop(bool)` | 置顶显示 |

## Time

| 成员 | 说明 |
| --- | --- |
| `deltaTime` | 受 `timeScale` 影响 |
| `unscaledDeltaTime` | 不受 `timeScale` 影响 |
| `time` / `unscaledTime` | 累计时间 |
| `frameCount` / `fixedFrameCount` | 帧计数 |
| `timeScale` | 时间缩放 |
| `fixedDeltaTime` | 固定步长，默认 `0.02` |
| `maximumDeltaTime` | 单帧最大步长，默认 `0.1` |

固定更新在普通更新之前执行。

## SceneManager 与 Scene

| 成员 | 说明 |
| --- | --- |
| `SceneManager.GetActiveScene()` | 取活动场景 |
| `SceneManager.LoadScene(string path)` | 请求加载场景 |
| `Scene.name` / `path` / `buildIndex` / `isLoaded` | 场景信息 |
| `Scene.IsValid()` | 是否已加载 |

`LoadScene` 是单活动场景的同步请求，在当前脚本帧结束后切换。

## Input

| 成员 | 说明 |
| --- | --- |
| `GetKey(KeyCode)` / `GetKeyDown` / `GetKeyUp` | 键盘 |
| `GetMouseButton(int)` / `GetMouseButtonDown` / `GetMouseButtonUp` | 鼠标按键 |
| `mousePosition` | 鼠标位置 |
| `mouseScrollDelta` | 滚轮增量 |

## Debug

| 成员 | 说明 |
| --- | --- |
| `Log` / `LogWarning` / `LogError` | 写日志 |
| `LogException` | 记录异常 |
| `Assert(bool, message)` | 断言失败时记录错误 |

Lua 运行时错误会进入脚本诊断与编辑器 Console。Lua 脚本用语言自身的 `error` 产生可定位错误。

## 数学类型

`Vector2`、`Vector3`、`Vector4`、`Quaternion`、`Color`、`Rect`、`Vector2Int`、`Vector3Int`、`RectInt`、`Bounds`、`BoundsInt`、`Ray` 与 `Mathf` 提供小写字段、常量、运算符与常用函数。

## 清单与边界

- `Library/ScriptApi/ScriptApi.json` 是合并后的 API 清单，schema 版本 `2`；
- 各模块的源清单位于对应的 `*/Gen/ScriptApi.json`；
- 不支持的可移植签名在构建期被拒绝，不会被静默跳过。

当前清单只包含引擎侧反射类型。脚本自定义类型间的互引用、协程、资源加载、物理与音频 API 尚未纳入。

## 下一步

- [UI 系统概览](../ui/overview.md)：`UICanvasComponent` 的完整语义。
- [反射与序列化](../architecture/reflection-serialization.md)：清单是如何生成的。
