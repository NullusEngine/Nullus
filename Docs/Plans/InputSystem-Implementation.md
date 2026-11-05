# Nullus Input System 实施与验收记录

更新：2026-10-30。本文件对应已批准的五阶段统一输入与编辑器 Command 迁移计划。
源码已接入 Editor 和 Game。已恢复主工程集成、处理保存及 Scene 右键首次输入丢失的可复现分支，完成 Release 核心性能检查及 Windows Debug 全量回归执行。以下区分实际通过、历史里程碑和仍开放的失败；尚未达到五阶段全部验收通过。
仓库原有的大量未提交修改保持原状；本文件列出的输入改动不代表整个工作区 diff。

最新全量结果见 `Docs/Reports/InputSystem-FullRegression-2026-10-30.md`：
52 个非聚合测试目标及 Editor/聚合目标构建成功；输入专项 199/199。
56 组 CTest 原始 49 通过、7 失败，其中 3 个目标修复后正式复验通过，
仍有 Unit、SpriteImport、VfxEditor、ECSPlayer 四个目标开放。
原库存和并行新增共 5,102 个用例均有执行记录；独立补跑不能替代一次
不受 JobSystem 全局状态污染的完整聚合运行。

## 模块和运行时契约

| 模块 | 文件 | 职责 |
|---|---|---|
| NLS_Input（新增） | Runtime/Input/InputTypes、InputSystem、InputAction、InputUser、InputDomain、InputRebinding、InputReplay 的 .h/.cpp；CMakeLists.txt、InputDef.h | Base/Math 之上的纯 C++20 设备状态、序列事件、定义编译、求值、用户、Context、订阅、Fixed 缓冲、重绑定、无设备回放 |
| Platform | Runtime/Platform/Windowing/Inputs/PlatformInputBackend、WindowInputEvent、TextInputService、InputDisplay；InputManager.h/.cpp；Window.h/.cpp、Context/Device.h/.cpp | 唯一采集、GLFW/Win32 窗口登记、手柄轮询、捕获所有权、文本与 IME、显示名、旧 API 兼容 |
| ImGui 适配 | Runtime/UI/Input/ImGuiInputBridge.h/.cpp；UIManager.h/.cpp | 原后端事件链、多 Viewport、多 ImGui Context、NewFrame/Render 生命周期 |
| 资源适配 | Runtime/Core/Inputs/InputSerialization.h/.cpp；Core/Assets/AssetMeta、ArtifactManifest、ArtifactWriter、AssetVersion、NativeArtifactContainer；Runtime/Cook/AssetCooker.cpp；Project/Editor/Assets/AssetDatabaseFacade.cpp | 版本化 JSON、校验、原子保存、原生 Artifact 与 Cook |
| Engine | Runtime/Engine/Assets/InputActionsAsset.h/.cpp、RuntimeAssetDatabase；Components/PlayerInputComponent.h/.cpp；Serialize/PPtrResourceTypes.h、ObjectGraphInstantiator.h | 只读输入资产、稳定引用、打包读取、独立玩家实例和更新边界热重载 |
| Editor 触发适配 | Project/Editor/Shortcuts/EditorShortcutAdapter、EditorInputRouter、EditorGraphInput、EditorCommandInvocation | Action → 既有 Command Registry；焦点、面板、文本、模态、导航和图编辑器路由 |
| Editor 既有业务 | EditorShortcutRegistry/Command/Service；Core/EditorInputCommands.cpp；Editor.cpp、CameraController.cpp；Panels、Inspector 和 Scripting 中相应入口 | 保留命令、CanExecute、Undo、确认与文档历史；替换快捷键触发来源 |
| 旧配置导入 | Project/Editor/Shortcuts/EditorShortcutProfile.h/.cpp | shortcuts.json 的只读格式转换；不持有运行时配置、不保存旧文件 |
| 脚本 | Runtime/Scripting/InputScriptApi、RuntimePlaySession、GameInputFrame、CoreClrScriptBackend、LuaScriptBackend、ScriptEngineApi；Managed/Nullus.Managed/InputActions.cs、GeneratedBindings.cs；EditorScriptHost/Abi 和 Managed/Nullus.Editor | 共享原生运行态，Dynamic/Fixed 查询与订阅、Context/设备/Profile/重绑定、脚本生命周期 |

Input Core 不包含 JSON、AssetManager、ImGui、窗口后端或脚本依赖。宿主拥有一个 InputSystem 和平台采集器；所有窗口共享物理键鼠，不创建窗口专属键鼠设备。Editor Domain 随编辑器存在，Runtime Domain 随 Play Session 存在。每个 InputUser 独立拥有 Context、Interaction、Override、缓存和事件缓冲。只读 CompiledInput 可以共享。

析构顺序为：停止脚本/重绑定与订阅 → 销毁 Runtime users/domain → 拆除 Editor 适配/ImGui 子窗口回调 → 平台采集器 → InputSystem。PlayerInputComponent 借用 Domain 的弱生命周期。Play/Stop 不重置 Editor 物理设备。

### 求值顺序

Control Event/State → Composite → Binding Processor → Binding Chord/Interaction → 最高优先级 Context 映射组内聚合 → Action Processor → Action Chord/Interaction → State Commit → Callback Dispatch。

- 同一输入更新内，每条设备事件按 Sequence 顺序推进状态机。一次更新的 Pressed/Released 布尔值只是查询便利视图，不代替事件流。
- 同一 Action 的主绑定/备用绑定聚合；释放其中一个、另一个仍按住时，不产生 Action Released。
- Binding Interaction 决定来源是否参与聚合，Action Interaction 观察聚合结果；Binding Performed 不直接转发为 Action Performed。
- Pressed、Released、Performed、Canceled 独立。外部取消不会伪造 Released 或 Release-Performed。
- State、Delta、Rate 必须匹配定义；不能把 PointerDelta 和 Stick Rate 默认按幅值比较。默认 Gameplay 资源将 LookDelta/LookRate 分开，消费者按自己的时间步组合。
- Hold/Tap/MultiTap/Repeat 使用可注入的单调时间源。没有设备事件的更新仍推进到期计时。Pause 冻结该用户的活动 RealTime；SimulationTime 仍属于 Play 循环。
- Domain 先提交所有用户的 Snapshot，再分发回调。Domain.EnqueueChange 在下次输入更新开始执行；回调内的订阅、Context、用户移除/添加使用安全生命周期和延后变更。

