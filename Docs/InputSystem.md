# Input System 使用说明

实施文件、阶段结果和平台缺口见 [实施记录](Plans/InputSystem-Implementation.md)。本文的 C# 使用入口支持 Inspector Action 引用、生成类型化接口和按名称查找；业务脚本不需要复制 GUID。

## 接入游戏

在 Asset Browser 中打开 `.inputactions` 进入 Input Editor。可以从
`App/Assets/Engine/Input/DefaultGameplay.inputactions` 复制一份到项目 Assets，
保留既有 Action/Binding ID；创建不同的逻辑动作时生成新 ID。

给玩家对象添加 `Input/Player Input` 组件并指定资产。组件为该玩家维护独立
InputUser，支持键鼠和指定手柄。Play Domain 随 Play Session 创建和销毁；
共享资产中不存储玩家按住状态或重绑定。

示例资源的 Jump Action ID 是 `172db044-427e-4b65-9042-9c645648c37f`，
Gameplay Context ID 是 `df500085-d399-4d61-b1ea-18e635e2291f`。
名称用于显示，代码和持久化关系使用 ID。

## C++

通过 `PlayerInputComponent::GetInputUser()` 获取用户，用
`FindAction("Gameplay/Jump")` 或 `FindAction(ActionId)` 缓存句柄，然后调用 `GetActionValue`、
`GetActionState`、`WasActionPressed` 或 `WasActionReleased`。
定义热重载会更换 generation；重新获取句柄后再查询。

```cpp
auto user = playerInput.GetInputUser();
auto handle = user->FindAction("Gameplay/Jump");
bool pressed = user->WasActionPressed(handle);

// 保存 token 为所属对象的成员；析构或 Reset 会取消订阅。
auto token = user->BindAction("Gameplay/Jump", NLS::Input::ActionPhase::Performed,
    [](const NLS::Input::ActionEvent& event) { /* 消费所属游戏行为 */ },
    NLS::Input::UpdateMode::Fixed);
```

宿主已负责 `InputDomain::Update` 和 `BeginFixedTick`，组件不再自行推进输入。
Update 查询当前 Snapshot；需要保留多次短按时订阅事件或遍历
`GetActionEvents()`。FixedUpdate 查询 `GetFixedFrame()` 的有序事件和按索引
对应当前定义的 Action Snapshot。连续 Rate 乘本次 FixedDeltaTime，Delta
只使用一次；不要把鼠标位移和手柄速度意图直接比较幅值。

资源/Profile 更新由上层在 `InputDomain::EnqueueChange` 安排到下一输入边界。
纯 C++ 独立使用 InputUser 时，调用者负责在安全边界修改配置。变更接口由宿主线程调用；工作线程使用已提交的 Snapshot 副本，EnqueueChange 不是跨线程队列。

旧 `InputManager` 提供 `IsKeyDown`、`WasKeyPressed`、`WasKeyReleased`、
`GetMousePosition/Delta/Wheel` 和 `GetGamepadState`。这些查询读取统一缓存，
不会启动第二套 OS 轮询。一个宿主只创建一个平台采集器，子窗口登记到它。

## C#

### Inspector Action 引用

声明 `[SerializeField] InputActionReference`，Inspector 会显示项目 `.inputactions` 中的 Action 选择器。也可以拖入资产；包含多个 Action 时会继续选择具体动作。选择/清除通过现有 SerializedProperty/Undo 提交，保存为定义 ID 与 ActionId，不按显示名称持久化。改名不改变引用；丢失动作显示 Missing，并保留原引用供修复。

把此脚本和 Player Input 放在同一个玩家对象上，使用组件的 user 解析到该玩家：

```csharp
using System;
using Nullus;

public sealed class JumpInput : Behaviour
{
    [SerializeField] private InputActionReference jump = new();
    private IDisposable? subscription;

    private void Start()
    {
        var player = GetComponent<PlayerInput>();
        subscription = jump.Resolve(player.user)?.BindAction(this, InputActionPhase.Performed,
            value => { /* 使用 value.Tick / Value 驱动游戏行为 */ },
            mode: InputUpdateMode.Fixed);
    }
    private void OnDestroy() => subscription?.Dispose();
}
```

单玩家可以使用 `jump.action`，它读取主用户。多人时使用 `jump.Resolve(player.user)`；可共享同一个只读引用，各自读取独立状态。引用不自行创建输入用户或加载另一套定义，Player Input 必须已指定对应资产。Default Context 由组件启用，Action 引用本身不改变 Context 的消费和启停策略。

### 生成 C# 接口

在 Input Editor 点击 **Generate C# Class**。`PlayerControls.inputactions` 会在旁边生成 `PlayerControls.inputactions.g.cs`，提供 `PlayerControlsActions`。之后通过 Input Editor 保存该资产时自动更新已生成文件；从外部修改资产后再次点击生成。生成器不覆盖非 Nullus 生成的用户代码，名称冲突会给出诊断。

