# 可复用反射 Inspector 宿主层

状态：已实现（2026-10-20）。本文记录编辑器反射属性栈的分层、宿主桥接 API 与后续缺口。

2026-10-25：公共入口已扩展为 `Properties::PropertyPanel`，独立值控件位于 `UI::Controls`。注册、事务和迁移说明见 [统一属性系统](UnifiedProperties.md)。本文中的 `ReflectedPropertyEditor` API 继续作为兼容入口。
相关文件：

- `Project/Editor/Inspector/ReflectedPropertyEditor.{h,cpp}` — 单值/结构体反射编辑
- `Project/Editor/Inspector/ReflectedEditHost.h` — 新增：宿主事务桥接
- `Project/Editor/Inspector/EditorReflection.h` — 新增：声明式字段元数据
- `Project/Editor/Inspector/Serialization/*`、`InspectorView`、`InspectorProjectionBuilder` — 既有核心
- 使用方：场景 Inspector、VFX Editor、VFX Library Editor、UI Designer

## 分层

| 层 | 组件 | 职责 |
| --- | --- | --- |
| 值/目标模型 | `SerializedObject` / `SerializedProperty` / `ISerializedTargetBackend` | TypeTree、字节流、多目标、撤销边界 |
| 绘制引擎 | `InspectorView::DrawSerializedProperties` | 按 `PropertyNode` 生成控件 |
| 显示投影 | `InspectorProjectionBuilder` | 从 `SerializedProperty` 生成显示树 |
| 元数据 | `InspectorMetadataRegistry` + `EditorReflection.h` | 显示名、范围、多行、Tooltip |
| 自定义抽屉 | `InspectorRegistry::RegisterPropertyDrawer` | 按字段/类型/元数据/兜底 |
| 宿主 API | `ReflectedPropertyEditor` + `ReflectedValueBackend` | 编辑一份反射值，返回 `changed/completed/canceled` + 路径 |
| 事务桥接 | `IReflectedEditHost` + `ApplyReflectedEdit` | 把编辑信号映射为文档的 Begin/Preview/Commit/Cancel |

## 宿主桥接

```cpp
class IReflectedEditHost
{
public:
    virtual bool BeginPropertyEdit(std::string_view label) = 0;
    virtual bool PreviewPropertyEdit() = 0;
    virtual bool CommitPropertyEdit() = 0;
    virtual bool CancelPropertyEdit() = 0;
    virtual bool IsPropertyEditOpen() const = 0;
    virtual std::string_view GetPropertyEditError() const { return {}; }
};
```

两种用法：

1. **自绘 + 候选值校验**（VFX）：先 `Draw`，把变化写入候选值，再调用
   `ApplyReflectedEdit(host, edit, label)`。宿主在 `PreviewPropertyEdit` 里发布候选。
2. **整体绘制**（UI Designer）：`DrawReflectedValue(editor, host, id, label, value, options, apply)`；
   首次 `changed` 时打开宿主事务，`apply(value, paths)` 写入文档，`completed` 提交，`canceled` 回滚。
3. **多值/混合值**：`DrawReflectedValues(editor, host, id, label, values, options, apply)`。
   `values` 作为多个 `SerializedObject` target 交给共享 Inspector：相同的字段正常显示，不同的字段
   显示 Mixed（`—`）；编辑一个字段会把同一个值写入所有 target。`apply` 收到主 target 的新值与变化路径，
   文档宿主按现有语义写入所有选中对象。

   调用方必须在 `id` 中包含**选择集身份**（例如 GUID 列表）。编辑器只比较主 target 的源值来避免拖动
   中途被宿主写回的其他 target 重置；选择集变化换用新 `id` 可以保证旧 target 值不会被复用。

帧末统一调用 `FinishReflectedEdits(editor, host)`：刷掉编辑器内部待提交值，并在没有 ImGui 项/弹窗
持有交互时提交通知。连续拖动因此仍然只产生一条文档撤销。

## 声明式元数据

```cpp
Inspector::RegisterEditorStructMetadata(NLS_TYPEOF(MyEditorStruct), {
    { "mode", "Mode" },
    { "opacity", "Opacity", "", false, 0.0f, 1.0f },
    { "text", "Text", "", true }
});
```

字段类型/成员指针仍由 `NLS_META_EXTERNAL_FIELD` 注册（编译期需要），显示名/范围/多行由这一张表声明，
避免每个字段写一次 `Register`。

## 已迁移的宿主

| 宿主 | 文档事务 | 状态 |
| --- | --- | --- |
| UI Designer | `UIDocumentDesignerModel::Begin/End/CancelTransaction` | 已迁移，`UIDocumentInspectorView` 实现 `IReflectedEditHost` |
| VFX Editor | `VfxDocument::BeginEdit/PreviewEdit/EndEdit` | 已迁移，`VfxPropertyEditHost` + `ApplyReflectedEdit` |
| VFX Library Editor | `VfxLibraryDocument` 同名接口 | 已迁移 |
| 场景 Inspector | 原生 `SerializedObject` target/undo | 原生路径，不使用宿主桥接 |

## 新增编辑器接入步骤

1. 定义编辑器侧 `STRUCT`（或外部反射结构）与字段；
2. 用 `NLS_META_EXTERNAL_*` 注册类型/字段，用 `RegisterEditorStructMetadata` 声明显示名/范围；
3. 构造 `ReflectedPropertyEditor`，设置生命周期 token；
4. 实现 `IReflectedEditHost`（映射到自己的文档事务）；
5. 用 `DrawReflectedValue` / `ApplyReflectedEdit` 绘制与提交，帧末 `FinishReflectedEdits`；
6. 补单测：字段路径覆盖、写回/撤销、`ApplyReflectedEdit` 状态机。

## 验证

- `NullusUIDesignerTests` 53/53：模型 19、反射 Inspector 9（含声明式元数据）、宿主桥接 9、
  多值/混合值 4、画布几何 9、画布拖拽 1、面板 2。
- `ReflectedEditHostTests.cpp` 覆盖：changed/completed/canceled、连续编辑复用、Begin/Preview 失败、
  无信号不触碰宿主。
- `ReflectedValueBackendTests.cpp` 覆盖：多 target 混合字段检测、一次编辑写入所有 target、
  `SetSources` 刷新混合状态、单值兼容、混合值绘制不产生编辑信号。

## 剩余缺口

| 缺口 | 影响 | 建议 |
| --- | --- | --- |
| 同一字段只能写入同一个值 | 多选不支持为不同 target 输入不同值（与主流编辑器一致） | 保持 |
| 选择集身份需由调用方放进 `id` | 调用方忘记时可能复用旧 target 值 | 宿主层可在后续提供 `id + selection revision` 辅助 |
| JSON 文档适配手写 | 每个 JSON 资产类型重复 `Read/Apply` | 抽 `JsonReflectedDocument<T>`（字段表驱动） |
| 区块 chrome 各自实现 | CollapsingHeader/按钮样式重复 | 抽 `ReflectedSection` 小工具 |
| 自定义抽屉仅场景路径 | 文档编辑器尚未接入脚本抽屉 | 宿主桥接已具备接入点，按需扩展 |