### 宿主帧顺序

1. 平台 Event Pump；每个窗口只有登记过的一个归一化入口。GLFW/Win32 回调继续链到有效的 ImGui Platform Backend。
2. 手柄采样及失焦协调；准备本次原始有序事件。UIManager 为自己的 ImGui Context 执行一次有效 NewFrame。
3. Editor 的连续导航用户更新，Scene/浮动预览相机读取导航 Action；绘制面板获得当前焦点和遮挡/手势所有权。正常 UI 绘制结束后，Scene 再确认一次指针捕获，接住本帧因旧活动控件而暂时阻断的首次右键/中键；此处不重复移动、滚轮或模拟更新。
4. EditorInputRouter 授权各作用域；独立命令用户在当前焦点下计算完整命令事件序列，Adapter 调用 Registry。
5. Runtime UI 使用同一批有序事件；按 UI 焦点/消费/捕获屏蔽 Gameplay 的相应输入来源，再更新 Runtime Domain。Dynamic 回调先于游戏 Update，Fixed 命令先于对应物理步。
6. ImGui Render；宿主 EndFrame 清空瞬时输入，不清除仍按住的设备状态。resize repaint 不重新分发输入、不推进 Play。

WantCaptureKeyboard/Mouse 只是路由依据之一；窗口/内容面板、当前指针下实际窗口、活动控件、文本、菜单、模态、Game View 和手势捕获共同决定授权。

### FixedUpdate

- Action Event 指向下一次尚未执行的 Tick。零 FixedUpdate 时保留；执行目标 Tick 时按序消费一次；后续物理步不重复 Press/Release/Delta。
- Tick 开始采样 State/Rate；Rate 数值表示每秒意图，消费者使用该 Tick 的 FixedDeltaTime。Delta 只出现在所属 Tick。
- Pause/Stop/重新 Play、重绑定、资源重载使用 epoch/generation 隔离旧命令；Dynamic 可观察 Canceled，旧命令不进入后续 Fixed Tick。
- 单步延续现有一帧推进语义；零物理步的单步事件留在单步缓冲，恢复/退出单步时清除余量。
- InputReplayRecord/ReplayInput 支持类型化日志：设备/窗口拓扑、原始事件与来源序号、定义/Profile、Context、配对、阻断、取消、暂停、更新时刻、Tick 和 EndFrame。回放使用真实求值器，不依赖物理设备或 Gameplay 回调。日志应从新的 Play epoch 开始并包含初始配置；不承诺从任意正在进行的 Interaction 中途自动恢复。当前提供 C++ 日志接口，未增加独立的磁盘录像格式或后台录制 UI。

### Context 与恢复

支持 Never、OnActuated、OnStarted、OnPerformed；支持 RequireRelease、ResumeImmediately。默认编辑命令 OnActuated，在已授权作用域内，即使 CanExecute=false 也不向其他域兜底。消费的是 Action 来源；持续指针捕获由窗口和 owner 单独管理。

失焦、Context 禁用、资源更换、重绑定或 Play 生命周期取消受影响 Action；底层按住状态按真实设备保留，整宿主失焦/设备断开才协调清空。RequireRelease 必须先中立，避免粘键与幽灵输入。重绑定有独立 suspension lease，不会在结束时错误解除 Pause、失焦或组件禁用；Definition/Profile 改变会取消尚未应用的捕获结果，已完成但未应用的结果也可以取消。捕获事件被用户检查点跳过，之后同批次独立短按仍可执行。

## 阶段 1：事件和设备

新增 Runtime/Input/InputTypes、InputSystem 与平台 Backend/TextInputService；修改 Window、Device、InputManager 和 ImGuiInputBridge/UIManager。

完成：序列/单调时间/来源事件、同帧多边沿、状态查询、键鼠/手柄、虚拟输入、设备/窗口 generation、热拔插清理、屏幕/窗口坐标、warp 标记、指针捕获 owner、主/子窗口唯一入口、多 ImGui Context 原后端转发。InputManager 保留低层接口和既有 Event 注入测试能力。

自动化入口：InputSystemTests.cpp、InputPlatformTests.cpp、InputManagerTests.cpp。Windows 隐藏真实 GLFW/Win32 窗口覆盖事件去重、非 BMP 字符、浮动窗口、上下文切换和重复采集器拒绝。真实键鼠/手柄和可见多 Viewport 拖拽仍待人工验收。

下一阶段兼容点：输入核心不能再自行轮询 OS；原 ImGui 后端必须始终收到原始事件。新窗口注册到现有采集器。

## 阶段 2：Action、Context 和用户

新增 InputAction、InputUser、InputDomain、InputRebinding、InputReplay 的 .h/.cpp；新增 InputActionTests.cpp、InputSerializationTests.cpp。

完成：稳定 Guid 定义/编译缓存、独立用户状态、Button/Axis/Vector2/Vector3、Axis/Vector composites、Scale/Invert/Deadzone/Normalize/Clamp、两层 Press/Release/Hold/Tap/MultiTap/Repeat、Chord、精确/子集修饰键、Primary、Context 消费/恢复、Dynamic/Fixed、RAII 订阅、三态 Override、轴去噪/启动事件过滤、配置/生命周期日志回放。

自动化覆盖：别名同时激活、部分释放、处理后阈值、无事件计时、同帧多次短按、Context 切换、State/Delta/Rate 校验、零/多物理步、回调修改订阅/Context、玩家隔离、活动订阅重载、重绑定捕获和后续独立输入、确定性命令回放。

下一阶段兼容点：缓存 ActionHandle 带 generation；资源更换后重新 FindAction，或使用稳定 ActionId 订阅。不要把共享定义作为玩家状态。

## 阶段 3：路由与 C++ 迁移