```csharp
// 在 Start 中初始化；Player Input 此时已创建用户并应用资产。
controls = new PlayerControlsActions(GetComponent<PlayerInput>().user);
var move = controls.Gameplay.Move.ReadValue<Vector2>();
bool jumpPressed = controls.Gameplay.Jump.WasPressedThisFrame();
```

`controls.Gameplay.Enable()/Disable()` 使用该玩家的 Context。生成包装不创建输入运行时、不开启第二个设备采集器，也不拥有 Player Input 的生命周期。生成文件中的 GUID 是工具维护的资源身份。

### 按名称查找

```csharp
var actions = GetComponent<PlayerInput>().actions;
var jump = actions.FindAction("Gameplay/Jump", throwIfNotFound: true);
var move = actions["Gameplay/Move"];
```

在初始化时查找并缓存。名字可以是 Action 名称、`Context/Action` 或 ID；ASCII 大小写不敏感。重名且无法消歧时 FindAction 返回 null，索引器/throwIfNotFound 抛出异常。查找结果保存稳定 ID，之后改名仍可使用。`ReadValue<bool/float/Vector2/Vector3>()`、`IsPressed()`、`WasPressedThisFrame()` 和 `WasReleasedThisFrame()` 提供常用读取方式，原有 GetState/GetValue/BindAction 保留兼容。

Snapshot 边沿只表示本次更新是否发生过按下/释放。需要逐个处理同一更新内的多次输入时继续使用 BindAction；物理命令订阅使用 `InputUpdateMode.Fixed`。

`InputDevice.All` 枚举物理设备，提供 Id、Kind、IsConnected 和 IsMapped；缓存 ID 后可交给 InputUser.PairDevice。`InputUser.All` 枚举当前玩家；省略 user 使用主玩家。`InputAction.GetState`
和 `GetValue` 支持 Dynamic/Fixed；`InputUser` 提供 Context、设备配对、
绑定显示名、重绑定和 Profile 保存/加载。对象销毁、脚本重载和 Stop 会
清理旧订阅；禁用期间的事件不会在重新启用时补发。重绑定会话使用 Dispose/取消结束，ConflictCount 查询当前候选的潜在冲突；定义或 Profile 改变会取消尚未应用的捕获结果。

## Lua

`Nullus.Input.GetActionValue(id, user, fixed)` 返回状态表，
`WasActionPressed/WasActionReleased` 提供便捷查询。
`BindAction(owner, id, phase, callback, user, fixed)` 返回可 `Dispose()` 的
token，owner 为脚本对象；phase 可以使用 Nullus.Input.ActionPhase 的命名常量，其原生顺序为
（Started=0、Pressed=1、Released=2、Performed=3、Canceled=4、ValueChanged=5）。
查询也接受 `Gameplay/Jump` 这样的名字路径。需要长期订阅、重命名兼容时使用稳定 ID；当前类型化代码生成面向 C#。
`PushContext/PopContext/EnableContext/DisableContext`、
`RestoreBindingDefault/ClearBinding`、`SaveProfile/LoadProfile` 作用于同一原生用户。
GetDevices/GetUsers 提供设备/用户枚举；设备条目包含 id、kind、connected、mapped，PairDevice/UnpairDevice 使用该 id。BeginRebinding/GetRebindingStatus/GetRebindingConflictCount/ApplyRebinding/CancelRebinding 管理重绑定 token；用完应取消或应用。Lua 数字 token 的整数精度上限为 2^53-1。

## 编辑器命令与配置

通过现有 `EditorShortcutService` 注册 CommandId 和业务回调，通过稳定
Action/Binding ID 定义输入。菜单、工具栏和快捷键调用
`TryExecuteCommand`，显式内容目标放在 InvocationContext 中。
不要增加局部 `ImGui::IsKeyPressed` 业务扫描。普通文本框/列表等控件仍
使用 ImGui 自己的输入处理。

默认编辑器映射位于 `App/Assets/Editor/Input/EditorShortcuts.inputactions`。
Command 只登记业务元数据、CanExecute 和回调；默认键位、备用键和 Repeat
只在该资源中配置，部署 Editor 时必须携带该文件。导航是连续 Action 的设置项，
不会注册一个空的业务 Command。

独立宿主先调用 `InitializeInput`，再 `LoadDefaults`（或 `SetInputDefaults`），
最后 `LoadProfile(newProfilePath, legacyJsonPath)`。旧的 ExecuteShortcut、
ExecutePressedShortcut、ExecutePressedGlobalControls 轮询入口已删除。
`GetBindingSlots(CommandId)` 返回当前资源的稳定 BindingId，设置界面的数值
槽位仅表示当前显示顺序，不作为持久化身份。

用户配置只写项目 `UserSettings/editor.inputprofile`；旧 `shortcuts.json`
在新 Profile 不存在时导入并保留原文件；SaveProfile 只接受 .inputprofile。
旧配置冲突的提案保留为 unresolved，可在公共重绑定接口中解决；导入诊断通过
GetProfileDiagnostics 查询。Editor 与 Gameplay Profile 分开保存。

