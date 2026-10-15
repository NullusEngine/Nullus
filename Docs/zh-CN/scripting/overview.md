# 脚本系统概览

Nullus 提供 C# 与 Lua 两套脚本后端。两者不是两套对象模型，而是同一套原生运行时之上的两个语言前端：脚本组件类型、实例句柄、字段访问与生命周期都走同一条 ABI 与同一份 API 清单。

## 架构

```text
原生场景对象
    |
ScriptRuntime（语言无关）
    |-- IScriptBackend -> CoreCLR（C#）
    |-- IScriptBackend -> Lua 5.4（Lua）
    |
ScriptApiDatabase（ScriptApi.json）
    |
生成强类型 C# 绑定 / Lua 反射绑定
```

关键约束：

- 只有一个语言无关的脚本组件类型，C# 与 Lua 不是两种组件；
- `ScriptApiDatabase` 是原生反射与脚本绑定之间唯一的可移植契约；
- 成员 ID、公开名称与两种语言的适配器由生成器从清单产出，不各自维护一份编号；
- 原生 ABI 版本不匹配时运行时拒绝加载，而不是尝试兼容。

## 脚本资产

把 `.cs` 或 `.lua` 放进项目 `Assets/` 目录即可参与资产管线：

- `.cs` 保存后触发增量构建，成功后热重载；
- `.lua` 保存后重载即生效，没有编译步骤；
- 两者都会出现在 Asset Browser 中，并有各自的分类图标。

## 生命周期回调

两种语言使用同一组回调名：

| 回调 | 触发时机 |
| --- | --- |
| `Awake` | 对象首次进入运行态，一次 |
| `Start` | 首次进入运行态且在 `Awake` 之后，一次 |
| `OnEnable` | 每次从停用变为启用 |
| `OnDisable` | 每次从启用变为停用 |
| `Update` | 每帧 |
| `FixedUpdate` | 固定步长帧 |
| `LateUpdate` | 每帧、在 `Update` 之后 |
| `OnDestroy` | 销毁时，一次 |

C# 侧的回调必须是实例方法、无参数、返回 `void`，名称完全匹配；静态方法或有参数的同名方法不会被识别。

原生侧回调顺序与激活门控见[生命周期与激活状态](../scene/lifecycle.md)。

## C# 与 Lua 的分工

| 维度 | C# | Lua |
| --- | --- | --- |
| 运行时 | CoreCLR | Lua 5.4 |
| 绑定方式 | 生成强类型绑定 | 反射式惰性绑定 |
| 脚本形态 | 派生 `Nullus.Behaviour` 的类 | 返回 module 表 |
| 迭代方式 | 保存后增量构建并热重载 | 保存后重载即生效 |
| 调试 | 断点、附加进程、混合调试 | 可选 LuaPanda 集成 |
| 整数宽度 | 64 位 | 32 位（`LUA_USE_C89`） |

因为 Lua 侧整数只有 32 位，64 位对象句柄不跨脚本边界。UI 元素按名字寻址，`UICanvasComponent` 在内部完成名字到句柄的解析。

## C# 脚本形态

```csharp
using Nullus;

public sealed class ExampleBehaviour : Behaviour
{
    private void Awake()
    {
        Debug.Log($"{name} is ready");
    }

    private void Update()
    {
        transform.Rotate(Vector3.up * (30.0f * Time.deltaTime));
    }
}
```

`Behaviour` 继承自 `Component`，因此可以直接访问 `gameObject`、`transform`、`tag`、`enabled` 与 `isActiveAndEnabled`。

## Lua 脚本形态

```lua
local Module = {}

function Module:Awake()
    Nullus.Debug.Log("ready")
end

function Module:Update()
    local position = self.transform.localPosition
    position.x = position.x + Nullus.Time.deltaTime
    self.transform.localPosition = position
end

return Module
```

脚本返回一张表，生命周期函数是表上的成员，调用时以表自身作为 `self`。引擎 API 通过全局 `Nullus` 表暴露，例如 `Nullus.Time` 与 `Nullus.Debug`。

## 脚本与场景的绑定

脚本组件挂在 GameObject 上，其可序列化字段参与对象图序列化。C# 侧的字段清单由生成器写入脚本清单，编辑器据此在 Inspector 中绘制可编辑项，并在保存场景时写入字段值。

字段在热重载时尽可能保留：成功构建后编辑器在帧边界切换新的程序集并重新绑定，已存在的实例只经历 `OnDisable` 与 `OnEnable` 环绕的后端替换，不会补造 `Awake`、`Start` 或 `OnDestroy`。

## 脚本与编辑器

`Assets/**/Editor/**/*.cs` 属于编辑器程序集，只在编辑器进程中加载，可以引用 `Nullus.Editor` 注册自定义 Inspector、工具窗口与菜单项。玩家构建不包含这些脚本。写法见[编辑器脚本扩展](../architecture/editor-extensions.md)。

## 当前边界

- 没有 `MonoBehaviour` 兼容类型；
- 不提供协程；
- 脚本 API 不覆盖物理、音频与 Renderer/Material；
- 不提供资源加载 API。

需要这些能力时先扩展 API 清单与原生适配器，再更新两种语言的文档与模板，顺序不能颠倒。详细清单见[脚本 API 参考](api.md)与[功能状态矩阵](../roadmap/index.md)。

## 下一步

- [脚本 API 参考](api.md)：可用的类型与成员。
- [脚本调试与热重载](debugging.md)：断点、附加与故障排查。
