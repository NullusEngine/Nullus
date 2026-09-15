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

脚本实例上的 `self.gameObject`、`self.transform` 和组件成员使用与 C# 相同的对象图。Lua 的全局表 `Nullus` 提供 `Find`、`FindWithTag`、`Instantiate`、`Destroy`、`GetComponent`、`AddComponent`、`Time`、`Input` 和 `SceneManager`。组件查询可以传入 `"Transform"`、`"Camera"`、`"Light"`、`"UICanvas"`，或清单中注册的脚本类型名。

需要一套可运行的自检界面时，把 `TestProject/Assets/ScriptTestPanel.ui` 指到 UI Canvas，并把 `TestProject/Assets/UiScriptTest.lua` 挂到同一个 GameObject：脚本会按名字更新计数文本、进度条和开关。`NullusUnitTests` 里的 `ScriptRuntimeTests.LuaUiScriptDrivesTheProjectUiCanvasDocument` 用内置夹具覆盖同一套按名字驱动的 Canvas 脚本流程，因此在没有本地 TestProject 目录的 CI 检出中也能运行。

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
| `UICanvas` | `SetText(name, text)`、`GetText(name)`、`SetValue(name, value)`、`GetValue(name)`、`SetChecked(name, checked)`、`GetChecked(name)`、`Focus(name)`、`GetDocumentGeneration()` |

UI 元素按**名字**寻址。原生元素句柄是带文档 generation 的 64 位值，而本仓库用 `LUA_USE_C89` 编译 Lua，Lua 整数只有 32 位，因此句柄不跨脚本边界；UI Canvas 的成员方法在内部完成名字到句柄的解析。

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

## 用脚本编写编辑器界面

项目里的 `Assets/**/Editor/**/*.cs` 会编译成独立的 `GameScriptsEditor.dll`，只被编辑器加载，不会进入玩家构建；其余 `.cs` 仍然进入 `GameScripts.dll`。编辑器脚本可以引用 `Nullus.Editor`，用声明式方式扩展现有编辑器界面。

自定义组件 Inspector：

```csharp
using Nullus;
using Nullus.Editor;

namespace Nullus.GameScriptsEditor;

[CustomEditor(typeof(UICanvasComponent))]
[CanEditMultipleObjects]
public sealed class UiCanvasInspector : ComponentEditor
{
    public override void OnInspectorGUI()
    {
        EditorGUILayout.HelpBox("脚本绘制", MessageType.Info);
        EditorGUILayout.PropertyField(serializedObject.FindProperty("document"));

        if (EditorGUILayout.Button("重置像素比"))
        {
            var pixelsPerUnit = serializedObject.FindProperty("pixelsPerUnit");
            if (pixelsPerUnit is not null)
            {
                pixelsPerUnit.floatValue = 100.0f;
                ApplyModifiedProperties();
            }
        }
    }
}
```

自定义工具窗口：

```csharp
using Nullus.Editor;

namespace Nullus.GameScriptsEditor;

public sealed class AssetAuditWindow : EditorWindow
{
    [EditorMenuItem("Window/Asset Audit")]
    public static void ShowWindow()
    {
        var window = GetWindow<AssetAuditWindow>() ?? new AssetAuditWindow();
        window.Open();
    }

    public override void OnGUI()
    {
        EditorGUILayout.Label("Hello from a project editor script");
    }
}
```

约束与语义：

- 目标类型可以是两种：有 `NativeTypeName` 的反射引擎组件（生成器会为每个原生组件产出这样的 C# 类型，因此 `typeof(UICanvasComponent)` 始终可用），或项目里的脚本组件（`Nullus.Behaviour` 子类，例如 `[CustomEditor(typeof(RotatingCube))]`）。
- 所有脚本组件在原生侧共享同一个 `ScriptComponent` 反射类型，因此脚本组件的编辑器按**脚本自身的类型名**匹配（C# 全名）；一个脚本的编辑器不会影响其他脚本组件。脚本组件之间暂不支持 `editorForChildClasses` 的基类匹配。
- 脚本不能直接写组件对象。所有取值/写值都通过 `serializedObject` / `SerializedProperty`，提交时走 `ApplyModifiedProperties()`，共享同一套 Undo、Prefab override 和多对象语义。
- `EditorGUILayout` 提供 `Label`、`Space`、`Toggle`、`IntField`、`FloatField`、`Slider`、`TextField`、`Button`、`HelpBox`、`PropertyField` 以及 `BeginHorizontal/EndHorizontal`、`BeginVertical/EndVertical`。
- 脚本编辑器默认优先级高于内置编辑器，因此同名目标会覆盖内置实现；重复的 `(目标类型, 继承规则, 优先级)` 会直接报错。
- 菜单项只能挂在 `Window` 和 `Tools` 两个顶层菜单下。
- 脚本或编辑器抛出的异常会被捕获：该 Inspector 回退为通用绘制并显示错误，连续失败超过阈值后停用该脚本编辑器，不会影响其他面板。
- 脚本重新编译后会自动重扫注册表并重建已打开的编辑器与窗口实例。

当前 v1 不开放脚本 `CustomPropertyDrawer` 注册，也不提供任意 ImGui 直通：控件集合固定为上面列出的受控 API。

## 清单和边界

构建时合并生成的 `Library/ScriptApi/ScriptApi.json` 是 API 清单，版本为 2；各模块的源清单位于对应的 `*/Gen/ScriptApi.json`。成员 ID、公开名称和 C#/Lua 适配器由生成器从清单产生。ABI v1 不会被运行时接受，C# 与 Lua 不应各自维护一份成员编号。

当前 v1 不包含物理、音频、协程、Renderer/Material 或资源加载 API，也不提供 MonoBehaviour 兼容类型。需要这些能力时应先扩展清单和原生适配器，再更新两种脚本语言的文档和模板。
