# Input System：Unity 使用方式对齐

2026-10-29。范围为 Action 的引用与业务脚本入口，保留统一设备采集、InputDomain/InputUser、现有 Fixed 事件缓冲、InputManager 和编辑器 Command 分发。没有另建输入运行时。

## 已实现入口

- Inspector：`[SerializeField] InputActionReference jump = new()`。选择项目资产中的动作；拖入多动作资产后继续选动作。保存定义 ID/ActionId，显示名字不承担持久化身份。缺失项保留引用，选择/清除沿用 SerializedProperty 的提交和 Undo 路径。
- 生成接口：Input Editor 的 Generate C# Class 生成相邻 `.inputactions.g.cs`。`PlayerControlsActions(player.user).Gameplay.Jump` 绑定已有玩家；Enable/Disable 委托该玩家的 Context。Input Editor 保存时刷新已生成文件。工具拒绝覆盖非本工具生成的源文件，名称冲突返回诊断。
- 名称查询：`GetComponent<PlayerInput>().actions["Gameplay/Jump"]`，以及 `FindAction(name, throwIfNotFound)`。初始化查询后缓存稳定 ID；ASCII 大小写不敏感，重名不能消歧时返回 null/抛异常。C++ 也提供按名称 FindAction/BindAction。
- 读取：`ReadValue<T>()` 支持 bool、float、Vector2、Vector3；`IsPressed`、`WasPressedThisFrame`、`WasReleasedThisFrame` 提供常用查询。原有 GetState/GetValue/BindAction 与 GUID 接口保留。
- 多玩家：同一 InputActionReference 可以 Resolve 到不同 InputUser；生成包装不创建新用户。PlayerInput 的 user 来自该原生组件，并验证仍属于当前 Domain。保留原生成身份 PlayerInputComponent 的兼容类型。
- 生命周期：动作删除后仍向旧订阅提供 DefinitionChanged/Canceled；错误资产的引用不读取新资产动作。脚本字段复用字符串 ABI、现有序列化和源生成字段访问器，未改变 NativeApiTable 布局。

使用示例见 [InputSystem.md](../InputSystem.md) 和 `Samples/Input/JumpInput.cs`、`PlayerControls.inputactions`、实际生成的 `PlayerControls.inputactions.g.cs`。

## 文件与职责

| 文件 | 变更 |
|---|---|
| Runtime/Input/InputAction.h/.cpp、InputUser.h/.cpp | 初始化时按名称、Context/Action 查询；重名消歧；按名称订阅转为稳定 ID |
| Runtime/Scripting/InputScriptApi.h/.cpp | 名称解析、限定定义的引用、UUID 字节顺序与玩家组件查询；保留删除动作的取消事件 |
| Runtime/Scripting/PlayScriptBindings.cpp | PlayerInput 的 GetComponent 解析，复用现有原生组件执行链 |
| Managed/Nullus.Managed/InputActions.cs、InputActionReference.cs、ManagedAbi.cs | C# Action 引用、用户专属 Action、类型化读取、字段编解码、PlayerInput 门面与旧类型兼容 |
| Managed/Nullus.ScriptGenerator/ScriptApiGenerator.cs、Nullus.ScriptAnalysis/NullusScriptAnalyzer.cs、ScriptAnalysisRules.cs | 将 ActionReference 识别为可序列化字段；同步分析器到现有按名称分发的私有生命周期契约，继续拒绝静态、带参数、非 void 和 async 的错误签名 |
| Project/Editor/Inputs/InputCodeGenerator.h/.cpp | 生成 C# 包装，原子更新，保护用户源文件；没有 JSON 解析进入 Input Core |
| Project/Editor/Inputs/InputActionReferencePicker.h/.cpp、Properties/InputActionReferenceDrawer.cpp | 只读枚举、选择/拖入动作、混合值与缺失引用显示；目录枚举在初始化/打开选择器时执行 |
| Inspector/ScriptSerializedAdapter.cpp、Properties/BuiltinPropertyDrawers.cpp | 使用既有 Drawer Registry 按字段类型接入选择器 |
| Panels/InputAssetEditor.cpp | 生成按钮及保存后刷新 |
| Tests/Unit/InputUnityUsageTests.cpp、Tests/Unit/CMakeLists.txt | 新增独立 NullusInputUsageTests，也由聚合源码收集包含 |
| Tests/Managed/InputUsageSmoke、Samples/Input、Docs/InputSystem.md | 实际 C# 编译/执行检查、可复制的示例和新接口说明 |