新增 Shortcuts/EditorShortcutAdapter、EditorInputRouter、EditorGraphInput、EditorCommandInvocation，Core/EditorInputCommands.cpp。修改 Editor/Core 的 Context、Application、Editor、EditorActions、CameraController、EditorInputFrame；Game/Core 的 Context、Game；RuntimePlaySession/GameInputFrame。

保留并适配 EditorShortcutRegistry/Command/Service。Registry 新增统一 CanExecute/TryExecuteCommand，沿用 availability/execute/Undo 业务。InvocationContext 保存来源、面板、显式对象/选择集/路径、Play Mode 与来源序号。命令回调以 CommandId 和注册 generation 解析，不缓存可能失效的注册表元素指针。

完成迁移的文件与作用域：

- Panels/MenuBar、Toolbar、EditorTopBar、SceneViewToolbar：File/Edit/Play/工具/枢轴等同语义按钮与快捷键共用 CommandId 和有效提示。
- Panels/SceneView、Hierarchy、AssetBrowser：作用域命令、显式右键目标与键盘选择；继续使用原确认、等待释放、HierarchyUndo 和资源业务。
- Inspector/InspectorView、Panels/Inspector：组件 copy/paste/reset/move/remove 共用目标校验和既有 Undo；销毁目标不执行，也不兜底到 Global。
- Panels/UIDocumentDesigner、VfxEditor、VfxLibraryEditor、Console：去掉已迁移的局部业务热键检测，保留文档执行器、历史和原生控件。
- CameraController：生产路径读取连续导航 Action；原 scene-view.fly-* 迁移成导航 Composite 的独立槽位，允许 Shift 加速，不保留空业务执行体。相机预览 F/方向观察转为命令。
- ImGuiNodeEditor 的实现/声明/API/internal 与 UPSTREAM.md：单独管理键盘输入及程序化请求，保留 Alt+点击删连线等指针手势；Clipboard 只在使用 BeginShortcut 的图客户中取得所有权，现有文档剪贴板业务不被替换。

Text Editing、Modal、Game View、工作台、Console 和图区域均有路由边界。普通 ImGui 控件继续使用原生输入。动画工作台原有普通控件/图输入沿用此边界；未建立第二个快捷键后端。

每个已迁移类只有 Action → Adapter → Registry 一条输入触发链。EditorShortcutResolver 及 ExecuteShortcut、ExecutePressedShortcut、ExecutePressedGlobalControls 已删除，未初始化宿主也不再提供旧检测器回退。Registry 不保存 Binding、Override 或冲突算法；Command 不保存默认键位、备用键或 Repeat。默认映射、槽位和 Interaction 只来自 .inputactions；动态命令没有资产映射时提供可由 Profile 绑定的空槽位。导航使用独立 ShortcutDescriptor 设置元数据，不注册空业务 Command。

下一阶段兼容点：菜单弹出前保存内容目标；CanExecute 不参与输入域授权。新快捷键必须注册 Command + 默认资源映射，不增加 ImGui::IsKeyPressed 业务扫描。

## 阶段 4：资源、Profile、脚本和文本

4A：新增 Core/Inputs/InputSerialization、Engine/Assets/InputActionsAsset、Components/PlayerInputComponent、Panels/InputAssetEditor；修改资产分类、原生 Artifact、Cooker、PPtr 引用、AssetBrowser 打开流程、ShortcutSettingsPanel/ShortcutPropertyDrawer。

- .inputactions/.inputprofile version=1；校验 ID、引用、枚举、范围、有限数值和 Nature；失败不替换当前合法数据；文件原子保存。
- 默认资源 App/Assets/Editor/Input/EditorShortcuts.inputactions：124 Action、134 稳定槽位。App/Assets/Engine/Input/DefaultGameplay.inputactions：Move、LookDelta、LookRate、Jump 示例。
- Editor 写项目 UserSettings/editor.inputprofile；Gameplay 独立用户/路径。Default/Override/ExplicitlyUnbound 区分恢复默认与清除。设置 Undo 保存完整三态和未应用项。
- 仅新 Profile 不存在时导入 shortcuts.json，旧文件保留；旧覆盖抑制全部备用别名，unassigned 清除全部槽位，未知/删除/冲突项保留诊断。初始化生产宿主只写新 Profile。
- 冲突检查使用公共有效 Binding/Chord 和互斥 Context；显示、槽位重绑定、Primary/物理/逻辑键查询共用公共接口。鼠标和手柄也能重绑定到编辑器命令。
- PlayerInputComponent 绑定只读资产及独立用户；已存在映射的热重载在下一次 Domain.Update 开始应用，保留合法 Override/订阅，取消受影响 Action 并刷新句柄；非法资产保留上一版。
- 输入资产经真实导入、原生容器、Manifest、Runtime loader 和压缩 .npak 验证；包内读取不依赖松散源文件。
- Input Editor 可编辑 Action/Context/Binding/Composite/Processor/Interaction、稳定 ID、校验及保存；调试页查看设备、用户/Context/Action、epoch、Tick、待消费事件及无效 Override。

4B：新增 InputScriptApi、Managed InputActions.cs，并扩展 CoreCLR/Lua、ScriptEngineApi、EditorScriptHost/Abi、Managed/Nullus.Editor 的动态菜单注册。

- C#/Lua 使用原生用户状态，支持 Dynamic/Fixed 查询/有序事件、Action 订阅、Context、设备枚举/连接/映射状态、设备配对、显示名、Profile、重绑定及候选冲突计数。失效设备句柄不能配对；超时释放租约并保留可查询的终态。最新脚本适配修改及新增超时测试尚待 Play 集成复测，不计入本轮 128 项独立测试。
- 订阅由对象所有权和 token 管理；禁用后跳过旧事件，销毁/重载/Stop 解绑；异常不损坏其他订阅；Native ABI 尾部扩展并验证尺寸。
- 动态菜单稳定 CommandId 使用 script: 命名空间；注册 generation 管生命周期，菜单改名不改变持久化身份；旧描述符仍保留菜单功能。
- 当前 Lua 配置用双精度 number 传递设备/用户/事件 token，整数精确范围为 2^53-1；C# 使用 ulong。避免本仓库 Lua C89 的 32 位 integer 截断 generation 设备 ID。

