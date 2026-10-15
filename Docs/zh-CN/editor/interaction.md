# 视口操作与快捷键

本页覆盖 Scene View 的导航、变换 Gizmo、选取、撤销与播放控制。所有快捷键都可以通过 `Edit > Shortcuts...` 重新绑定。

## 视口导航

Scene View 使用编辑器相机，独立于场景中的 `CameraComponent`。移动编辑相机不会改动游戏相机，进入 Play 后 Game View 使用场景里的相机。

常用操作：

- 拖动鼠标右键旋转视角；
- 使用滚轮缩放；
- 用中键平移视角。

需要精确复现某个取景时，编辑器支持通过启动参数强制 Scene View 相机位姿和步进移动，用于批量验证与回归对比，参数见[目录结构与命令行](../reference/layout-cli.md#编辑器命令行)。

## 变换 Gizmo

工具栏提供三种变换模式，切换命令分别是 `editor.icon.toolbar.move`、`editor.icon.toolbar.rotate`、`editor.icon.toolbar.scale`：

| 模式 | 作用 |
| --- | --- |
| Move | 平移选中对象 |
| Rotate | 旋转选中对象 |
| Scale | 缩放选中对象 |

Gizmo 的改动写入对象 `TransformComponent`，并通过撤销栈记录。旋转以四元数保存，避免万向节累积误差。

## 选取

- 在 Scene View 中点击对象进行选取。
- 点击使用“命中代理”而不是直接读渲染结果，因此选中判定与渲染可见性解耦。
- 在 Hierarchy 中点击条目同样会同步选中状态。

选取与 Gizmo 共享同一份选中集合，Inspector 始终显示当前选中对象。

## 撤销与重做

| 命令 | 默认绑定 |
| --- | --- |
| `edit.undo` | Ctrl+Z |
| `edit.redo` | Ctrl+Y，或 Ctrl+Shift+Z |
| `edit.delete-selected-gameobject` | 删除所选对象 |
| `edit.delete-selected-gameobject-hierarchy` | 删除所选对象及其层级 |

撤销服务记录操作名，`Edit` 菜单会显示成 `Undo Move GameObject` 这样的可读标签。撤销覆盖场景对象增删改、Inspector 字段编辑、Prefab Override 与资产文件操作。

## 播放控制

| 命令 | 默认绑定 |
| --- | --- |
| `editor.play` | F5 |
| `editor.stop` | Escape |

`editor.stop` 只在运行态可用，编辑态下不可用。进入 Play 后脚本按 `Awake`、`OnEnable`、`Start`、`Update` 顺序执行；停止时按 `OnDisable`、`OnDestroy` 收尾。语义细节见[生命周期与激活状态](../scene/lifecycle.md)。

## 图形调试快捷键

| 命令 | 默认绑定 | 说明 |
| --- | --- | --- |
| `debug.renderdoc.capture-next-frame` | F11 | 抓取下一帧 |
| `debug.renderdoc.open-latest-capture` | Ctrl+F11 | 打开最近一次抓取 |

两项都需要 RenderDoc 处于可用状态，否则不会执行。配置与命令行用法见[性能分析与图形调试](../quality/profiling.md)。

## 显示开关

`Debugging > Debug Draw` 控制辅助绘制：`Grid`、`Bounds`、`Cameras`、`Lights`。`Wireframe Mode` 切换线框显示。这些开关只影响编辑器视口，不改变最终构建的渲染结果。

## 自定义快捷键

`Edit > Shortcuts...` 打开快捷键面板。面板由命令注册表驱动：

- 每个命令有稳定 ID、显示名、分类与上下文；
- 可以设置单个绑定、追加备用绑定、清除绑定或标记为未分配；
- 提交时会校验冲突，全局命令之间重复绑定会被拒绝；
- 配置以版本化 profile 保存，默认 profile 名为 `Default`。

## 下一步

- [编辑器与项目设置](settings.md)：调整视口缓存、渲染与调试选项。
- [调试与测试](../quality/debugging.md)：把编辑器行为变成可复现的检查。
