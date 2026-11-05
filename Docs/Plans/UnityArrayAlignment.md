# 数组与 List 的 Unity 行为对照

以 Unity 6（6000.0）默认 Inspector 的 `ReorderableListWrapper` 与 `ReorderableList` 为对照。本次范围是统一属性链路中的数组、List 和固定缓冲区。

## 实现

- `Runtime/UI/Controls/CollectionControl.*`：独立 ImGui 集合控件，不依赖反射、资产数据库或撤销服务。
- `Project/Editor/Properties/CollectionPropertyDrawer.*`：字段元数据、序列化游标、剪贴板与引用拖放适配。
- `SerializedProperty`：逐目标执行排序、复制、删除和批量追加，保留各目标自身的值和未编辑分量。元素按顺序提取；字节重排按实际偏移处理字符串、嵌套数组的四字节对齐，不构造反射对象或加载引用资源。
- Inspector、文档属性面板和 C#/Lua PropertyField 均沿公共 Drawer 进入；原有 Inspector 数组绘制分支已删除。

| 项目 | 行为 |
| --- | --- |
| 布局 | 整行折叠标题，右侧数量框，带边框列表，元素标签、拖动柄、选中色及右下角加减按钮 |
| 尺寸 | 18px 标题、21px 基础行高、20px 拖动区与页脚、48px 数量框，随 DPI 与字体缩放 |
| 选择 | 单选、Ctrl 多选、Shift 范围、全选；键盘上/下循环、Home/End、左/右收起展开 |
| 结构操作 | 添加、修改数量、复制/删除选中项、浮动行预览、让位动画、拖动排序、清空；固定缓冲区禁止修改结构 |
| 编辑会话 | 数量确认/有效失焦时提交，拖动释放后一次修改；Esc 或外部版本变化取消拖动 |
| 多选 | 显示最短公共范围、不同数量显示混合符号，按目标保留各自元素值、较长尾部和未编辑分量 |
| 添加 | 按默认 ReorderableList 的 `count + 1` 设置全部目标的长度；增长复制各目标自身的末元素，空数组创建默认值 |
| 删除 | 界面删除命令直接移除所选槽位；公共 `DeleteArrayElementAtIndex` 保留既有引用先清空再删除的 API 语义 |
| 元数据 | Range、Flags、多行、Tooltip、只读及具名 Drawer 传给元素，Header/Space 只绘制一次；字段/精确数组类型注册可替换容器 |
| 剪贴板 | 单个或多个元素、整组数组的拥有字节与持久引用快照；粘贴前检查类型 |
| 引用拖放 | 资产浏览器 Ctrl/Shift 多选或层级多选可批量拖入，按类型筛选后原子追加 |
| 公共菜单 | 恢复默认、恢复继承及 Prefab 覆盖动作沿宿主执行 |
| 大集合 | 无 1024 项界面上限；结构版本缓存、可变行高及可见范围裁剪；多选上限沿 SerializedObject 配置 |
| 状态 | 按面板和属性保存折叠、选择及输入状态；普通值刷新保留布局缓存 |

Unity 6 的默认拖动在开始排序时收束为当前元素；公共排列算法同时支持批量排列，供扩展模块使用。

## 参考来源与验证边界

- https://github.com/Unity-Technologies/UnityCsReference/blob/6000.0/Editor/Mono/GUI/ReorderableList.cs
- https://github.com/Unity-Technologies/UnityCsReference/blob/6000.0/Editor/Mono/Inspector/ReorderableListWrapper.cs
- https://github.com/Unity-Technologies/UnityCsReference/blob/6000.0/Editor/Mono/SerializedProperty.bindings.cs

公开 C# 源码不包含所有原生数组行为。界面颜色依据默认明/暗主题实现，不能据此宣称逐像素一致。已在隔离项目准备 Unity 原生行为探针；本机 Unity 2022.3.62f3 因缺少有效许可证无法执行，未改动许可证设置。探针与日志位于 `build/unity-array-reference`。

自动化测试、实际 ImGui 渲染图、Editor 编译与 Unity 实机对照分别记录。三个 DPI 的离屏绘制检查不能代替实际显示器、系统缩放和操作系统输入验收。

## 本次验证

Windows Debug 的 `Editor` 构建通过；七组测试 **457/457 通过，0 跳过**。

| 目标 | 通过用例 |
| --- | ---: |
| NullusPropertyControlsTests | 29 |
| NullusInspectorPropertyTests | 121 |
| NullusUIDesignerTests | 82 |
| NullusMaterialEditorTests | 7 |
| NullusVfxAuthoringTests | 98 |
| NullusVfxEditorTests | 17 |
| NullusVfxEditorPanelTests | 103 |

新增数组/集合用例共 33 个：独立控件 15 个，属性适配与数据行为 18 个。覆盖实际鼠标/键盘输入、浮动行预览、行移动动画、离屏源行持续绘制、拖动释放一次命令、Esc、混合数量确认、Ctrl/Shift 和 Command 修饰键语义、可见行裁剪、长文本、混合长度、嵌套结构/集合、剪贴板、单条撤销/重做、失效会话、双面板状态、非对齐字符串和后续 64 位字段保持。

