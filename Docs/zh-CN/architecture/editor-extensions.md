# 编辑器脚本扩展

编辑器不是封闭程序：项目可以在自己的 `Assets` 目录里写编辑器脚本，注册自定义 Inspector、工具窗口与菜单项。本页说明扩展的边界与接入方式。

## 程序集划分

| 源文件位置 | 产物 | 加载范围 |
| --- | --- | --- |
| `Assets/**/Editor/**/*.cs` | `GameScriptsEditor.dll` | 仅编辑器进程 |
| `Assets/**/*.cs`（其余） | `GameScripts.dll` | 编辑器与玩家构建 |
| `Managed/Nullus.Editor` | 编辑器脚本 API | 编辑器 |

编辑器脚本可以引用 `Nullus.Editor`，项目运行时代码不能。

## 可扩展点

| 扩展点 | 基类或特性 |
| --- | --- |
| 自定义 Inspector | `ComponentEditor` + `[CustomEditor]` + `[CanEditMultipleObjects]` |
| 自定义工具窗口 | `EditorWindow` + `[EditorMenuItem]` |
| 菜单项 | `[EditorMenuItem]`，顶层只能是 `Window` 或 `Tools` |

菜单项最终出现在编辑器顶部菜单的对应位置，与内置面板并列。

## 编辑器侧数据通道

脚本不直接写组件对象。读写统一经过：

```text
serializedObject -> SerializedProperty -> ApplyModifiedProperties()
```

这条路径让脚本编辑器与内置编辑器共享：

- 撤销栈；
- Prefab Override 记录；
- 多对象编辑语义；
- 字段类型转换与校验。

绕过它会破坏撤销与 Prefab 语义，属于不支持用法。

## 目标类型

| 目标 | 声明方式 |
| --- | --- |
| 反射引擎组件 | 使用带 `NativeTypeName` 的生成类型 |
| 项目脚本组件 | 使用 `Nullus.Behaviour` 子类 |

脚本组件在原生侧共享同一个反射类型，因此按脚本自身的 C# 全名匹配。一个脚本的编辑器不会影响其他脚本组件，也不支持按基类匹配子类。

## 优先级与冲突

- 脚本编辑器默认优先级高于内置编辑器，可以覆盖内置实现；
- 重复的（目标类型、继承规则、优先级）组合在注册时直接报错，不做静默去重。

## 错误隔离

脚本或编辑器抛出的异常被捕获：

- 该 Inspector 回退为通用绘制并显示错误；
- 连续失败超过阈值后停用该脚本编辑器；
- 其他面板不受影响；
- 脚本重新编译后自动重扫注册表并重建已打开的编辑器与窗口实例。

## 受控控件集合

`EditorGUILayout` 提供 `Label`、`Space`、`Toggle`、`IntField`、`FloatField`、`Slider`、`TextField`、`Button`、`HelpBox`、`PropertyField`，以及 `BeginHorizontal` / `EndHorizontal`、`BeginVertical` / `EndVertical`。

## 当前边界

- 不开放脚本 `CustomPropertyDrawer` 注册；
- 不提供任意 ImGui 直通调用；
- 扩展点限于 Inspector、工具窗口与菜单项。

## 与原生编辑器代码的关系

内置面板（Hierarchy、Inspector、Asset Browser、Console、Profiler、Project Settings 等）由 C++ 编辑器实现，通过同一套面板与命令系统注册。脚本扩展与内置面板在菜单与面板管理上等价，但脚本侧只拿到受控 API。

面板自身可以按需开关。检查面板注册情况的最快方式是打开 `Window` 菜单，看目标项是否出现。

## 扩展开发建议

1. 先在 `Assets/**/Editor/` 下写最小 Inspector 验证注册链路；
2. 通过 `serializedObject` 访问字段，不要缓存组件对象；
3. 对多对象编辑使用 `[CanEditMultipleObjects]` 并在实现中避免假设单一目标；
4. 需要新的控件类型时先扩展受控 API，再更新本文档。

## 下一步

- [UI Designer 与编辑器扩展](../ui/designer.md)：完整示例代码。
- [模块结构](modules.md)：程序集与工具的位置。
