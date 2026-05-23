# Research: Editor Top And Bottom Bars

**Feature**: `010-editor-top-bottom-bars`
**Date**: 2026-05-03

## Decision 1: Build top and bottom bars on a fixed viewport-anchored panel abstraction

**Decision**: 为 Editor 顶栏和脚栏新增一层固定锚定主视口边缘的 UI 面板抽象，而不是继续使用普通 `PanelWindow` 或把脚栏硬塞进 `PanelMenuBar`。

**Rationale**:

- 当前 `PanelMenuBar` 只封装 `ImGui::BeginMainMenuBar()`，适合主菜单，但不适合作为底部脚栏的统一承载。
- 当前 `Toolbar` 和 `FrameInfo` 都是普通 `PanelWindow`，具备可停靠/可移动语义，和“固定贴边栏位”目标不一致。
- Editor 当前使用 `Canvas` + dockspace，顶栏/脚栏如果仍作为普通窗口，会在布局重置、resize 或停靠时丢失“全局固定栏位”的产品语义。

**Alternatives considered**:

- 继续让 `Toolbar` 作为普通可停靠窗口存在，只调整默认位置。
  Rejected because: 这无法满足“固定顶栏”的要求，且用户仍可能把它拖走。
- 仅复用 `PanelMenuBar` 处理顶栏，脚栏单独做临时窗口 hack。
  Rejected because: 会让顶栏和脚栏走两套完全不同的承载规则，后续扩展成本更高。

## Decision 2: Reuse existing editor actions and SceneView state instead of inventing a new editor-mode system

**Decision**: 顶栏中的播放控制和核心场景编辑模式优先复用现有 `EditorActions`、`SceneView`、`Toolbar` 和 `MenuBar` 的行为与状态来源。

**Rationale**:

- `Toolbar` 已经接通 `StartPlaying`、`PauseGame`、`StopPlaying`、`NextFrame`，并监听 `EditorModeChangedEvent` 更新按钮启用状态。
- `SceneView` 已经维护当前 gizmo operation，并通过 `W/E/R` 在 `TRANSLATE/ROTATE/SCALE` 间切换。
- 通过整合已有状态源，可以把此次工作聚焦在 UI 入口重组，而不是扩散为大范围编辑器状态重构。

**Alternatives considered**:

- 新建独立的顶栏状态管理器，复制一份播放与编辑模式状态。
  Rejected because: 容易与现有 `EditorActions`/`SceneView` 双源失同步。
- 直接在顶栏里内联新逻辑，不复用现有 `Toolbar`/`MenuBar` 结构。
  Rejected because: 会增加回归风险，并且破坏现有命令入口的单一职责。

## Decision 3: Treat Unity alignment as workflow alignment, not full ecosystem parity

**Decision**: 将“和 Unity 对齐”解释为顶栏工作流布局、核心操作分区和可辨识状态表达对齐，而不是要求复制 Unity 的账户、云服务、布局下拉和完整生态入口。

**Rationale**:

- 当前 Nullus 仓库已经具备主菜单、播放控制和场景变换模式这些核心可映射能力。
- 规格已明确首版范围仅包含顶栏和 FPS 脚栏；若把 Unity 生态能力一并纳入，会显著扩大范围且缺乏底层行为支持。
- 对暂不支持入口使用禁用态或省略展示，更符合用户预期，也避免产生“点了没反应”的伪可用体验。

**Alternatives considered**:

- 强行把 Unity 顶栏上的所有视觉槽位都渲染出来。
  Rejected because: 会产生大量没有实际行为支撑的假入口。
- 完全忽略 Unity 风格，只做一个普通 Nullus 自定义工具栏。
  Rejected because: 无法满足用户关于 Unity 对齐的直接需求。

## Decision 4: Source FPS from the main frame timing path, not from the existing FrameInfo panel

**Decision**: 脚栏 FPS 直接使用现有主循环/时钟提供的帧时间信息进行展示，而不是依赖 `FrameInfo` 窗口存在或把 `FrameInfo` 缩减后嵌入脚栏。

**Rationale**:

- `FrameInfo` 当前是面向聚焦视图的独立统计窗口，承担的内容远多于 FPS，并且是否更新取决于该窗口是否打开。
- 脚栏要求“始终可见、轻量、持续更新”，更适合直接绑定全局帧时间来源。
- `Runtime/Base/Time/Clock` 已存在 `GetFramerate()` 能力，Editor 主循环本身也以每帧 deltaTime 驱动更新，可形成稳定的脚栏读数。

**Alternatives considered**:

- 直接复用 `FrameInfo` 面板并强制停靠到底部。
  Rejected because: 它的职责过重，且当前语义是普通窗口而不是全局状态栏。
- 仅显示 `ImGui::GetIO().Framerate`。
  Rejected because: 该值虽然可用，但从规划上更希望脚栏绑定引擎主循环的帧时间语义，而非局限于 ImGui 自身统计。