## 实际验证

独立验证目录 `build/input-unity-validation`，避免改写其他任务正在使用的主工程产物。它编译当前 Input Core、输入序列化、快捷键适配、最新 InputScriptApi 和新增生成/选择器生产源码。受影响的 5 个 Editor/Scripting 编译单元单独编译检查。

| 检查 | 结果 |
|---|---|
| NullusInputTests | 58/58 |
| NullusInputEditorTests | 62/62 |
| NullusInputPlatformTests | 13/13 |
| NullusInputUsageTests | 11/11 |
| CTest | 4/4；共 144 个 C++ 用例通过 |
| Nullus.Managed、ScriptGenerator、ScriptAnalysis、InputUsageSmoke | 使用仓库 .NET 8 SDK 实际构建 |
| InputUsageSmoke | 21 项检查通过：实际生成文件编译、UUID 字节序、玩家隔离、字段编解码、私有字段的生成 getter/setter、类型化读取及 Context 委托 |

Smoke 同时加载真实 ScriptAnalysis 与 ScriptGenerator。私有 Start/OnDestroy 按当前运行时契约通过；额外的错误签名编译样本确认静态/带参数方法仍产生 NLS101，async 方法仍产生 NLS102，记录在 `build/input-unity-analyzer-negative.log`。

原生新增回归包含名称消歧、改名后缓存与订阅、跨玩家覆盖、错误定义引用、删除动作取消、旧脚本字段序列化、生成碰撞/源文件保护、生成更新和选择器重命名兼容。

当前主产物存在并行 ECS/Physics 改动的 DLL 入口点不一致。独立测试复用匹配的既有 Engine/Scripting/ECS 依赖快照，最新 InputScriptApi 在测试内直接编译；Physics 使用已构建的新库。PE 导入/导出核验无缺失，依赖哈希保存在 `build/input-unity-dependency-manifest.json`。这不等价于最新完整 Editor/Game 链接通过。

证据：`build/input-unity-delivery-build.log`、`input-unity-final-api-build.log`、`input-unity-editor-final-build.log`、`input-unity-final-tests.log`、`input-unity-final-ctest.xml`、`input-unity-final-results/*.xml`、`input-unity-managed-smoke-build.log`、`input-unity-managed-smoke-tests.log`。独立工程与辅助生成程序只保存在 build 内，没有替换生产实现。

## 兼容与剩余验收

- 需要主工程重新配置，将新增 Editor cpp 和测试目标纳入生成工程，并重新构建 NLS_Input、NLS_Scripting、EditorProject、Editor/Game 及托管 SDK/项目脚本。当前正在运行的 Editor 不会自动获得新按钮和接口。
- 本轮没有操作主线程的可见 Editor。选择器实际点击/拖拽、Undo/Redo、保存重开，以及主工程完整 Play 集成仍需可见验收。没有声称 Windows 实机设备或 macOS/Linux 已验证。
- 输入域、Context、Fixed 语义按 Nullus 既有契约；未复制 Unity 的所有 API。Action 引用要求 Player Input 已加载对应定义，生成包装借用已有用户；它不拥有独立 Action Asset 实例。
- 当前从 Input Editor 保存自动刷新生成文件；外部修改资产后需再次生成。生成类名取文件名加 Actions，界面未增加命名空间/类名持久化选项；底层生成函数支持传入命名空间。
- 核心和编辑器编译保留既有 C4996/APIENTRY 警告；托管首次构建存在既有及新增 ModuleInitializer 的 CA2255 提示，没有为通过验收屏蔽警告或删除测试。
