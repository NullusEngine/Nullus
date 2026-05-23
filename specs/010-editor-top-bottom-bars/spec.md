# Feature Specification: Editor Top And Bottom Bars

**Feature Branch**: `[010-editor-top-bottom-bars]`
**Created**: 2026-05-03
**Status**: Draft
**Input**: User description: "给编辑器增加一个顶栏和脚栏，顶栏的功能和unity对齐。底栏暂时就只要显示帧率就行"

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Use A Unified Editor Top Bar (Priority: P1)

作为编辑器用户，我希望启动编辑器后就能在窗口顶部看到一条固定的顶栏，把常用菜单和核心编辑操作集中到一个接近 Unity 的位置，这样我不需要依赖分散的浮动窗口来完成基础操作。

**Why this priority**: 顶栏是编辑器主工作流入口，直接影响场景编辑、播放控制和基础命令发现效率；没有统一顶栏时，整体工作流会显得割裂。

**Independent Test**: 启动编辑器并打开任意项目后，用户无需额外打开其他窗口，即可从顶栏访问主菜单和核心编辑控制，并成功触发相应行为。

**Acceptance Scenarios**:

1. **Given** 用户打开编辑器并进入主界面，**When** 编辑器完成首帧显示，**Then** 窗口顶部显示一条固定顶栏，且不会被普通内容面板替代或挤出可见区域。
2. **Given** 用户需要访问文件、窗口、资源或帮助类命令，**When** 用户查看顶栏菜单区域，**Then** 用户可以从统一的顶部菜单入口访问这些命令，而不需要寻找独立菜单窗口。
3. **Given** 用户需要执行进入播放、暂停、停止或单步推进等常见操作，**When** 用户查看顶栏主工具区域，**Then** 用户可以在顶栏中直接找到这些控制项并触发对应动作。

---

### User Story 2 - Adjust Scene Editing Mode From The Top Bar (Priority: P2)

作为进行场景编辑的用户，我希望顶栏提供与 Unity 使用习惯一致的核心场景编辑控制入口，例如变换操作模式和坐标参考模式，这样我可以在熟悉的位置快速切换当前编辑语义。

**Why this priority**: 这类控制项是 Unity 风格顶栏的关键识别特征，也是高频场景编辑动作的上下文入口；它们决定顶栏是否真正服务于编辑工作流，而不只是一个菜单容器。

**Independent Test**: 在场景编辑状态下，用户仅通过顶栏即可识别当前核心编辑模式，并完成模式切换，切换结果会立即反映到后续编辑行为中。

**Acceptance Scenarios**:

1. **Given** 用户处于场景编辑状态，**When** 用户查看顶栏工具区，**Then** 顶栏会显示当前可用的核心场景编辑模式入口，并清晰标识当前激活状态。
2. **Given** 用户需要从一种编辑模式切换到另一种编辑模式，**When** 用户在顶栏执行切换，**Then** 编辑器应立即更新当前模式，并在后续场景交互中使用新模式。
3. **Given** 某些 Unity 顶栏功能当前版本尚不支持，**When** 用户查看对应区域，**Then** 编辑器应以清晰且一致的方式表明该功能暂未提供，而不是展示误导性的可操作入口。

---

### User Story 3 - Read Frame Rate From A Bottom Status Bar (Priority: P3)

作为编辑器用户，我希望窗口底部有一条固定脚栏，并且当前版本至少持续显示帧率，这样我在编辑过程中可以随时感知运行流畅度，而不必额外打开统计面板。

**Why this priority**: 帧率读数是最直接的运行状态信号，脚栏提供了轻量、持续可见的反馈通道，也为后续扩展更多状态信息预留了稳定位置。

**Independent Test**: 打开编辑器并保持任意编辑视图可见时，用户无需打开附加窗口即可在底部持续看到帧率信息；在运行一段时间后，该信息仍保持可读并随帧变化更新。

**Acceptance Scenarios**:

1. **Given** 用户打开编辑器主界面，**When** 窗口底部进入可见状态，**Then** 用户可以看到一条固定脚栏，且脚栏中至少包含当前帧率信息。
2. **Given** 编辑器正在正常渲染界面，**When** 用户持续观察脚栏，**Then** 帧率读数会随运行状态更新，而不是永久停留在初始化占位值。
3. **Given** 用户调整窗口大小或切换常见工作面板，**When** 顶栏和脚栏重新参与布局，**Then** 帧率信息仍保持可见且不与主要内容区域发生重叠。