4C：TextInputService 分离 Unicode 提交与 IME composition。Windows 观察组合态/光标，原字符路径仅提交一次，Runtime TextField 提供候选窗位置，ImGui 保持原 IME 后端。macOS/Linux 当前仅声明字符提交能力，不宣称原生组合态/候选窗适配已完成。

下一阶段兼容点：不修改默认 Binding 保存玩家配置；游戏资源必须进入 Runtime Manifest；文本不得当成键盘 Action。真实 IME 和跨平台待验。

## 阶段 5：回归、调试与质量

新增/扩展 InputSystemTests、InputActionTests、InputSerializationTests、InputPlatformTests、EditorInputAdapterTests、InputAssetTests、PlaySessionTests、PlayInputTests、EditorScriptTests、ImGuiNodeEditorIntegrationTests 与受影响既有测试；Tests/Unit/CMakeLists.txt 提供独立目标并保留聚合 NullusUnitTests 的原断言。

PlayLoopNoUiTests 的本地无 UI RuntimePlaySession 编译副本使用独立类名，正常导入共享脚本 API，修复其在聚合目标中的重复符号；没有删测试、屏蔽链接错误或改变断言。

核心缓存和容器重用已实现；Debug 基准使用 128 个活动 Action、2000 次更新。测试检查事件和固定缓冲不会无限累计，不设置易抖动的机器耗时门槛。最新测试日志记录实际耗时；未运行堆分配追踪或 Release 性能基准，不能据此声称零分配。

现有三平台 CI 的聚合 Unit 源码 glob 包含新增测试；本次只在本机 Windows 构建/执行，不将 CI 配置存在称作 CI 已通过。

### 2026-10-29 快捷键迁移收尾

本轮实际文件和职责：

| 文件 | 完成的清理 |
|---|---|
| Shortcuts/EditorShortcutRegistry.h/.cpp、EditorShortcutCommand.h | Registry 只登记/查询/执行命令；拆出 ShortcutDescriptor；移除默认绑定、备用键、Override、旧冲突算法；availability/CanExecute 之间检查注册 generation，阻止卸载后调用旧闭包 |
| Shortcuts/EditorShortcutResolver.h/.cpp | 删除；同时移除 Tests/Unit/CMakeLists.txt 中的编译引用 |
| Shortcuts/EditorShortcutService.h/.cpp、EditorShortcutAdapter.h/.cpp | 删除全部旧轮询/匹配/全局扫描和未初始化回退；有效槽位从编译资源缓存 BindingId；配置仅在 InputUser 中维护；菜单继续进入原 Registry |
| Shortcuts/EditorShortcutProfile.h/.cpp | 缩为只读导入器；删除全部配置修改和 Save；新 Profile 优先，未知版本拒绝导入，旧文件不写回；先转换整组候选，再按 Context priority/注册顺序保留冲突胜出项，失败提案保存到 unresolved |
| Core/Editor.cpp、Editor.h、EditorInputCommands.cpp、Shortcuts/EditorGraphInput.cpp | Command 注册只描述业务调用；删除重复 C++ 默认键位/别名/Repeat 表；启动先加载默认资源和新 Profile，仅新 Profile 缺失时导入旧 JSON；析构只保存新 Profile |
| Core/CameraController.cpp | 删除 F/方向键重复检测、导航绑定解释和 WASD 回退；导航值只读 Action；相机 Shift 速度和 Alt 指针手势仍使用统一 InputManager 的低层状态 |
| Panels/ShortcutSettingsPanel.h/.cpp | 列表使用 Command/连续输入的描述元数据；按资源实际槽位显示/重绑定/清除/恢复，槽位顺序改变不修改持久化 BindingId |
| Tests/Unit/EditorInputTestSupport.h（新增），EditorInputAdapterTests、EditorShortcutConflictTests、EditorShortcutPersistenceTests、EditorShortcutResolutionTests、ShortcutSettingsPanelTests、InputAssetTests | 原有行为断言迁入真实 InputSystem/CompiledInput 路径；测试辅助代码只生产定义/设备事件，不实现另一套匹配器；增加 10 项迁移与生命周期回归 |

静态调用审计覆盖 123 个生产 CommandId，默认资产全部存在对应 Action；另有一个连续导航 Action，共 124 个 Action。生产 Editor 中 Resolver、旧三个 Execute 入口、defaultBinding、alternateBindings 均为零引用。保留的原生 ImGui 输入属于文本、列表/搜索、改名、属性值控件和活动指针手势；Asset Browser 保留 Delete 等待释放保护，不再用它触发业务删除。审计结果在 `build/input-migration-audit.log`。

新增回归覆盖：真实默认资产与导航不注册 Command、槽位重排后的稳定 ID、持有动作时重载与活动订阅、全部别名恢复的原子校验、新 Profile 优先且拒绝旧文件写入、旧冲突提案保存/修复、交换默认键位、未知旧版本、导航覆盖导入、availability 中替换命令的回调安全。

兼容变化：独立调用者必须 InitializeInput 并提供默认定义/资源；不再接受旧的按键 predicate 轮询接口。Binding 的数值槽位仅用于当前设置 UI，持久化及 Undo 始终使用资源的 BindingId。默认资源必须随 Editor 部署，不再有 C++ 键位表作为第二来源。完整 Editor/Game/Play 链接、可见交互、全量/跨平台/Release 性能验收仍待并行改动稳定后进行。

## 构建和测试证据

### 2026-10-29 剩余编辑器操作迁移

新增 `Project/Editor/Shortcuts/EditorInputActions.h/.cpp`：稳定操作标识和 Editor
消费者查询入口；复用现有 InputUser、Action Snapshot、Profile、重绑定与设置界面，
没有创建另一套设备采集或快捷键解析器。Input Core 本轮没有增加 Editor/ImGui 依赖。

