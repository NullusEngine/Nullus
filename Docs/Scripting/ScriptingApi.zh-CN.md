# Nullus 脚本 API v1

Nullus 的脚本 API 采用常用的组件化运行时语义，脚本运行时不使用 Mono。公共脚本基类是 `Nullus.Behaviour`，C# 和 Lua 最终调用同一套原生脚本 ABI v2。

## C# 脚本

脚本直接继承 `Behaviour`，不需要句柄构造函数。生命周期方法采用消息方法形式：它们不是基类虚方法，不需要 `new` 或 `override`，只需是无参数、返回 `void` 的实例方法；可以写成 `private`，运行时会缓存委托后调用。

```csharp
using Nullus;

public sealed class Rotator : Behaviour
{
    private void Awake() { }

    private void Update()
    {
        transform.Rotate(Vector3.up * (90.0f * Time.deltaTime));
    }

    private void OnDestroy() { }
}
```

脚本字段使用普通可序列化字段；需要显式标记时使用 `[SerializeField]`。不要声明带 `NativeObjectHandle` 参数的构造函数，也不要让脚本继承旧的托管命名空间或旧的带 `dt` 生命周期。

## Lua 脚本

Lua 文件必须返回一个模块表。生命周期同样没有参数，时间从 `Nullus.Time` 读取：

```lua
local Module = {}

function Module:Awake()
    self.angle = 0.0
end

function Module:Update()
    self.angle = (self.angle + Nullus.Time.deltaTime * 90.0) % 360.0
end

return Module
```

脚本实例上的 `self.gameObject`、`self.transform` 和组件成员使用与 C# 相同的对象图。Lua 的全局表 `Nullus` 提供 `Find`、`FindWithTag`、`Instantiate`、`Destroy`、`GetComponent`、`AddComponent`、`Time`、`Input` 和 `SceneManager`。组件查询可以传入 `"Transform"`、`"Camera"`、`"Light"`，或清单中注册的脚本类型名。

Editor 的“创建 C# 脚本”和“创建 Lua 脚本”从 `Assets/Editor/ScriptTemplates` 读取模板。C# 模板使用 `#SCRIPTNAME#` 替换为文件名对应的类型名；模板文件缺失时创建操作会失败并记录诊断。

## 类型和常用成员

| 类型 | 主要成员 |
| --- | --- |
| `Object` | `name`、`GetInstanceID()`、`Destroy(Object, delay)`、`Instantiate(GameObject)`、相等性 |
| `Component` | `gameObject`、`transform`、`tag`、`CompareTag`、`GetComponent<T>()` |
| `Behaviour` | `enabled`、`isActiveAndEnabled`、上述生命周期 |
| `GameObject` | `new GameObject(name, tag)`、`activeSelf`、`activeInHierarchy`、`tag`、`layer`、`transform`、`SetActive`、`AddComponent<T>()`、`GetComponent<T>()`、`Find`、`FindWithTag`、`CreatePrimitive` |
| `Transform` | `position`、`rotation`、`localPosition`、`localRotation`、`localScale`、`parent`、`root`、`forward` / `right` / `up`、`Translate`、`Rotate`、`LookAt`、空间转换 |
| `Camera` | `fieldOfView`、`orthographic`、`orthographicSize`、`nearClipPlane`、`farClipPlane`、`clearColor` |
| `Light` | `color`、`intensity`、`range`、`spotAngle`、`type` |

`Vector2`、`Vector3`、`Vector4`、`Quaternion`、`Color`、`Ray` 和 `Mathf` 提供小写字段、常量、运算符和常用数学函数。角度使用度数；`Quaternion` 使用 `(x, y, z, w)` 分量。

## 时间、输入和诊断

`Time` 的 `deltaTime` 受 `timeScale` 影响，`unscaledDeltaTime` 不受影响。默认 `fixedDeltaTime` 为 `0.02`，`maximumDeltaTime` 为 `0.1`；固定更新在普通更新前执行。`time`、`unscaledTime`、`frameCount` 和 `fixedFrameCount` 可用于确定性逻辑。

`Input` 提供 `GetKey`、`GetKeyDown`、`GetKeyUp`、`GetMouseButton`、`GetMouseButtonDown`、`GetMouseButtonUp`、`mousePosition` 和 `mouseScrollDelta`。键盘参数使用 `KeyCode`。

C# 使用 `Debug.Log`、`Debug.LogWarning`、`Debug.LogError`、`Debug.LogException` 和 `Debug.Assert`。Lua 运行时错误会进入脚本诊断和 Editor Console；Lua 脚本应使用 Lua 自己的 `error` 产生可定位的脚本错误。

## 生命周期和对象语义

- 新组件先创建并绑定实例，再按 `Awake`、`OnEnable`、`Start` 的顺序进入运行态。
- `enabled` 或 GameObject 的 active 状态改变时，按需要调用 `OnEnable` / `OnDisable`；`isActiveAndEnabled` 同时反映两者。
- `Destroy` 可以指定秒数延迟；销毁请求在安全的帧边界处理，回调中销毁不会使当前迭代器失效。
- `Instantiate(GameObject)` 克隆对象层级、Transform、脚本组件和已序列化字段。
- `SceneManager.LoadScene(path)` 是单活动场景的同步请求，在当前脚本帧结束后切换；`GetActiveScene()` 返回场景的 `name`、`path`、`buildIndex` 和 `isLoaded`。
- 失效或已销毁对象继续访问会抛出失效引用错误；不要缓存并复用已销毁对象。

## 清单和边界

构建时合并生成的 `Library/ScriptApi/ScriptApi.json` 是 API 清单，版本为 2；各模块的源清单位于对应的 `*/Gen/ScriptApi.json`。成员 ID、公开名称和 C#/Lua 适配器由生成器从清单产生。ABI v1 不会被运行时接受，C# 与 Lua 不应各自维护一份成员编号。

当前 v1 不包含物理、音频、协程、Renderer/Material 或资源加载 API，也不提供 MonoBehaviour 兼容类型。需要这些能力时应先扩展清单和原生适配器，再更新两种脚本语言的文档和模板。