---

### Edge Cases

- 当编辑器窗口宽度缩小时，顶栏必须优先保持核心命令和当前状态可辨识，不能因为空间不足而让主操作区完全消失。
- 当某些对齐 Unity 的目标功能在当前版本没有对应能力时，界面必须明确区分“可用”“不可用”“暂未实现”，避免用户误判。
- 当编辑器刚启动、项目刚载入或视图暂未稳定输出时，脚栏中的帧率显示必须使用可理解的初始状态，而不是空白或错误数值。
- 当用户切换播放状态或切换主要视图焦点时，脚栏帧率显示必须继续存在，不能因为焦点变化而消失。

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: System MUST provide a persistent top bar anchored to the top edge of the editor window across normal editing workflows.
- **FR-002**: System MUST consolidate the editor's primary menu access into the top bar so users can reach common editor-level commands from a Unity-like top menu location.
- **FR-003**: System MUST provide a primary tool region in the top bar for high-frequency editor controls, including play-state controls and core scene editing mode controls.
- **FR-004**: System MUST visually indicate which top bar controls are currently active, inactive, or unavailable so users can understand the current editing state at a glance.
- **FR-005**: Users MUST be able to trigger the editor's existing play-related actions from the top bar without opening a separate toolbar window.
- **FR-006**: Users MUST be able to switch among the editor's supported core scene editing modes from the top bar.
- **FR-007**: System MUST present coordinate/reference mode choices in the top bar when those choices are supported by the editor's current scene editing workflow.
- **FR-008**: System MUST keep unsupported Unity-inspired top bar affordances clearly non-interactive or clearly absent, rather than implying full parity where no corresponding behavior exists.
- **FR-009**: System MUST provide a persistent bottom status bar anchored to the bottom edge of the editor window.
- **FR-010**: System MUST display current frame rate information in the bottom status bar during normal editor operation.
- **FR-011**: System MUST keep top bar and bottom status bar visible and non-overlapping with the main content region during common window resizing and panel layout changes.
- **FR-012**: System MUST preserve existing editor commands and workflows when relocating or merging current menu and toolbar entry points into the new top bar.

### Key Entities *(include if feature involves data)*

- **Top Bar Region**: 编辑器顶部的固定全局操作区域，承载主菜单、主工具入口、当前编辑模式状态以及对齐 Unity 习惯的核心控制布局。
- **Top Bar Command Group**: 顶栏中的一组相关操作入口，例如菜单命令组、播放控制组、场景编辑模式组或参考模式组，每组都需要有明确的位置与状态表达。
- **Bottom Status Bar**: 编辑器底部的固定状态区域，当前版本负责持续显示帧率，并为后续状态信息扩展提供统一承载位置。
- **Frame Rate Indicator**: 脚栏中的实时性能读数，用于向用户表达当前编辑器运行流畅度。

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: 在全新打开编辑器后的 10 秒内，用户无需打开额外工具窗口即可定位到顶部主菜单、播放控制和场景编辑核心模式入口。
- **SC-002**: 至少 90% 的常见编辑会话中，用户可以直接从顶栏完成进入播放、暂停、停止或单步推进等基础流程，而无需依赖旧的独立工具栏窗口。
- **SC-003**: 用户在任意常规编辑布局下都能持续看到底部帧率信息，并且窗口尺寸变化后该信息在 1 秒内恢复到稳定可读状态。
- **SC-004**: 对当前版本未支持的 Unity 顶栏能力，界面不会将其误表现为可执行功能，从而避免“点了没反应”的伪可用交互。

## Assumptions

- 本次“和 Unity 对齐”默认指向 Unity 顶栏中的核心编辑工作流布局与交互习惯，而不是要求逐项复刻 Unity 的所有品牌入口、云服务入口或账户体系入口。
- 当前版本只要求脚栏显示帧率，不要求同时展示内存、绘制统计、后端信息或项目状态等其他状态项。
- 顶栏会优先承载 Nullus 已具备或可直接映射的编辑器能力；对尚无底层行为支持的 Unity 风格入口，本次以清晰的非可交互表达或不展示处理。
- 现有独立菜单栏、工具栏或统计入口可以被重组、替换或弱化，但用户已能完成的核心工作流不应因此丢失。