| 实际文件 | 完成的迁移 |
|---|---|
| Shortcuts/EditorShortcutAdapter.h/.cpp、EditorShortcutService.h/.cpp | 加载新增资源 Action 和独立槽位，缓存带 generation 的句柄；同帧边沿与增量缓存；用事件来源区分所有权恢复和新捕获；PrepareInput 前返回中立状态，避免旧路由提前消费新按下；导航用户正常清理已分发事件，避免待分发事件累计；设置、冲突、Profile 与重绑定复用现有接口 |
| Core/CameraController.cpp、Editor.cpp | Fly/Pan/Orbit、加速、指针 Delta 和滚轮改读 Action；导航作用域使用逻辑 Fly 状态，删除固定右键、中键、Alt、Shift 的操作判断；保留窗口捕获、失焦、光标环绕、首次捕获抑制和真实视口授权 |
| Panels/SceneView.h/.cpp、Core/EditorInputCommands.cpp | 临时吸附、选择修饰、导航排他及取消改用 Action；新增有实际执行体的 scene-view.cancel-gesture Command/作用域，统一取消选择和 Gizmo；取消帧不先提交鼠标释放，避免提交后再撤销 |
| UI/UIDocumentCanvasView.cpp | 平移、Space+左键组合、Ctrl/Shift/普通滚轮模式、吸附绕过、等比缩放和画布选择修饰改读 Action；保留布局、模型事务及原有业务执行器 |
| Panels/VfxEditor.cpp | 时间轴滚轮缩放改读 Action；列表选择控件保留原生修饰键语义 |
| Panels/AssetBrowser.cpp | 删除操作的等待释放保护查询当前删除 Action，消除固定 Delete 键假设；保留菜单鼠标释放和确认保护 |
| App/Assets/Editor/Input/EditorShortcuts.inputactions | 新增 20 个 Action、32 个 Binding，合计 144/166；原有 124 个 Action 和 134 个 Binding 的数据与 ID 均保持不变 |
| Tests/Unit/EditorInputAdapterTests.cpp、CameraControllerInputTests.cpp、VfxEditorPanelTests.cpp | 新增资源槽位、晚事件与增量、Pan/Orbit 切换、画布组合与滚轮分流、导航换绑持久化、Game 输入交还、Save 不被连续修饰动作消费、热重载缓存、取消命令唯一执行及无事件 Hold 计时回归；真实相机覆盖旧右键失效、新键捕获并配合 W 移动；VFX 既有快捷键测试改为真实 Input/Command 路径，保留原业务断言 |

静态审计保留的输入包括 Asset Browser/Hierarchy/VFX 列表多选、文本改名与 Inspector
原生属性控件取消、VFX 添加弹窗的上下键/Enter、窗口 resize 重绘和指针捕获所有权。
这些是原生控件或底层指针状态，不作为独立业务快捷键分发。相机、Scene 与 UI 画布
已无固定键盘/业务快捷键轮询。证据为 `build/input-remaining-migration-audit.json`、
`build/input-remaining-operation-audit-before.log` 和本轮更新后的审计记录。

本轮已经真实构建 EditorProject、Editor、输入集成、UI Designer 和 VFX 面板目标。
记录为 `build/input-remaining-final-link.log`、`build/input-remaining-vfx-test-build.log`；
源码编译验证为 `build/input-remaining-source-verification.log`。独立五组输入测试
154/154，真实相机及视口回归 15/15；没有用空实现替换依赖。

| 正式定向测试目标 | 同期通过结果 |
|---|---|
| NullusInputEditorTests | 73/73 |
| NullusInputIntegrationTests | 42/42 |
| NullusUIDesignerTests | 83/83 |
| NullusVfxEditorPanelTests | 107 通过，6 项 Vulkan 测试因当前未编译该后端而跳过 |
| 合计 | 311 项执行：305 通过、6 跳过，CTest 4/4 |

最终证据为 `build/input-remaining-main-final-ctest.xml`、`build/input-remaining-main-final-tests.log`、
`build/input-remaining-main-final-results/`。VFX 中旧的纯 ImGui 按键注入测试
已迁入真实 InputSystem、默认资产、Context 和 Command Registry；三项导入测试在
较长 TEMP 下发生 DXC 临时输出路径失败，改用工作区内 `tmp/in` 后 3/3 通过，随后
完整 VFX 定向组通过。记录为 `build/input-remaining-vfx-import-check.log`；没有修改
Shader 编译器或降低导入断言。

最后收尾期间其他会话将 RuntimePlaySession::End 改为 End(bool)，旧脚本库一度
造成输入集成重新链接失败；同步依赖又遇到另一个主构建占用 Engine/MetaGenerated.obj。
保留新接口且未终止其他构建；共享主构建结束后继续同步 Game 和测试。相关记录为
`build/input-remaining-release-check-build.log`、`build/input-remaining-dependency-sync-build.log`
及 `build/input-remaining-synchronized-delivery-build.log`。随后发现生成工程中新加的
PhysicsShapeBuilder.cpp 尚未产生对象文件，重新加载生成工程后成功补编译；没有修改
物理实现、删除功能或提供替代符号。最终 Game、输入集成、UI Designer 和 VFX 面板
完整依赖构建通过，记录为 `build/input-remaining-fresh-project-build.log`；最终 Editor
和 73 项快捷键测试目标构建通过，记录为 `build/input-remaining-final-editor-build.log`。
上表四组回归使用这次同步后的当前库重新运行，45.67 秒完成。早期隔离相机库快照因
脚本 ABI 变化不再用作最终交付，最新相机断言由正式 InputIntegration 目标执行。

本轮实际命令：`Tools/BuildCleanEnvironment.cmd cmake --build build --config Debug --target Game NullusInputIntegrationTests NullusUIDesignerTests NullusVfxEditorPanelTests -- /m:1 /v:minimal /p:LinkIncremental=false`；随后用 `--target Editor NullusInputEditorTests` 并加 `BuildProjectReferences=false` 完成最后链接。
验证命令：`ctest --test-dir build -C Debug -R "^(NullusInputEditorTests|NullusInputIntegrationTests|NullusUIDesignerTests|NullusVfxEditorPanelTests)$" --output-on-failure --output-junit build/input-remaining-main-final-ctest.xml`，TMP/TEMP 指向工作区 `tmp/in`。