每个槽位区分 Default、Override、ExplicitlyUnbound。恢复默认与清除绑定是
不同操作，设置面板 Undo 保存完整三态。有效按键提示调用
`Windowing::Inputs::GetBindingDisplayName` 或 `InputUser.GetBindingDisplayName`。
重绑定先等待启动控件释放，忽略轴噪声，不捕获启动按钮本身。

编辑器相机与自定义画布也通过同一资源和 Profile 配置。默认资源现在包含
144 个 Action、166 个稳定 Binding 槽位；既有 Action/Binding ID 保持不变。
`camera-navigation.*` 配置飞行、平移、Alt+中键环绕、加速、指针 Delta 和滚轮缩放；
`scene-view.*` 配置临时吸附、选择修饰和手势取消；UI Designer 的平移、滚轮模式、
吸附绕过、等比缩放及选择修饰，以及 VFX 时间轴缩放也使用 Action。

Editor C++ 消费者通过 `Shortcuts::GetEditorInputState(EditorInputAction::...)`
读取缓存的 Snapshot。每个正常宿主帧先执行 `PrepareInput`；准备前的焦点查询
返回中立状态，不提前消费仍属于旧路由的当前帧事件。独立的导航 InputUser 在
准备阶段计算这些状态，不执行业务 Command；实际视口/画布仍校验当前窗口、
遮挡和指针所有权。
同帧晚到事件刷新保留原始 Pressed 并累计新的 Delta，不因重复查询重新累计。
所有权重新开放时，恢复的 held 值不能自行发起指针捕获；正常 Hold 计时仍可在
无新设备事件的更新中推进。配置重载或重绑定会刷新句柄和 Snapshot 缓存。

Scene 手势取消统一执行 `scene-view.cancel-gesture` Command。绘制阶段观察到取消
Action 后暂停本次手势提交，当前 UI 路由完成后由 Command 取消并恢复编辑状态；
不再由物理 Escape 检测器与 Command 各取消一次。Asset Browser 的删除等待释放
查询删除 Action 状态，支持更换 Delete 的绑定。

文本改名、列表选择、弹窗选择项等 ImGui 控件继续使用其原生输入；窗口重绘、
指针坐标、控件拖拽和捕获释放等底层查询保留在统一 InputManager/ImGui 路径中。

## 调试和回放

Input Editor 的调试页显示设备、Context、Action、epoch、Fixed Tick、
待消费事件、有序事件和未应用 Override。无设备测试使用
`InputSystem::SubmitEvent` 和注入的单调 Clock。

宿主正常事件泵结束时调用 `FinishEventPump`，再更新输入用户，最后调用
`EndFrame`。帧尾只移除事件泵结束前的记录；保存等命令的原生对话框期间
收到的新事件及其按键边沿、Delta、Wheel 留给下一次更新。不能在回调后
直接清空整个队列，否则释放事件会丢失，后续按下无法产生新边沿。

`InputReplayRecord` / `ReplayInput` 重放完整的类型化日志，包括初始定义、
Profile、设备/窗口登记、原始事件、Context/取消/暂停、更新时间和 Tick。
从新 Play epoch 记录，保留零步 Update、无新设备事件的 Update 和
FinishEventPump 和 EndFrame；只保存最终按住状态不足以复现 Hold/Tap 或多次短按。
当前接口面向 C++ 自动化与诊断，不提供任意时刻的 Interaction 内存快照。

## Release 性能与堆分配检查

`Tests/InputPerformance` 提供可独立构建的 `NullusInputBenchmark` 和现有核心
行为测试的 Release 目标。构建与复现命令见该目录的 README。主工程开启
`NLS_BUILD_PERFORMANCE_TESTS` 时也包含基准；分配检查仅在优化配置中运行。

基准静态链接相同生产输入源码，避免仅在可执行文件替换 `operator new` 时
漏掉 DLL 内分配。计时与分配分别采样；计数器覆盖当前线程的 C++ 分配，
不代表平台、其他 DLL、其他线程或整个编辑器的 OS 堆统计，也不替代泄漏检查。
热路径在已预热容量内要求零分配；定义编译、Override 和容量增长单独记录。
不通过截断事件缓冲或丢弃 Fixed 命令来维持零分配。

高频查询缓存 `ActionHandle`；按名称查找用于初始化，并在定义 generation
变化后刷新句柄。变更队列在安全边界执行后复用已有容量；回调分发仅为实际
匹配的订阅保持临时所有权，仍支持回调内退订、注册和 Context 修改。

## 全量回归状态

2026-10-30 的 Windows Debug 全量执行及修复记录见
`Docs/Reports/InputSystem-FullRegression-2026-10-30.md`。
输入五个专项目标 199/199 成功；全部 56 组 CTest 原始 49 通过、7 失败，
其中 3 个目标已修复并正式复验通过，4 个仍开放。
聚合库存和并行新增共 5,102 项均有执行记录，但全局 JobSystem 状态污染导致的
聚合卡住尚未关闭；隔离补跑不代表完整验收通过。
可见编辑器、真实输入设备及 macOS/Linux 验收仍单独记录。
