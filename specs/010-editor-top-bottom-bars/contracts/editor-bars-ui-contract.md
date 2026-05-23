# Contract: Editor Top And Bottom Bars UI

**Feature**: `010-editor-top-bottom-bars`
**Date**: 2026-05-03

## Purpose

定义 Nullus Editor 顶栏与脚栏在首版中的布局职责、交互边界和状态表达契约，确保实现过程不会把固定栏位退化回普通浮动窗口，也不会把未实现的 Unity 风格入口错误地暴露成可用功能。

## Top Bar Contract

### Placement

- 顶栏 MUST 固定锚定在主编辑器窗口顶部。
- 顶栏 MUST 在默认编辑流程中始终可见。
- 顶栏 MUST NOT 依赖普通停靠窗口位置才能被访问。

### Content

- 顶栏 MUST 包含主菜单入口。
- 顶栏 MUST 包含播放控制入口。
- 顶栏 MUST 包含当前支持的核心场景编辑模式入口或状态展示。
- 顶栏 SHOULD 按接近 Unity 的认知分区组织内容：菜单区、主工具区、状态区。

### Behavior

- 顶栏中的既有命令入口 MUST 保持原有行为语义不变。
- 顶栏中的播放控制 MUST 与当前编辑器模式联动，正确表达可用与不可用状态。
- 顶栏中的场景工具状态 MUST 反映当前编辑模式，而不是仅显示静态图标。
- 对当前版本无底层行为支撑的 Unity 风格入口，界面 MUST 使用禁用态或直接省略。

## Bottom Status Bar Contract

### Placement

- 脚栏 MUST 固定锚定在主编辑器窗口底部。
- 脚栏 MUST 在默认编辑流程中始终可见。
- 脚栏 MUST NOT 遮挡主编辑内容区域。

### Content

- 脚栏首版 MUST 至少显示一个 FPS 状态项。
- 脚栏 MAY 在未来扩展更多状态项，但本契约版本不要求实现。

### Behavior

- FPS 状态 MUST 在编辑器正常运行期间持续更新。
- 启动初期如尚未形成稳定帧时间，脚栏 MUST 显示可理解的初始化状态，而不是空白。
- 窗口 resize、布局恢复和主视图切换后，FPS 状态 MUST 保持可见。

## Layout And Resize Contract

- 顶栏与脚栏 MUST 与 dockspace 主内容区共存。
- 顶栏与脚栏 MUST 在窗口 resize 后继续贴附上下边缘。
- 当窗口宽度缩小时，实现 MUST 优先保留核心入口的可辨识性。
- 顶栏与脚栏 MUST NOT 依赖某个单一图形后端专有分支才能维持布局正确性。

## Backward Compatibility Contract

- 现有主菜单命令 MUST 保持可达。
- 现有播放控制行为 MUST 保持可达。
- 如果旧的独立 `Toolbar` 或 `FrameInfo` 仍保留，职责 MUST 与新栏位清晰区分，避免重复入口导致混淆。
- 如果旧入口被弱化或默认隐藏，用户已能完成的核心工作流 MUST 仍可通过新栏位完成。