本轮仍未做 Windows 可见窗口的完整手动操作、真实键鼠/手柄、IME、macOS/Linux
或 Release/堆分配验证；没有重跑完整 NullusUnitTests，历史全量未完成及失败归因
缺口仍保留。以下既有集成记录作为历史证据。

### 2026-10-29 集成恢复和首次输入修复

本轮实际修改与职责：

| 文件 | 实际修改 |
|---|---|
| Runtime/Input/InputSystem.h/.cpp | 正常平台事件泵结束时记录 Sequence 边界；EndFrame 保留命令回调/原生对话框期间收到的后续事件，并将其边沿、Delta、Wheel 转交下一帧。保留设备状态和事件顺序；用户 checkpoint 防止重复消费 |
| Runtime/Input/InputReplay.h/.cpp | 增加 FinishEventPump 日志操作，保留原操作数值；回放覆盖对话框后的释放及零步/多步 Fixed 消费 |
| Project/Editor/Core/SceneViewCameraInteractionStateMachine.cpp | Blocked 解除时继续处理同一帧的新右键/中键边沿；没有新按下边沿的 held 状态仍不能取得捕获，窗口失焦、文本和未授权视口仍阻断 |
| Project/Editor/Core/EditorGuiInput.h（新增）、Editor.cpp | 根据当前 ActiveId/InputTextState 判断文字编辑，避免控件已释放焦点后仍受上一轮 WantTextInput 阻断；新导航按下仅在实际指针窗口授权、无活动拖拽/弹窗阻断及原指针所有者已释放时聚焦视口；正常 UI 绘制后刷新 Scene 捕获，resize 重绘不重复输入 |
| Project/Editor/Panels/SceneView.h/.cpp、Core/CameraController.h/.cpp | 共用相机路由准备与指针状态更新；绘制后只重新确认捕获；在捕获抑制帧初始化 Fly 朝向，使下次真实移动立即旋转，不额外丢弃一个首次鼠标样本 |
| Tests/Unit/InputSystemTests.cpp、InputActionTests.cpp、EditorInputAdapterTests.cpp | 增加晚到边沿/鼠标 Delta、对话框内释放后的连续保存、真实默认资源跨帧 Ctrl+S、Dynamic/Fixed 回放；Debug 基准包含事件泵边界 |
| Tests/Unit/SceneViewCameraInteractionStateMachineTests.cpp、EditorInputFocusTests.cpp（新增）、CameraControllerInputTests.cpp、CMakeLists.txt | 覆盖阻塞解除当帧首次右键、held 不接管、真实文字控件释放焦点及指针所有权；真实 CameraController 隐藏窗口测试验证原始右键被接住、晚刷新不重复运动/滚轮和下一次移动旋转；焦点与状态机测试加入主 InputIntegration 目标 |
| Runtime/Scripting/ScriptRuntimeSetup.cpp | 嵌套项目优先选择自己的托管产物；祖先项目的 Library/ScriptBuild 缓存不再覆盖当前项目，仓库 SDK/build 仍可作为回退 |
| Managed/Nullus.Managed/Behaviour.cs、Tests/Unit/ScriptRuntimeTests.cpp | 用 ConditionalWeakTable 保存 Type 键的包装器/工厂/生命周期缓存，解除 collectible ALC 强引用；验证重载后仅一个活动 ALC、Shutdown 后为零，并隔离测试项目目录 |

保存回归在修复前实际失败：首次保存回调模拟原生消息循环接收释放，EndFrame 清空后，第二/三次按下均无法再执行。相机回归在修复前也实际失败：Blocked 恢复当帧的新右键返回 Neutral，没有获得捕获。失败日志分别为 `build/input-save-repro-test.log`、`build/input-camera-repro-test.log`。

最新独立构建直接编译生产 Input、完整 Platform 源码、快捷键适配和相机状态机；Platform 编译定义、源文件和真实链接库来自主 vcxproj。产物隔离在 `build/input-validation`、`build/input-ui-acceptance-host`。另外单独编译当前 CameraController、状态机和原 CameraControllerInputTests，链接真实 EditorProject 静态库快照及生产依赖，输出 `build/input-camera-runtime/Debug/NullusInputCameraRuntimeTests.exe`；没有替代相机或窗口实现。此测试使用隐藏原生窗口、注入输入和 UI backend None，验证真实控制器行为，不验证渲染或可见物理操作。

| 最新独立目标 | 结果 |
|---|---|
| NullusInputTests | 58/58 |
| NullusInputEditorTests | 62/62 |
| NullusInputPlatformTests | 13/13 |
| NullusInputCameraTests | 9/9（包含既有状态机断言） |
| NullusInputFocusTests | 2/2（真实 ImGui 文字控件和视口焦点） |
| 上述独立目标 | **144/144，CTest 5/5** |
| NullusInputCameraRuntimeTests | **15/15**（6 项真实控制器、9 项 ViewportMouseInput） |
| 最新定向回归合计 | **159/159** |

证据：`build/input-routing-final-tests.log`、`build/input-routing-final-ctest.xml`、`build/input-routing-final-results/`、`build/input-camera-runtime-final-verified-build.log`、`build/input-camera-runtime-final-tests.log` 和 `build/input-camera-runtime-final.xml`。`build/input-focus-source-verified-build.log` 还记录 Editor、SceneView、CameraController、命令与属性设置及相应测试等 10 个受影响编译单元构建成功；源码哈希记录为 `build/input-routing-validation-final-snapshot.json`。

复现上述独立验证可使用 `cmake --build build/input-validation --config Debug --target NullusInputTests NullusInputEditorTests NullusInputPlatformTests NullusInputCameraTests NullusInputFocusTests NullusInputEditorSourceChecks -- /m:1 /v:minimal`、`ctest --test-dir build/input-validation -C Debug --output-on-failure --output-junit build/input-routing-final-ctest.xml`，真实相机目标单独构建后执行上述 exe；日志保存最终结果。128 个活动 Action、2000 次带事件泵边界的 Debug 更新曾记录约 443.7 us/update；同时有其他构建负载，仅作样本，不是 Release 性能或零分配结论。

