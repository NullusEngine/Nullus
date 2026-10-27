# 编辑器工作区

编辑器是日常工作的主界面。它和游戏运行时共用同一份引擎代码，Scene View 里的渲染结果来自同一条渲染管线，不是另一套预览实现。

![Nullus 编辑器：Sponza 场景、Hierarchy、Inspector、变换 Gizmo 与 Asset Browser](../../Screenshots/editor.png)

## 工作区构成

| 区域 | 作用 |
| --- | --- |
| 顶部菜单栏 | File、Edit、Build、GameObjects、Resources、Window、Layout、Tools、Debugging、Help |
| 工具栏 | 变换工具切换（移动/旋转/缩放）、播放控制与常用操作 |
| 状态栏 | 当前项目、运行状态与提示信息 |
| Scene View | 编辑视角的视口，带 Gizmo、网格与调试绘制 |
| Game View | 运行态输出画面 |
| Hierarchy | 当前场景的对象树 |
| Inspector | 选中对象或资源的属性编辑 |
| Asset Browser | 项目资产浏览、搜索、拖放与预览 |
| Asset View | 资源预览面板，可打开材质球、模型与 Prefab 预览 |
| Console | 日志与脚本诊断 |
| Profiler / Frame Info | 性能与帧统计面板 |
| Project Settings | 项目级设置 |
| UI Document Designer | 可视化编辑 UI 文档 |

面板可以停靠、浮动与拖动重排。`Window` 菜单逐个开关面板，`Layout` 菜单提供 `Default`、`Tall`、`Wide`、`2 by 3`、`4 Split` 五套预设。

## 菜单

### File

- `New Scene`（Ctrl+N）、`Save Scene`（Ctrl+S）、`Save Scene As...`（Ctrl+Shift+S）
- `Exit`（Alt+F4）

### Edit

- `Undo` / `Redo`，菜单项会带上当前操作名，例如 `Undo Move GameObject`
- `Settings...`：打开项目设置
- `Shortcuts...`：打开快捷键面板

### Build

- `Build game`：构建游戏
- `Build game and run`：构建并运行
- `Temporary build`：临时构建

### GameObjects 与 Resources

`GameObjects` 提供创建对象的入口；`Resources` 提供 `Compile shaders` 与 `Save materials` 两个资源维护动作。

### Tools

`Tracy Profiler` 子菜单提供 `Open Tracy Profiler` 与 `Open Capture Folder`。构建未启用 Tracy 时这两项仍存在，但会提示目标不可用。

### Debugging

- `Debug Draw`：`Enabled`、`Grid`、`Bounds`、`Cameras`、`Lights` 开关
- `Wireframe Mode`
- `RenderDoc`：`Enabled`、`Capture Next Frame`（F11）、`Open Latest Capture`（Ctrl+F11）、`Open Capture Folder`、`Auto Open Replay UI`

### Help

`GitHub`、`Bug Report`、`Feature Request` 直接跳转到对应页面。

## 面板细节

### Hierarchy

显示当前场景的对象树，支持父子和选中状态展示、Prefab 连接状态着色，以及删除所选对象。Prefab 实例、Override、丢失引用与生成只读对象会以不同颜色标记，含义见[Prefab 工作流](../prefab/workflow.md)。

### Inspector

Inspector 由反射元数据驱动：字段类型决定控件，属性是否有 setter 决定是否可编辑，组件是否支持多实例决定能否重复添加。脚本组件按脚本自身类型名匹配自定义编辑器。

改动进入统一撤销栈，保存与重载是显式动作，不会因为拖动滑块而隐式写盘。

### Asset Browser

按文件夹、源资产与生成子资源组织内容，提供搜索、分类、图标、拖放、对象引用选择与缩略图预览。缩略图与预览是异步生成的，受每帧预算与缓存控制。

### Console

收集编译诊断、资产导入错误、脚本日志与运行时错误。日志接口在 [脚本 API 参考](../scripting/api.md) 中说明。

### Profiler 与 Frame Info

需要相应构建开关才能显示实时数据。启用方式与读数解释见[性能分析与图形调试](../quality/profiling.md)。

### Icon Browser

`Window > Icon Browser` 是开发调试窗口：按 ID/tag/alias 搜索，预览尺寸档位、交互状态与 Dark/Light 图标主题，过滤 Missing/Unused，查看纹理 generation 与加载诊断，并支持 `Reload editor icons` 热重载。运行时不读取 `Tools/Icons` manifest。

## 面板与脚本扩展

项目内的编辑器脚本可以注册自定义 Inspector、工具窗口与菜单项，注册结果会出现在 `Window` 和 `Tools` 菜单下。写法见[编辑器脚本扩展](../architecture/editor-extensions.md)。

## 下一步

- [视口操作与快捷键](interaction.md)：把常用操作变成肌肉记忆。
- [编辑器与项目设置](settings.md)：配置渲染、缓存与用户偏好。
