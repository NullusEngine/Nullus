# Data Model: Editor Top And Bottom Bars

**Feature**: `010-editor-top-bottom-bars`
**Date**: 2026-05-03

## Entities

### EditorTopBarRegion

编辑器顶部的固定全局栏位区域，负责统一承载菜单、主工具和核心编辑状态。

| Field | Type | Description | Validation |
|-------|------|-------------|------------|
| anchorEdge | enum (`Top`) | 锚定边缘 | 固定为顶部 |
| visibility | enum (`Visible`, `HiddenByStartupFailure`) | 顶栏当前可见状态 | 正常编辑流程中必须为 `Visible` |
| menuSectionPresent | bool | 是否包含主菜单区 | 必须为 true |
| toolSectionPresent | bool | 是否包含主工具区 | 必须为 true |
| stateSectionPresent | bool | 是否包含当前编辑状态区 | 首版至少在支持功能时可见 |

**Relationships**:

- 包含多个 `TopBarCommandGroup`
- 读取 `PlaybackModeState`
- 读取 `SceneToolState`

**State**:

`Visible` 是常态；仅在编辑器未完成正常 UI 启动时允许短暂不可用。

---

### TopBarCommandGroup

顶栏中一组语义一致的操作集合，用于表达菜单命令、播放控制或场景工具切换。

| Field | Type | Description | Validation |
|-------|------|-------------|------------|
| groupId | string | 命令组唯一标识 | 非空，唯一 |
| displayName | string | 组名或可见标签 | 可为空，仅图标组时允许 |
| interactionType | enum (`Menu`, `MomentaryAction`, `Toggle`, `Selection`) | 该组的交互类型 | 必须是受支持的类型 |
| availability | enum (`Available`, `Disabled`, `Hidden`) | 命令组当前可用性 | 不允许使用“看似可用但无行为”的未定义状态 |
| activeItemId | optional string | 当前激活项标识 | 对 `Toggle`/`Selection` 类组可选存在 |

**Relationships**:

- 属于一个 `EditorTopBarRegion`
- 可映射到一个或多个 `EditorCommandBinding`

---

### EditorCommandBinding

UI 入口与现有编辑器行为之间的绑定定义，保证迁移入口时不改变命令语义。

| Field | Type | Description | Validation |
|-------|------|-------------|------------|
| bindingId | string | 绑定唯一标识 | 非空，唯一 |
| sourceGroupId | string | 所属命令组标识 | 必须引用存在的 `TopBarCommandGroup` |
| actionKind | enum (`PlayAction`, `MenuAction`, `SceneToolAction`, `StatusOnly`) | 绑定的行为类别 | 必须匹配已有编辑器能力 |
| enabled | bool | 当前是否允许触发 | 不可用时必须与 UI 状态一致 |

**Relationships**:

- 多个绑定属于一个 `TopBarCommandGroup`
- 绑定到现有 `EditorActions` 或 `SceneView` 相关状态/行为

---

### PlaybackModeState

编辑器播放态的展示模型，用于驱动顶栏播放控制区的启用与激活表达。

| Field | Type | Description | Validation |
|-------|------|-------------|------------|
| mode | enum (`EDIT`, `PLAY`, `PAUSE`, `FRAME_BY_FRAME`) | 当前编辑器播放状态 | 必须来自现有 Editor 模式 |
| canPlay | bool | 当前是否允许进入播放 | 与模式规则一致 |
| canPause | bool | 当前是否允许暂停 | 与模式规则一致 |
| canStop | bool | 当前是否允许停止 | 与模式规则一致 |
| canStep | bool | 当前是否允许单步 | 与模式规则一致 |

**State Transitions**:

`EDIT -> PLAY -> PAUSE -> FRAME_BY_FRAME`

允许的切换必须与现有 `EditorActions` 语义一致，不新增未定义状态。

---

### SceneToolState

场景编辑核心工具状态，用于驱动顶栏中 Unity 风格的工具切换显示。

| Field | Type | Description | Validation |
|-------|------|-------------|------------|
| activeOperation | enum (`TRANSLATE`, `ROTATE`, `SCALE`) | 当前变换操作模式 | 必须来自已有 SceneView/gizmo 能力 |
| referenceMode | enum (`SupportedValue`, `Unsupported`) | 坐标/参考模式能力暴露状态 | 若暂未完整支持，必须明确为 `Unsupported` |
| statusPresentation | enum (`Interactive`, `Disabled`, `Hidden`) | 在顶栏中的展示方式 | 必须与底层能力一致 |

**Relationships**:

- 被 `EditorTopBarRegion` 读取
- 行为来源于 `SceneView`

---

### EditorBottomStatusBar

编辑器底部固定状态栏区域，首版只承载 FPS 状态。

| Field | Type | Description | Validation |
|-------|------|-------------|------------|
| anchorEdge | enum (`Bottom`) | 锚定边缘 | 固定为底部 |
| visibility | enum (`Visible`, `HiddenByStartupFailure`) | 脚栏可见状态 | 正常编辑流程中必须为 `Visible` |
| statusItems | list | 已挂载状态项列表 | 首版至少包含 1 个 FPS 状态项 |

**Relationships**:

- 包含一个 `FrameRateIndicatorState`

---

### FrameRateIndicatorState

脚栏中持续展示的帧率状态项。

| Field | Type | Description | Validation |
|-------|------|-------------|------------|
| displayLabel | string | 可见标签，如 FPS | 非空 |
| currentValue | float | 当前帧率值 | 不得为负 |
| presentationState | enum (`Initializing`, `Live`) | 显示状态 | 启动早期允许 `Initializing`，稳定后应转为 `Live` |
| lastUpdateSource | enum (`MainFrameTiming`, `FallbackTiming`) | 最近一次更新时间来源 | 首版目标为 `MainFrameTiming` |

**State Transitions**:

`Initializing -> Live`

若主循环已经产生稳定帧时间，则状态必须进入 `Live`。

## Validation Rules Summary

| Rule | Applies To | Description |
|------|------------|-------------|
| V-001 | EditorTopBarRegion | 正常编辑流程中必须存在且固定贴顶 |
| V-002 | TopBarCommandGroup.availability | 未实现能力不得伪装成可执行操作 |
| V-003 | EditorCommandBinding | 顶栏绑定后的命令语义必须与既有编辑行为一致 |
| V-004 | PlaybackModeState | 顶栏按钮启用态必须与现有编辑器模式一致 |
| V-005 | SceneToolState | 仅能暴露当前仓库实际支持的场景工具模式 |
| V-006 | EditorBottomStatusBar | 正常编辑流程中必须固定贴底且持续可见 |
| V-007 | FrameRateIndicatorState.currentValue | 帧率显示不得为空白或负值；初始化阶段也必须可理解 |