本轮在最后两项首次输入修复之前，主工程 Editor、Game、NLS_Scripting 及七个定向测试目标已经真实构建成功，日志为 `build/input-integration-resume-build-approved.log`。修复托管产物选择和 ALC 缓存后，七组集成测试 **259 项中 258 通过、1 项可选 LuaPanda 跳过，CTest 7/7**；证据为 `build/input-integration-final-tests.log`、`build/input-integration-final-ctest.xml` 和 `build/input-integration-results-final/`。这组结果不能代替最后新增修复及其他并行改动的集成复测。

全量 NullusUnitTests 已构建，但未完成合格验收。测试目录物理 DLL 曾缺少两个 GetRevision 导出，重链接真实 NLS_Physics 并部署后可以执行。随后一次全量运行中断前记录 3348 项开始、3212 通过、99 失败、36 跳过，最后一项渲染测试未完成。进程在诊断采样退出时被结束（0xc0000354）；不能称全量通过，也不能把中断归结为引擎断言。99 个失败尚未完成归因，日志为 `build/input-full-unit-tests.log`。

后续重建遇到并行生成产物/依赖不同步：Scene.generated.h 为第 43 行宏而源码为第 44 行，运行真实 MetaParser 后恢复；接着 Engine 链接缺少新 ECS JobBatch 导出。已重新配置并启动完整依赖构建，过程记录在 `build/input-final-refresh-configure.log`、`build/input-final-dependencies-build.log`。检测到其他会话也在写同一 Core/Editor 产物后，结束本任务确认过身份的重复构建树，让其他流水线完成；记录为 `build/input-own-build-yield.json`。未结束其他构建，也未修改或删除这些并行业务实现。最新完整集成仍待补测。

可见 Editor 使用独立验收项目和复制目录，操作脚本检查 PID、启动时间、窗口/前景及点击所有权。曾观察到 E 切换工具及原生 New Scene 保存对话框，但尚未完成可靠的连续保存、重开和右键相机交互验收，不能仅凭截图、窗口存在或进程存活宣称修复完成。

### 历史结果：快捷键清理后的独立模块验收

当时用户确认 Physics/FixedStepClock、RuntimePlaySession 仍在并行修改，要求先验证独立输入模块，因此该轮未继续重链接集成目标；此限制已被本轮恢复集成的指令取代。

独立工程位于 `build/input-validation-source/CMakeLists.txt`，生成目录为 `build/input-validation`。它直接编译当前 Runtime/Input 全部生产源码、Core/Inputs/InputSerialization.cpp、Editor Shortcut Registry/Service/Adapter/只读 Profile 导入器与相同测试源码；Platform 和 ImGui 使用主工程已经构建的真实库，并重新编译 ImGuiInputBridge 和原生窗口测试。另有独立对象目标编译受影响 Editor 源码。没有替换依赖实现或模拟编译结果。

验证产物与主 build 分开，未改写共享的物理/Play 构建产物。测试使用工作区下的临时目录，避免 Windows 沙箱 TEMP 中原子文件重命名被拒绝。

```powershell
.\Tools\BuildCleanEnvironment.cmd cmake -S build/input-validation-source -B build/input-validation
.\Tools\BuildCleanEnvironment.cmd cmake --build build/input-validation --config Debug -- /m:1 /v:minimal
$env:TMP = "$PWD/build/input-validation/temp"
$env:TEMP = $env:TMP
.\Tools\BuildCleanEnvironment.cmd ctest --test-dir build/input-validation -C Debug --output-on-failure
```

| 目标 | 最新结果 | 说明 |
|---|---|---|
| NLS_Input + 3 个独立测试目标 | 编译、链接通过 | 当前生产输入/序列化/快捷键适配源码；复用已构建 Base/Math/Platform/ImGui 依赖 |
| NullusInputTests | **55/55** | 多边沿、两层 Interaction、Fixed、Context、重绑定事务、Profile、回放和默认资源 |
| NullusInputEditorTests | **60/60** | 原有 50 项迁入统一运行时；新增 10 项配置迁移、稳定槽位、热重载和命令生命周期回归 |
| NullusInputPlatformTests | **13/13** | 原有 InputManager、隐藏 GLFW/Win32 窗口、ImGui 多 Context/子窗口及输入唯一性 |
| Editor 源码检查目标 | 8 个编译单元通过 | Editor、EditorInputCommands、CameraController、EditorGraphInput、ShortcutSettingsPanel、ShortcutPropertyDrawer、ShortcutSettingsPanelTests、InputAssetTests；独立编译，不链接 Physics/Play |
| 合计 | **128/128；CTest 3/3 通过** | Windows 独立自动化验证；不包含 Engine/Play 集成或可见实机交互 |

证据：

- `build/input-migration-configure.log`、`build/input-migration-source-configure.log`
- `build/input-migration-final-build.log`、`build/input-migration-editor-compile.log`
- `build/input-migration-final-tests.log`、`build/input-migration-final-ctest.xml`
- `build/input-migration-audit.log`；上轮 118 项的旧日志仍保留，不作为本轮结果
- `build/input-validation/Testing/Temporary/LastTest.log`（完整 GoogleTest 明细及 Debug 性能采样）
- 输入变更的 `git diff --check` 通过；新增 Input/Core 输入源文件没有行尾空白。此项只代表文本质量检查。

### 历史集成里程碑与当时阻塞

以下是真实的历史结果，不能代替最新源码的集成验证：

| 目标 | 历史已观察结果 | 当前状态 |
|---|---|---|
| Editor / Game / NullusUnitTests 等 | `input-boundary-regression-build.log`、`input-journal-build.log` 曾构建成功 | 最新整体构建未通过，等待并行物理改动稳定后重生成工程 |
| NullusInputIntegrationTests | 30/30：真实 .npak 读取、组件边界热重载、Inspector 目标销毁、图输入等 | 最新集成待复测 |
| NullusPlayModeTests | 29/29：C#/Lua、UI 同帧多点击、Fixed 独立消费等 | 新增设备枚举/配对与重绑定超时状态的最新版本待构建/执行 |
| NullusPlayLoopNoUiTests | 1/1 | RuntimePlaySession 最新修改后待复测 |
| NullusUnitTests 全量 | 曾启动 4,738 项，未完成；TEMP 原子提交失败后，在渲染对象绑定测试触发断言退出 | 不能声称全量通过；重定向 TEMP 的 3 个失败样本已单独 3/3 通过，完整重跑待集成稳定 |

