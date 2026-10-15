# UI Designer 与编辑器扩展

编辑器既是 UI 系统的使用者，也是它的压力测试。本页说明 UI 文档的可视化编辑入口，以及项目脚本如何扩展编辑器界面。

## UI Document Designer

### 创建一个屏幕界面

前置条件：用 `NLS_ENABLE_GAME_UI=ON` 构建编辑器，打开可运行且已经保存的场景。

1. 在 Asset Browser 的项目目录右键，选择 `Create > UI Document`，命名为 `HUD`。编辑器创建 `.ui` 文件并打开 UI Designer；也可双击文件或从 `Window > UI Designer` 找到面板。
2. 在设计器的 Hierarchy 选中根节点，在 `Controls` 点击 `Panel` 添加子节点。设 `Name` 为 `StatusPanel`、`Layout > Size` 为 `(320, 100)`，把 `Style > Background` 设为不透明颜色。
3. 在预览中确认面板可见。点击 `Save`，应显示 `Saved`；`Undo` / `Redo` 可回退或恢复设计操作，`Reload` 重新读磁盘文档。
4. 在场景中创建空对象 `HUD`，通过 Inspector 的组件添加入口加入 `UICanvasComponent`。把 `.ui` 文件拖到 `Document` 引用字段，设置 `Render Mode` 为 `Screen Overlay`。
5. 保存场景并进入 Play。预期 Game View 上出现有色面板。需要文字时添加 Text/Button，填写 `Text`，并按项目字体资源配置字体；需要脚本控制时给元素唯一名字，通过 Canvas API 查找。

设计器能编辑名称、文本、布局模式/大小/间距与背景色。完整文档 schema 的所有字段尚未全部做成控件；更多样式和资源配置仍需编辑 JSON。文档能解析只证明格式有效，字体、输入与最终画面仍需在目标后端运行检查。

编辑器提供 `UIDocumentDesigner` 面板，用于打开与编辑 UI 文档：

- 打开文档后按 `UIDocument` 的定义构建元素树；
- 选中节点后在属性区编辑布局、样式、文本与控件属性；
- 文档模型与运行时实例分离，设计期修改不会直接改动运行中的 `UIDocumentInstance`；
- 保存写回文档 JSON，刷新后重新实例化并递增代次。

设计模型的校验逻辑与运行时共用同一套解析代码，因此文档在设计器里能解析、在运行时也能解析；解析失败会给出带路径的诊断，而不是静默丢弃节点。

编辑器自身的界面覆盖层也通过同一套 UI 运行时发布：编辑器把界面绘制到独立覆盖层，再由渲染侧合成，而不是在业务代码里直接操作渲染资源。

## 脚本扩展编辑器

项目内的编辑器脚本可以注册自定义 Inspector、工具窗口与菜单项。

### 目录约定

```text
<Project>/Assets/**/Editor/**/*.cs   ->   GameScriptsEditor.dll（只在编辑器加载）
<Project>/Assets/**/*.cs             ->   GameScripts.dll（进入玩家构建）
```

编辑器脚本可以引用 `Nullus.Editor`，并继承 `ComponentEditor` 或 `EditorWindow`。

### 自定义 Inspector

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

### 自定义工具窗口

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

## 目标类型匹配规则

自定义编辑器可以针对两类目标：

| 目标 | 匹配方式 |
| --- | --- |
| 反射引擎组件 | 使用带 `NativeTypeName` 的生成类型，例如 `typeof(UICanvasComponent)` |
| 项目脚本组件 | 使用 `Nullus.Behaviour` 子类，按脚本自身类型名匹配 |

所有脚本组件在原生侧共享同一个 `ScriptComponent` 反射类型，因此脚本组件的编辑器按脚本自身的类型名匹配（C# 全名）。一个脚本的编辑器不会影响其他脚本组件。脚本组件之间暂不支持基类继承匹配。

## 数据访问约束

脚本不能直接写组件对象。所有读写都经过 `serializedObject` / `SerializedProperty`，提交时走 `ApplyModifiedProperties()`，从而共享同一套撤销、Prefab Override 与多对象编辑语义。

## 可用控件

`EditorGUILayout` 当前提供：

- `Label`、`Space`
- `Toggle`、`IntField`、`FloatField`、`Slider`
- `TextField`、`Button`、`HelpBox`
- `PropertyField`
- `BeginHorizontal` / `EndHorizontal`、`BeginVertical` / `EndVertical`

## 注册与优先级

- 同名目标的脚本编辑器默认优先级高于内置编辑器，因此可以覆盖内置实现；
- 重复的（目标类型、继承规则、优先级）组合会直接报错；
- 菜单项只能挂在 `Window` 与 `Tools` 两个顶层菜单下。

## 错误隔离

脚本或编辑器抛出的异常会被捕获：该 Inspector 回退为通用绘制并显示错误；连续失败超过阈值后停用该脚本编辑器，不影响其他面板。脚本重新编译后会自动重扫注册表并重建已打开的编辑器与窗口实例。

## 当前边界

- v1 不开放脚本 `CustomPropertyDrawer` 注册；
- 不提供任意 ImGui 直通，控件集合固定为上面列出的受控 API。

需要更强的扩展能力时，先扩展受控控件集合与注册模型，再更新本文档。

## 下一步

- [编辑器脚本扩展](../architecture/editor-extensions.md)：与模块结构相关的说明。
- [UI 系统概览](overview.md)：回到运行时语义。