证据：

- `build/collection-final-editor-build.log`：Editor 与回归目标编译、部署。
- `build/collection-ctest.log`、`build/Testing/Temporary/LastTest.log`：七组完整运行结果；完整回归使用 `Tools/BuildCleanEnvironment.ps1`。最终 DPI 几何断言、混合值横线与插入点核对另外复跑了全部 29 项独立控件测试，见 `build/collection-controls-tests.log`；行坐标记录为 `build/collection-validation/*.layout.txt`。
- `build/collection-validation/{dark,light}-{100,150,200}[-drag|-mixed].png`：实际 ImGui 绘制数据生成的 18 张渲染图，包含常规、拖动和混合数量状态。测试窗口使用默认 ImGui 字体；它用于控件布局和主题检查，不代表完整 Editor 的窗口装饰与字体验收。
- 本轮没有进行 Unity 实机逐项对照，也没有完成实际显示器 DPI、macOS/Linux 与系统拖放验收。Command 行为在 ImGui 的 macOS 配置下模拟测试；它不等于 macOS 平台验收。


### 2026-10-26 原生组件回归修复

MeshRenderer 的原生 Inspector 在展开父属性后复用了父游标，使材质数组被读取为 0；原生后端又把 `root.materials` 的 `root` 当成字段，导致提交被拒绝。已按属性路径解析子游标，并让原生后端的校验、反序列化使用同一套完整 TypeTree 路径。嵌套字段也恢复读取声明字段的显示名等元数据。

新增真实 MeshRenderer 与完整 Inspector 输入回归：初始 2 个槽位可读，实际点击 `+` 后增至 3 个，后续帧保持；一条撤销恢复 2 个槽位，重做恢复 3 个。本轮 Editor Debug 构建通过，Inspector/序列化 123 项及 UI Designer 82 项共 **205/205 通过**。证据见 `build/material-array-repro.log`、`build/material-array-ctest.log`、`build/material-array-editor-build.log`。


### 2026-10-26 默认 Inspector 根节点与展开状态

默认原生组件的字段遍历曾把 TypeTree 的 `root` 容器本身作为 PropertyField，造成额外的 root 折叠行。现在跳过命名/未命名的序列化根容器，只绘制其直接字段，各 PropertyField 自己负责嵌套内容。序列化路径和脚本接口保持原样。

同一组件和后端的每帧绑定改为刷新值，避免 Reset 清空展开状态和局部结构版本；有未提交编辑时保留候选值。目标/后端变化才重新绑定。新增回归覆盖刷新后展开状态与游标、源数据变化、暂存与提交，以及默认 MeshRenderer 中窄/中/宽布局和实际折叠点击后的持续帧比较，逐帧比较布局、滚动范围、顶点、索引和裁剪矩形。

本次 Editor Debug 构建通过；Inspector/序列化 126 项、UI Designer 82 项、独立控件 29 项，共 **237/237 通过，0 跳过**。证据见 `build/array-flicker-repro.log`、`build/array-flicker-render-repro.log`、`build/array-flicker-ctest.log` 和 `build/array-flicker-editor-build.log`。这些连续帧测试验证 ImGui 输出稳定性，不代替真实 GPU 窗口的现场验收。


### 2026-10-26 User Matrix 的领域 Drawer

User Matrix 的底层状态是 Matrix4；为了保持旧 Getter/Setter 和脚本接口，反射字段仍为 16 项 Array<float>。现在以“MeshRenderer 声明类型 + root.userMatrixValues 字段路径”注册 Drawer，使用公共 4×4 Matrix 控件，固定矩阵格子，不再通过可增删列表编辑。资产格式和 Getter/Setter 不变。按格子独立写入目标游标，多选保留其余分量差异，剪贴板检查 16 个浮点的矩阵形状，保留默认/继承/Prefab 菜单。

修正公共 Matrix 控件的宽度作用域：SetNextItemWidth 只供第一项使用，后续行原先会回退到表格默认列宽；矩阵四行现统一使用宿主提供的宽度。

新增回归使用默认字段 Drawer、Editor 的 Ruda 字体和真实点击，检查展开后多帧高度、滚动范围与不同字号/窗口尺寸；多目标实际输入单格后验证其它格差异、单条撤销和重做。另验证窄宿主默认列宽下，矩阵四行仍保持布局。旧可变长数组路径的自动收起现象未能在无窗口测试中稳定复现，本次用矩阵控件替换该字段的编辑路径；实际用户 GPU 窗口验收仍需与自动化结果分开。

Editor Debug 编译通过；Inspector/序列化 128 项、独立控件 30 项、UI Designer 82 项，共 **240/240 通过，0 跳过**。证据：`build/user-matrix-fix-tests.log`、`build/user-matrix-ctest.log`、`build/user-matrix-editor-build.log`。最终部署的 Editor 与测试目录使用同一份 UI DLL。