最新主工程构建日志 `build/input-delivery-build.log` 中，Scripting 链接缺少 FixedStepClock 的 Configure/Advance/Reset 等符号。已确认 FixedStepClock.cpp 是生成工程之后新增的文件、尚未列入 NLS_Physics.vcxproj。没有删除物理功能或改写该实现来绕过链接；按用户要求停止整体构建，改为上述隔离验证。

现有 build 曾缺少部分生成项目。配置恢复使用已有本地 PhysX 源码 `-DNLS_PHYSX_SOURCE_DIR=D:/Code/Nullus/build/_deps/nullus_physx-src`，不下载新依赖；记录在 `build/input-offline-configure.log`。环境中的 Path/PATH 问题使用现有 BuildCleanEnvironment；MSVC 调试信息使用 /Z7。没有清理工作区或停止用户的 Editor/其他构建进程。

并行改动稳定后，继续重生成主工程，构建受影响 Editor/Game/Scripting 与测试目标，执行资源、Play、无 UI Play 和完整回归，重新检查部署 DLL 的一致性；不能沿用旧结果作为新版本验收。

## 尚未覆盖的验收与限制

- Windows 可见编辑器：保存/Undo/Redo、W/E/R、各面板 Delete、文本/弹窗、Scene/Game/浮动窗口拖拽、菜单和工具栏焦点的完整人工交互。隐藏窗口和单元测试不替代这项验收。
- 物理手柄轴噪声/拔插、不同键盘布局、鼠标捕获、Windows IME 真正输入及候选窗定位。已映射手柄的摇杆轴为 -1..1、扳机轴为 0..1；物理设备验证仍待完成。
- 当前 GLFW 接入合并物理键盘/鼠标；不暴露多个实体键鼠的独立身份。未映射控制器只枚举为 UnmappedJoystick，不读取其原始轴按钮；补充 GLFW 映射后才可驱动 Action。C++ GamepadState.mapped 与脚本 IsMapped/mapped 表达此限制。
- 内置 Runtime UI 桥接当前覆盖键鼠和文本；没有新增默认手柄 UI 导航。界面上层可以使用通用 Action/Context 接入手柄意图。
- macOS/Linux 编译、CI 执行、实机窗口焦点与 IME；当前环境没有完成这些验收。macOS/Linux 的原生组合态/候选窗适配尚未实现，不宣称 GLFW 字符提交等价于完整 IME。
- 回放已有 C++ 类型化日志和确定性验证；没有追加面向用户的录像文件标准或完整编辑器会话自动采集 UI。
- 核心 Debug 性能采样已记录；Release 基准与当前线程 C++ 分配计数已完成，详见下方 2026-10-29 报告。整编辑器、其他线程/外部 DLL 和跨平台性能仍未覆盖。
- 既有 Q/View 工具业务维持原项目行为，本次不扩展工具本身。

## 2026-10-29：Release 性能与堆分配检查完成

详细文件、职责、构建命令、数据和限制见
`Docs/Reports/InputSystem-Release-Performance-2026-10-29.md`。
本轮新增 `Tests/InputPerformance` 独立基准、分配计数器和 CTest 门槛，
并在主工程性能开关下接入。复用同一生产源码与现有 GoogleTest，不依赖编辑器或渲染构建。

实测后修改 `Runtime/Input/InputUser.cpp` 与 `InputDomain.cpp` 的队列容量复用，
每次回调修改从 2 次/192 字节、Domain 修改从 1 次/64 字节，降至预热后 0 次。
回调分发避免不匹配订阅的原子引用计数，消费阶段避免不必要的事件扫描；
128 Action 同时触发的三轮中位耗时从 488.972 μs 降至 158.438 μs。
18 种预热输入负载三轮均为 0 分配，定义编译和 Override 的结构性分配独立记录。

生产 `NLS_Input` Release DLL、独立 Release 与 Debug 回归目标均构建成功；
Release 核心行为 60/60，Debug 核心/编辑器适配/平台/相机状态机/焦点 157/157，
新增分配检查通过。测试保留原行为断言，新增订阅增长及嵌套/异常边界验证。

后续兼容性重点仍是可见编辑器、真实设备和跨平台验收；本轮不改变
公开输入 ABI、资源格式、事件顺序、Tick 归属或 Editor Command 迁移状态。
计数不覆盖 OS 堆、外部 DLL 和其他线程，不表示整个编辑器零分配或没有泄漏。

## 2026-10-30：全量回归执行完成，完整验收仍开放

实际改动包括 AView 可选 EditorActions 服务保护、Console 测试接入正式输入资产及
Action/Command 链、日志队列断言更新、Sprite Artifact 穷举预期、静态插件导入宏修复、
SDK 共享宿主库部署及材质测试传递 DLL 部署。文件、职责和命令见上述全量报告。

全部 56 组 CTest 执行结束。原聚合完成 3,375 项后卡住，剩余 1,720 项按套件
隔离补跑，5,095 项原库存缺失集合为空。之后补跑并行新增 7 项，6 成功、1 失败。
收尾 Console/面板/渲染执行 301 项筛选复验为 300 成功、1 条件跳过；
完整 RenderFrameExecutionTests 在新进程中 213/213。

仍须修复测试重置共享 JobSystem 后未恢复执行环境的顺序污染，以及报告中的
资源/Prefab、VFX、Sprite 混合渲染和 ECS 失败，然后在稳定产物上重跑不中断的全量。
现有 InputManager、编辑器功能和测试断言没有通过删减来换取通过。
本机结果不覆盖可见人工交互、真实设备或 macOS/Linux 验收。
