# Niagara 对齐差距审计（2026-10-19）

当前粒子编辑器尚未达到 UE Niagara 的界面、交互或完整核心功能对齐要求。反射属性、CPU/GPU 基础运行和部分模块已经实现；这些成果不能视为 UE 编辑器已经完成。

## 审计口径与证据边界

- 基准：本机 `D:/Epic Games/UE_5.7/Engine/Build/Build.version`，UE **5.7.4 / CL 51494982**；已重新读取确认。
- Nullus：`D:/Code/Nullus` 当前未提交工作区；HEAD 为 `0b154c12219f45a4acb71d3d50e5125abbeb7076`，实际审计包括大量未提交 VFX 源码，不能用 HEAD 单独复现。
- 参考图：用户附件 `codex-clipboard-0ffc74be-8689-4a21-a80b-d7e5b2ba65eb.png`；Nullus 图：`codex-clipboard-19a336da-ea60-45f7-a194-1e54ccf72e9d.png`。本轮已查看两张原图。
- 两图的语言、窗口尺寸、工作区状态及资产内容不同，DPI 未记录；不能由它们给出 1 DIP 几何验收或粒子数值/画面一致性结论。白色 Fountain 与橙色默认粒子本身不构成已证实的渲染错误。
- Nullus 图的 Details 外观与最新共享 Inspector 源码不同，截图所用二进制版本未核实。“图”表示截图可见问题，不能直接当成最新代码回归。
- “缺”：当前 VFX 创作/运行路径缺失或尚未接入；“部分”：存在有效子集，但尚有列出的缺口；“待验”：实现或基础机制存在，缺安装版 UE 对照证据；“图”：仅确认于附件。
- Nullus 原生坐标、单位、ImGui 技术体系、`.vfx` 格式及保留旧源文件的拒绝编辑策略是已同意的设计，不列为缺陷。Fluids、Nanite、MRQ 独立扩展排除在本轮范围外。
- 唯一默认入口和旧编辑器移除已经完成，不是本报告的缺口。本轮只做审计，没有修改实现、重建编辑器或把任何条目标为已验收。

## 可确认的现有基础

最新源码有真正的 `ReflectionDatabase` / TypeTree / `SerializedObject` / `InspectorView` 属性链路、文档事务撤销、多 System 文档、有序模块栈、版本化脚本/发射器、基础继承、静态开关、曲线数值编辑、CPU/GPU 整系统模拟和离屏预览。固定步、预热、粒子阶段、GPU Simulation Stage、基础 CPU 事件转发、Sprite/Mesh 和基础 Ribbon 条带都有实现。不能把这些全部写成“没有”。

当前登记 **117 个内置模块**。这不是安装版 Niagara 模块的完整语义覆盖率。现有 XML 报告 `build/vfx-inspector-authoring.xml`、`build/vfx-inspector-shared.xml`、`build/vfx-inspector-editor.xml` 本轮重新读取，合计 **148 项、0 失败、0 跳过**；没有重新执行测试。既有 Ribbon 连续条带 DX12/Vulkan 像素测试也有记录，但不代表完整 Ribbon 功能或编辑器 UI 对齐。

## 界面、样式和工作区

| ID | 状态 | 差距 |
| --- | --- | --- |
| UI-01 | 图/部分 | UE 独立资产编辑器的菜单、资产 tab、工具栏与工作区层级未复刻；当前为主编辑器 Particle Editor 内再放 System tab。宿主可以保持 Nullus，但资产工具区结构仍需对齐。 |
| UI-02 | 待验 | 左预览/参数、中 Overview/底部工具、右 Details 的停靠骨架已有；默认选中 tab、分隔条厚度、精确尺寸、面板最小宽度和重置结果未对照。不能仅凭截图比例判定默认 split 错误。 |
| UI-03 | 图/待验 | 字体字形、字号、字重、基线、抗锯齿与中文显示不同；最新 VfxStyle 没有完整 Niagara 字体度量配置。 |
| UI-04 | 缺 | 工具栏、tab、阶段、模块、渲染器、搜索、错误与设置图标体系未齐。当前大量入口使用普通英文文字按钮。 |
| UI-05 | 部分 | 已引入 System/Emitter/Particle 的 UE 阶段色，但完整背景、边框、网格、文本、禁用、悬停、选中、错误状态色与颜色空间关系未对齐。不能称所有阶段色都无来源。 |
| UI-06 | 图/待验 | 工具栏高度、tab 高度、行高、内外边距、缩进、间隔、控件圆角及分隔线与 UE 的密度不同。 |
| UI-07 | 图/待验 | Details 字段及左下 tab 名称截断、输入和标签宽度分配不佳；最新反射版需要重新截图复验。 |
| UI-08 | 缺 | 中文本地化及与 UE 同语言下的命名、提示、菜单文本未建立完整映射；当前主要为硬编码英文。 |
| UI-09 | 部分/待验 | 用户停靠缓存、每文档 dock ID 已有；跨显示器/DPI、浮动、重新打开、全部面板显示/隐藏与 UE 恢复行为尚未验收。 |
| UI-10 | 缺 | Window 菜单目前只提供 Script Editor、Generated Code、Script Stats 等少量开关，缺完整 Niagara 面板入口及模式切换。 |
| UI-11 | 图/待验 | 面板 tab 的图标、关闭按钮、激活线、拖放预览和焦点状态未对齐。 |
| UI-12 | 待验 | 100%/150%/200% 的字号测试已有局部记录；尚无同分辨率、同语言、重置布局后的 UE 截图比对及 ≤1 DIP 逻辑几何证据。 |

证据：[VfxStyle.h](D:/Code/Nullus/Project/Editor/VFX/Editor/VfxStyle.h:13)、[Workspace](D:/Code/Nullus/Project/Editor/Panels/VfxEditor.cpp:331)，对照安装版 `NiagaraSystemToolkitMode_Default.cpp` 的 `Standalone_Niagara_System_Layout_v29`。

## 工具栏、菜单和状态反馈

| ID | 状态 | 差距 |
| --- | --- | --- |
| TB-01 | 部分 | Save/Undo/Redo/Compile 已有，但 UE 图标按钮、分组、分隔、溢出、下拉箭头及禁用样式未齐。 |
| TB-02 | 部分 | 编译状态只是成功/未编译文字或问题数；完整 dirty、编译中、警告、失败、图标、tooltip 与源码定位状态流程未对照。 |
| TB-03 | 部分 | 编译菜单只有 Auto Compile，完整编译选项及按资产类型的可用性未齐。 |
| TB-04 | 缺 | Apply、Apply Scratch、Discard 的资产/暂存脚本专用工作流未按 UE 上下文建立；不能简单把它们当作所有 System 都始终启用的按钮。 |
| TB-05 | 缺 | 从预览生成缩略图、缩略图拍摄/保存流程未接入。 |
| TB-06 | 部分 | Bounds 只有显示开关；System/选中 Emitters 固定范围生成、编辑和对应菜单未齐。 |
| TB-07 | 缺 | Performance、GPU Profiling、编辑模式分析、平均/最大、相对/绝对、Clear Stats 菜单及数据未接入。 |
| TB-08 | 缺 | Debug 菜单的 Attribute Spreadsheet、Debug HUD、FX Outliner 未接入。 |
| TB-09 | 缺 | Baker、Scalability 模式按钮及对应布局/状态未实现。 |
| TB-10 | 部分 | Auto-play、Reset on change、Resimulate when paused 已有；Reset Dependent Systems、锁定源资产变更等命令及联动未齐。 |
| TB-11 | 待验 | 参考菜单存在 `STATS`、`WITH_NIAGARA_DEBUGGER` 和 System/Emitter 上下文条件；这些显示/隐藏/禁用条件尚未完整冻结，不能要求所有配置无条件显示。 |

证据：[Toolbar](D:/Code/Nullus/Project/Editor/Panels/VfxEditor.cpp:237)，安装版 `NiagaraEditorCommands.cpp`、`NiagaraSystemToolkitMode_Default.cpp`。

## 预览视口

| ID | 状态 | 差距 |
| --- | --- | --- |
| VP-01 | 部分 | 已运行整个 System，但视口相机仍为手工正交矩阵；缺 UE 默认透视视口及正交/透视切换。 |
| VP-02 | 缺 | Orbit、自由飞行、相机旋转/移动、视角 FOV、聚焦选中和相机设置未齐；当前交互主要是中键二维平移、滚轮缩放。 |
| VP-03 | 缺 | UE Advanced Preview Scene 的环境、天空/HDR、地面、灯光、阴影与环境旋转未接入。 |
| VP-04 | 部分 | 当前使用独立离屏 RT、SystemRenderProxy 和 ParticleRenderer；尚非完整独立 Scene 世界，无法覆盖环境几何、场景组件、动画和物理预览。 |
| VP-05 | 缺 | 透视菜单、视图模式、显示菜单、背景/网格/原点轴等完整工具条缺失。 |
| VP-06 | 部分 | 只有二维网格和两条轴线；缺 3D 地面网格、屏幕角落方向轴及对应交互/显示配置。 |
| VP-07 | 部分 | 发射器独显已有单个选择路径；多选隔离、锁定与 UE 隔离状态反馈未齐。 |
| VP-08 | 部分 | 包围盒叠加目前只遍历 CPU Emitters，画二维矩形；GPU 范围、3D System/Emitter 固定/动态范围展示未齐。 |
| VP-09 | 缺 | 变换 Gizmo、模块输入位置/方向/范围可视化、预览组件 Motion 等编辑操作未实现。 |
| VP-10 | 部分 | 重置、暂停、步进、Seek、修改后重模拟已有；循环范围、可配置步长/帧率、回放和缓存时间控制未齐。 |
| VP-11 | 缺 | SimCache 录制/载入/播放、帧选择与缓存比较未实现。 |
| VP-12 | 部分 | 预览有状态消息，缺 UE 粒子数、执行顺序、GPU Tick、内存及性能覆盖层。 |
| VP-13 | 部分 | Ribbon 的场景路径调用 `ResolveRibbonDrawSettings`，预览初始化/材质解析仍使用 Sprite settings；预览与场景设置解析不一致，实际图像影响尚未逐项复现。 |
| VP-14 | 待验 | 曝光、色调映射、背景、材质、深度和相机完全一致的结果对照尚未完成；两张不同资产截图不能替代该验收。 |

证据：[Preview](D:/Code/Nullus/Project/Editor/Panels/VfxEditor.cpp:934)、[VfxSystemPreview](D:/Code/Nullus/Project/Editor/VFX/Editor/VfxSystemPreview.cpp:139)。

## System Overview

| ID | 状态 | 差距 |
| --- | --- | --- |
| OV-01 | 部分 | 当前节点是通用节点编辑器的标题和文字清单；缺 Niagara 的彩色标题带、标题图标、紧凑分层行及外框/阴影度量。 |
| OV-02 | 部分 | System 节点仅显示名称、System Spawn/Update 文本；Properties、User Parameters、可操作阶段与系统模块摘要缺失。 |
| OV-03 | 缺 | Emitter 节点头部的启停、折叠、信息、缩略图、属性、目标图标及阶段操作区未齐。 |
| OV-04 | 部分 | 阶段使用彩色文字；缺背景条、阶段图标、折叠箭头、行级添加按钮和类型专属样式。 |
| OV-05 | 部分 | 模块行可选择，缺行尾启用框、告警、继承/版本/覆盖标记和操作图标。 |
| OV-06 | 缺 | 模块输入摘要、枚举/静态开关标签和动态输入小标记未呈现，例如 UE 图里的 Self、Infinite、Sphere、In Cone。 |
| OV-07 | 缺 | 在 Overview 中直接添加、启停、拖排、复制粘贴模块及修复依赖的完整操作未齐；Details 中有部分操作不能代替此视图。 |
| OV-08 | 部分 | 有节点/模块选择联动，但当前模型主要保存一个 emitter/module；多节点/多发射器同步编辑与状态联动未齐。 |
| OV-09 | 缺 | 注释框、分组、框选后的批量命令、跨 System 剪贴板及完整上下文菜单未齐。 |
| OV-10 | 部分 | Renderer 只显示一行文字，非 Sprite 被标为 Mesh；缺真实类型标识、添加/删除/多个 Renderer 及独立选中。 |
| OV-11 | 部分 | 有节点编辑器平移/缩放和 Fit 按钮；Niagara 缩放提示、背景标识、网格、缩放后行细节与焦点行为未对齐。 |
| OV-12 | 部分 | 发射器位置可保存；System 节点位置在恢复时固定为 `(-330,0)`，完整节点布局数据与恢复行为未齐。 |

证据：[Overview](D:/Code/Nullus/Project/Editor/Panels/VfxEditor.cpp:431)。

## Details 和模块栈属性

| ID | 状态 | 差距 |
| --- | --- | --- |
| DT-01 | 部分 | 反射和共享 Inspector 已完成，但 Niagara Stack 的整体结构、专用行布局和模块级定制控件没有因此完成。 |
| DT-02 | 部分 | 当前始终先绘 System timing 和选中 Emitter 属性，再绘全部栈；与 UE 按当前选择组织的标题、计数、栈内容和选择导航不一致。 |
| DT-03 | 部分 | System 编辑视图只暴露固定步与预热等 5 个字段；Effect Type、完整 System 生命周期、范围、依赖、可延展性等属性未齐。 |
| DT-04 | 部分 | Emitter 只暴露名称、Enabled、Target、Capacity、深度排序、视锥剔除；确定性/seed、空间、插值生成、范围、执行/分配和其余属性的编辑入口未齐。 |
| DT-05 | 部分 | 模块 Enabled 使用独立属性行，动作主要在右键菜单；UE 同一标题行上的折叠、启停、版本、重排、删除、重置和状态图标未齐。 |
| DT-06 | 部分 | 搜索只是按模块 label 的区分大小写子串筛选；缺跨输入/类别/子项搜索、高亮、结果导航、过滤器和 Stack 设置。 |
| DT-07 | 缺 | Life Cycle / Scalability / All 一类分段选择、输入摘要/高级/仅修改项显示、完整分类/层级编辑未齐。 |
| DT-08 | 部分 | 全局只读/隐藏/禁用元数据机制已有；模块定义主要只有 name/type/default，尚未导入 UE 每个输入的可见条件、EditCondition、范围、步进、单位和提示等。 |
| DT-09 | 部分 | 有按模块/阶段 Reset to Parent 按钮；缺逐输入默认值重置箭头、差异标记、继承覆盖来源与完整撤销 UX。 |
| DT-10 | 部分 | 输入来源被绘为单独的 Input Source 行；缺 UE 值旁的绑定下拉、嵌套动态输入树、模式图标和紧凑行结构。 |
| DT-11 | 部分 | 参数选择只列同类型 System 参数，缺完整命名空间、上下文可读属性、命名空间修饰及 Parameter Definitions/Collections 绑定。 |
| DT-12 | 部分 | 动态脚本实参在当前 UI/降级路径主要作为常量值传入，嵌套参数/动态输入/曲线组合的完整输入栈未齐。 |
| DT-13 | 部分 | 继承模块输入覆盖当前强制转为 Literal，不能完整保留/编辑 UE 的各种输入来源和嵌套绑定。 |
| DT-14 | 部分 | 简单缺前置模块时有 Fix Issue；多依赖、依赖顺序、跨阶段/跨发射器修复、建议版本和按项诊断体验未齐。 |
| DT-15 | 部分 | Simulation Stage 只提供 Enabled/Name/Iterations 和启用/次数绑定；迭代源、执行条件、线程布局及完整高级属性未齐。 |
| DT-16 | 缺 | Renderer 属性反射视图只有 Color 与 Blend；材质/贴图、资产选择、属性绑定、SubUV、Mesh 槽/LOD、Ribbon 设置等运行时已有数据不能在这里完整创作。 |
| DT-17 | 缺 | 当前缺 Renderer 类型选择、多个 Renderer 的添加/删除与逐 Renderer 属性面板。 |
| DT-18 | 待验 | 自适应标签列、窄面板、向量轴色、颜色控件、数值拖动/输入/菜单、焦点/禁用反馈需按最新共享 Inspector 构建重新对照 UE。 |

证据：[Details](D:/Code/Nullus/Project/Editor/Panels/VfxEditor.cpp:511)、[输入/Renderer 绘制](D:/Code/Nullus/Project/Editor/Panels/VfxEditor.cpp:719)、[反射编辑视图](D:/Code/Nullus/Project/Editor/VFX/Editor/VfxInspector.h:21)。

## Parameters、User Parameters 和 Local Modules

| ID | 状态 | 差距 |
| --- | --- | --- |
| PA-01 | 部分 | Parameters 主要为 System 参数的平面表单；System/Emitter/Particles/Module/Local/Engine 等命名空间树和分类浏览未齐。 |
| PA-02 | 部分 | User Parameters 当前主要是已有参数的临时预览值；UE 用户参数层级、分组、排序、编辑层级和展示元数据未齐。 |
| PA-03 | 缺 | 独立 Parameter Definitions 订阅、同步、解除同步和冲突处理未实现。 |
| PA-04 | 缺 | Parameter Collection 的资产、默认实例、世界实例及运行时传播未实现。 |
| PA-05 | 部分 | 参数增删、重命名、类型、默认值已可编辑；完整引用查找、重命名引用修复、删除影响确认及重定向未齐。 |
| PA-06 | 部分 | 参数拖到图/栈生成读取或绑定、类型/命名空间图标和可用性提示未齐。 |
| PA-07 | 部分 | Local Modules 仅枚举模块中的内联 DynamicInput；不是完整 Scratch Pad 模块库、层级编辑器和多脚本文档。 |
| PA-08 | 缺 | Scratch Pad 的创建/复制、Apply/Discard、导出资产、从资产导入和受影响实例更新等全流程未齐。 |

证据：[Parameters](D:/Code/Nullus/Project/Editor/Panels/VfxEditor.cpp:877)、[Local Modules](D:/Code/Nullus/Project/Editor/Panels/VfxEditor.cpp:385)。

## 脚本图、函数和动态输入

| ID | 状态 | 差距 |
| --- | --- | --- |
| GR-01 | 部分 | 已有运算节点、连接和独立库图；缺完整 Niagara System/Emitter/Module Script 文档与图类型切换。 |
| GR-02 | 缺 | Parameter Map 不是图中的一等类型；Map Input/Output、Get/Set、命名空间和历史追踪等完整节点语义未实现。 |
| GR-03 | 缺 | Custom HLSL 节点、输入/输出声明、include、数据接口函数调用和相应诊断未接入。自定义片元材质 HLSL 不等于 Niagara 脚本 Custom HLSL。 |
| GR-04 | 部分 | 存在版本化函数调用展开与静态开关；完整 Niagara 多输出/类型推导、重载、数值泛型和上下文规则未齐。 |
| GR-05 | 部分 | 节点目前使用通用名称和 A/B 等引脚；UE 节点标题/颜色、具名类型引脚、默认值、类型菜单和 pin 操作未齐。 |
| GR-06 | 部分 | 只有基础添加菜单；上下文动作搜索、按类型过滤、从 pin 拖线创建节点、收藏/建议/库来源展示未齐。 |
| GR-07 | 缺 | 注释、Reroute、分组、多节点剪贴板/粘贴、节点刷新和完整图整理工作流未齐。 |
| GR-08 | 部分 | 诊断可定位模块，缺精确节点/引脚/输入高亮、调用栈、外部脚本文档定位和完整跨视图导航。 |
| GR-09 | 部分 | Generated Code 目前为简单 HLSL 文本，Stats 主要为指令数；缺 UE 平台/脚本/阶段选择、搜索/复制、源码关联和完整编译统计。 |
| GR-10 | 部分 | 库资产可新增/暴露版本；脚本编辑器自身缺与 System 预览相同的完整工作区和多资产文档体验。 |

证据：[Script](D:/Code/Nullus/Project/Editor/Panels/VfxEditor.cpp:1103)、[库编辑器](D:/Code/Nullus/Project/Editor/Panels/VfxLibraryEditor.cpp:108)、[VfxLibrary.h](D:/Code/Nullus/Runtime/VFX/Authoring/VfxLibrary.h:8)。

## Timeline、Curves 和渐变

| ID | 状态 | 差距 |
| --- | --- | --- |
| TL-01 | 缺 | Timeline 没有真正的时间标尺、刻度、帧/秒模式和可缩放时间画布。 |
| TL-02 | 缺 | 无发射器轨道、Section、生成区间、循环/生命周期可视化与轨道图标。 |
| TL-03 | 缺 | 无 Section 开始/结束/时长拖动、裁剪、复制、关键帧和属性轨道编辑。 |
| TL-04 | 部分 | Seek 使用单个 Slider，缺画布游标、连续拖动反馈、标尺命中、时间范围平移和滚动条联动。 |
| TL-05 | 缺 | 无捕捉、帧率、时间显示格式、播放/工作范围边界及循环范围编辑。 |
| TL-06 | 部分 | Range 目前只限制时间滑块，未驱动对应播放区间的循环/停止逻辑；界面参数不能视为已有完整播放范围。 |
| TL-07 | 部分 | Step 直接走 `Tick(1/60)`，还受 playbackSpeed 影响；缺独立帧率/单帧步进语义与倒放控制。 |
| TL-08 | 缺 | UE 时间轴 Add/轨道菜单、mute/solo/lock、自动关键帧与选择工具未齐。 |
| TL-09 | 缺 | Timeline 与 Sequencer/Sequence 资产、场景时间和 SimCache 轨道集成未实现。 |
| CV-01 | 部分 | 已有折线/三次曲线绘制与 key 数值表，但曲线画布本身不支持拖拽 key 或切线。 |
| CV-02 | 缺 | 缺时间/值坐标轴、网格、框选、平移/缩放、Fit、通道显隐与同步游标。 |
| CV-03 | 部分 | Linear/Cubic 和切线数值存在；完整 Constant/自动/用户/断开切线、权重和边界外推选项未齐。 |
| CV-04 | 缺 | 多 key 变换、复制粘贴、对齐、捕捉和曲线资产工作流未齐。 |
| CV-05 | 部分 | 四分量输入可逐 key ColorEdit，但不是 UE 渐变条、颜色/透明度色标、拖排、颜色空间及渐变预设交互。 |
| CV-06 | 待验 | 曲线插值、切线单位、归一化年龄映射、重采样/精度与 CPU/GPU 结果尚未逐项对照安装版 UE。 |

证据：[Timeline](D:/Code/Nullus/Project/Editor/Panels/VfxEditor.cpp:1015)、[Curves](D:/Code/Nullus/Project/Editor/Panels/VfxEditor.cpp:1052)、[VfxSession](D:/Code/Nullus/Project/Editor/VFX/Editor/VfxSession.cpp:57)。

## 快捷键、选择、撤销和多文档

| ID | 状态 | 差距 |
| --- | --- | --- |
| IX-01 | 部分 | 已支持 Ctrl+S/Z/Y、Space、S、D/Shift+D、F；E 添加发射器、Ctrl+F 查找、A 全部适配、Ctrl+O 栈折叠、Ctrl+M 禁用模块折叠等参考命令未齐。 |
| IX-02 | 部分 | 文本编辑/弹窗优先级已有保护；工作区内仍主要用同一焦点标记路由，缺 Preview/Overview/Graph/Timeline/Details 各自完整的命令上下文。 |
| IX-03 | 部分 | 单文档共享选择已有；System、Renderer、输入、key、多个模块/发射器的统一选择模型未齐。 |
| IX-04 | 待验 | 独立撤销、连续拖动一次撤销和取消已有测试；跨图/曲线/栈/继承升级、弹窗与关闭时的全部事务范围仍未人工验收。 |
| IX-05 | 部分 | System 支持多文档与保存关闭提示；独立库编辑器主要只有一个活动文档，完整跨资产多文档和布局恢复未齐。 |
| IX-06 | 待验 | 拖放跨视图/跨文档、关闭编译中资产、热重载、重新打开和源依赖变更的完整交互矩阵未验收。 |

证据：[Shortcuts](D:/Code/Nullus/Project/Editor/Panels/VfxEditor.cpp:273)、[文档 tab](D:/Code/Nullus/Project/Editor/Panels/VfxEditor.cpp:311)。

## 资产家族、模板、继承与版本

| ID | 状态 | 差距 |
| --- | --- | --- |
| AS-01 | 部分 | System、Emitter、Module、Function、Dynamic Input 已有实际模型和编辑；AssetKind 中出现其它名称不代表其它资产已经实现。 |
| AS-02 | 缺 | Parameter Collection、Parameter Definitions、Effect Type、Data Channel、SimCache 缺完整独立模型、导入、编辑和运行闭环。 |
| AS-03 | 部分 | 当前 New 基本直接创建默认 System；缺 UE 创建向导、模板/行为示例/空系统、从已有发射器创建、搜索过滤和缩略图浏览。 |
| AS-04 | 部分 | 仓库已有 Starter/GPU/阶段/时间等例子，缺与全部参考模板一一对应的可运行资产及默认输入/版本数据。 |
| AS-05 | 部分 | 已有父发射器、指定版本和输入/阶段覆盖；完整继承差异树、冲突合并、重设父级、解除继承和来源导航未齐。 |
| AS-06 | 部分 | 已有新增版本、选择和暴露版本；完整升级脚本、破坏性变更迁移、版本说明/弃用、锁定变更和引用者批量升级未齐。 |
| AS-07 | 部分 | `.vfx` 和工件已有版本化，编译失败保留上一成功程序；所有新资产家族的依赖、热重载、cook 与源文件剥离闭环未齐。 |
| AS-08 | 待验 | 资产创建、编辑、保存重开、移位/重命名、组件引用到独立包全流程尚未按每类资产逐项验收。 |

证据：[AssetKind/System](D:/Code/Nullus/Runtime/VFX/Authoring/VfxAuthoring.h:11)、[库资产模型](D:/Code/Nullus/Runtime/VFX/Authoring/VfxLibrary.h:29)、[库编辑器创建菜单](D:/Code/Nullus/Project/Editor/Panels/VfxLibraryEditor.cpp:140)。

## 类型、编译语义与执行阶段

| ID | 状态 | 差距 |
| --- | --- | --- |
| IR-01 | 部分 | 类型只有 Float/Float2/Float3/Float4/Int32/Bool；缺独立语义的 Position、Quaternion、Matrix、NiagaraID、枚举、结构、数组、对象/接口等类型体系。Float4 能存四元数不等于完整类型支持。 |
| IR-02 | 部分 | 有顺序模块栈 SSA 和 CPU/GPU 共用编译链，缺完整 Parameter Map、作用域、命名空间权限、默认来源、历史与别名语义。 |
| IR-03 | 部分 | Particle Spawn/Update 与粒子 Simulation Stage 已可执行；System Spawn/Update、通用 Emitter Spawn/Update 图尚未实现。几个 SystemState/Emitter 名称模块写粒子列不能代替对应执行域。 |
| IR-04 | 部分 | Emitter Update 主要将特定模块降成 duration/rate/burst 等调度设置，输入要求 Literal；不能执行任意模块图和动态参数调度。 |
| IR-05 | 部分 | GPU Simulation Stage 只迭代存活粒子，缺 DI/Grid 迭代、状态筛选、执行行为、完整线程/分发布局与依赖绑定。 |
| IR-06 | 部分 | 独立函数/静态开关已实现部分展开；泛型、完整调用签名、脚本使用限制、类型推导和全部编译选项未齐。 |
| IR-07 | 缺 | 通用数据接口绑定、读写签名、资源依赖、CPU/GPU 提供者和 Custom HLSL 编译路径未齐。 |
| IR-08 | 待验 | 随机流/seed、循环/预热/固定步、阶段顺序、精度与浮点/整数边界尚无完整安装版 UE 行为对照。 |
| IR-09 | 待验 | CPU/GPU/设备限制的合法条件、警告/错误级别及实现中的容量/迭代/时间限制，尚未与参考冻结清单逐项核对。 |

证据：[VfxGraph.h](D:/Code/Nullus/Runtime/VFX/VfxGraph.h:21)、[编译执行域](D:/Code/Nullus/Runtime/VFXCompiler/VfxStackCompiler.cpp:1669)、[Emitter/Event 降级](D:/Code/Nullus/Runtime/VFXCompiler/VfxStackCompiler.cpp:1781)。

## 模块、函数和动态输入库

| ID | 状态 | 差距 |
| --- | --- | --- |
| LB-01 | 部分 | 117 个内置模块只是子集，并含别名/组合/本地扩展；冻结的 276 个 Module 资产尚未建立完整的版本、输入、输出和公式等价映射。 |
| LB-02 | 部分 | 名称相似模块仍有语义差距，例如 Shape Location 的模式/分布、Emitter State 的 Self/System/Inactive/Loop 条件和 Initialize Particle 的完整属性/条件。不能按名称直接判完成。 |
| LB-03 | 部分 | 当前 Add Module 是按阶段过滤的平面菜单；分类、搜索、关键词、建议、弃用、库可见性、来源/版本和预览说明未齐。 |
| LB-04 | 部分 | 独立函数/动态脚本可创作，但 250 个 Functions、297 个 DynamicInputs 的安装版库并未逐项移植/对照。算术 op 的存在不能替代具名函数资产的全部行为。 |
| LB-05 | 部分 | 带 Curve/Gradient/参数的基础输入可用；模块特有自定义编辑器、DI 资源选取、数组分区、音频、骨骼、地形/屏幕采样等输入流程缺失。 |
| LB-06 | 待验 | 缺所有模块的输入默认、静态开关、显示条件、依赖、随机、阶段/CPU-GPU 合法性和参考运行样例。 |

本轮机械名称匹配：158 个 Library 可见、非废弃 Module 资产中，97 个找到去空格/标点后的同名 ID 或显示名候选，61 个未找到。**这不是 61 个全部没有等价实现，也不是 97 个语义完成，更不是完成率。** 例如已有独立 Sphere/Box 模块仍不等于完整 ShapeLocation 的模式菜单。详细 158 行见 `NiagaraGapAudit-2026-10-19-module-candidates.csv`；61 个未找到同名候选的名字在文末完整列出。

证据：[ModuleLibrary](D:/Code/Nullus/Runtime/VFX/Authoring/VfxAuthoring.cpp:36)、[NiagaraParity.json](D:/Code/Nullus/Docs/Plans/NiagaraParity.json)。

## 数据接口与真实数据提供者

以下是当前粒子创作链路尚未接入的完整提供者族；引擎存在 Mesh/Texture/Physics 类型，不代表 VFX 已能作为 Niagara DI 采样和写入。

| ID | 状态 | 差距 |
| --- | --- | --- |
| DI-01 | 缺 | 通用 DI 实例、函数注册/签名、资源属性、每实例数据、绑定与生命周期模型。 |
| DI-02 | 缺 | Bool/Int/Float/Vector/Color/Quaternion/Position/NiagaraID/Mesh 等数组 DI、分布及可写存储/计数。 |
| DI-03 | 部分 | 盒/球/胶囊物理查询已有；完整 CollisionQuery、异步 GPU Trace、深度/GDF 碰撞、刚体/PhysicsAsset/骨骼碰撞、Cooking/网格/高度场提供者未齐。 |
| DI-04 | 缺 | SkeletalMesh、StaticMesh、DynamicMesh、SocketReader、表面/骨骼/UV/连通性/动画采样。 |
| DI-05 | 缺 | Spline、VectorField、CurlNoise 等完整资源型采样接口；简单解析形状/随机噪声模块不能替代。 |
| DI-06 | 缺 | 2D/Array/Cube/Volume Texture、虚拟纹理、Landscape、GBuffer、SceneCapture、Occlusion 的读取及绑定。 |
| DI-07 | 缺 | RenderTarget2D/Array/Cube/Volume、Grid2D/3D、RasterizationGrid、NeighborGrid 的读写、迭代与跨阶段同步。 |
| DI-08 | 缺 | Particle Reader、其它发射器粒子采样与跨系统/跨 GPU 数据依赖。基础事件转发不能替代。 |
| DI-09 | 缺 | Audio、Oscilloscope、Spectrum、AudioPlayer 和持续音频实例/参数绑定。 |
| DI-10 | 缺 | DataChannel Read/Write、世界发布、访问上下文与分区处理。 |
| DI-11 | 缺 | Export、DataTable、Actor/Component/UObject 属性读取、ConsoleVariable、平台集合与材质参数集合等接口。 |
| DI-12 | 缺 | SimCacheReader、VolumeCache、SparseVolumeTexture 和相关缓存数据提供者。 |
| DI-13 | 缺 | DebugDraw、RendererInfo、EmitterProperties 等查询/调试 DI 的完整调用路径。 |
| DI-14 | 待验 | 每个接口的 CPU/GPU 支持、函数签名、默认绑定、资源失效/热重载和设备条件均未完成参考实测。 |

矩阵 `data-interface` 的 110 行混合了 DI、DataChannel、SimulationStage 的原生声明及辅助/基类，**不是 110 个独立可用 DI 的精确计数**；具名条目全部保留在 1193 行附件中。

## 渲染器和材质

| ID | 状态 | 差距 |
| --- | --- | --- |
| RD-01 | 部分 | Sprite 已有贴图、SubUV、动态材质参数、混合、深度淡出和部分朝向；完整 Facing/Alignment 选项、属性绑定、可见性/排序条件及 Renderer 编辑 UI 未齐。 |
| RD-02 | 部分 | Mesh 已有最多八槽、每槽材质与每视图 LOD；逐粒子 LOD、分段材质、完整朝向/pivot、跨槽透明顺序及完整属性入口未齐。 |
| RD-03 | 部分 | Ribbon 已有相邻粒子相机朝向条带和连续像素用例；Ribbon ID/LinkOrder 的稳定拓扑、分组/排序、断带/死亡重连、细分/曲率、形状、切线/扭转、多 UV/距离平铺等完整能力未齐。 |
| RD-04 | 部分 | RibbonSettings 声明了 UV 平铺参数，但当前着色器主要用每段 0–1 UV；声明、执行和 UI 不能视为均已完成。 |
| RD-05 | 缺 | Light Renderer 的真实灯光生成、属性绑定、预算和场景灯光接入。 |
| RD-06 | 缺 | Decal Renderer 的材质、变换、朝向、排序和场景贴花接入。 |
| RD-07 | 缺 | Component Renderer 的类选择、实例池、属性绑定、更新与销毁。 |
| RD-08 | 缺 | Volume Renderer 的体积材质、范围、密度/阴影与场景体积接入。 |
| RD-09 | 部分 | 每个 Emitter 当前只有一个 renderer enum；UE 多 Renderer、独立启停、可见性/排序绑定和多个输出不能完整表达。 |
| RD-10 | 部分 | 自定义片元材质和动态参数已有；完整表面材质、顶点变形/WPO、光照/法线、阴影与运动矢量链路未齐。 |
| RD-11 | 部分 | 深度排序/剔除已有；自定义排序键、所有 Renderer 的范围/遮挡/LOD、跨 Renderer 的正确性与材质资源池未齐。 |
| RD-12 | 待验 | 所有渲染器/混合/材质/多视图/平台组合与 UE 同场景画面对照、MSAA 深度解析和设备限制尚未完成。 |

证据：[Renderer/Settings](D:/Code/Nullus/Runtime/VFX/VfxGraph.h:40)、[Ribbon shader](D:/Code/Nullus/Runtime/VFXCompiler/VfxCompiler.cpp:826)、[Renderer 属性](D:/Code/Nullus/Project/Editor/Panels/VfxEditor.cpp:865)。矩阵 8 行 Renderer 原生声明包含基类，实际目标是 7 种 Renderer，不能说有 8 种独立 Renderer。

## 世界运行、事件和 GPU 生产能力

| ID | 状态 | 差距 |
| --- | --- | --- |
| RT-01 | 部分 | CPU 组件更新/GPU 渲染发布已有；完整世界批处理、Worker 调度、共享 Simulation 相位和系统依赖图未齐。 |
| RT-02 | 部分 | 基础 CPU Spawn/Death/Collision 生成策略及跨发射器接收已有；没有完整 Event Handler 执行图、payload schema、任意事件属性与处理模式。 |
| RT-03 | 部分 | 当前每 emitter 接收单一事件类型，按 emitter 顺序单次分发；完整源选择/依赖排序、多 handler 和 UE 执行语义未齐。 |
| RT-04 | 缺 | GPU 事件生成/回读/消费及其异步生命周期未形成完整链路。 |
| RT-05 | 部分 | 单 System 的固定步/预热已有；完整世界调度、组件覆盖、依赖时钟、每种生命周期/循环状态语义仍需补齐和参考对照。 |
| RT-06 | 部分 | 不可变快照、扩容和延迟释放已有；帧资源/缓冲/PSO/常量池、精确计数驱动分配、缩容和大量实例内存策略未齐。 |
| RT-07 | 部分 | 间接绘制和部分 GPU 调度已有；更多存活数驱动的间接准备/分发、异步队列、多发射器资源依赖未齐。 |
| RT-08 | 部分 | GPU 范围异步读回已用于透明顺序；首帧回退及滞后范围不是完全实时排序，完整合并读回/自定义排序/性能仍需完善。 |
| RT-09 | 部分 | C#/Lua 已有基础激活/暂停/参数等 API；完整 DI/Channel/Cache、生命周期/池化、事件/委托和实际脚本场景未齐。 |
| RT-10 | 待验 | 错误编译、取消、热重载、关闭文档、设备失效、多视图与 GPU 延迟释放的完整组合/压力验收未完成。 |

证据：[CPU 事件分发](D:/Code/Nullus/Runtime/VFX/SystemInstance.cpp:154)、[编译阶段限制](D:/Code/Nullus/Runtime/VFXCompiler/VfxStackCompiler.cpp:1830)、[实施记录剩余项](D:/Code/Nullus/Docs/Plans/VfxImplementationStatus.md)。

## 生产工具

| ID | 状态 | 差距 |
| --- | --- | --- |
| PT-01 | 缺 | Stateless/Lightweight 的资产、模块配置、运行时和编辑预览闭环；矩阵虽列出 26 个原生模块，不能当作已实现。 |
| PT-02 | 缺 | Effect Type、预算、重要性处理、实例上限、距离/可见性裁剪及 Scalability 专用工作区。普通视锥剔除不能替代。 |
| PT-03 | 缺 | System/Component 池化、预生成/回收、按预算暂停/恢复和其状态诊断。 |
| PT-04 | 缺 | SimCache 创建、捕获属性/DI、存储/压缩、回放、重定位与检查器。 |
| PT-05 | 缺 | Baker 的相机、帧范围、图集/纹理/体积输出、预览和保存流程。 |
| PT-06 | 缺 | Sequencer 生命周期/参数/缓存轨道和场景时间集成。 |
| PT-07 | 缺 | Attribute Spreadsheet、Debug HUD、FX Outliner 的运行实例、属性、过滤、捕获和定位流程。 |
| PT-08 | 部分 | 仅有基本指令统计/日志；完整 CPU/GPU 时间、模块热度、内存、实例数、趋势/基线与分析 UI 未齐。 |

## 验收与基准本身的缺口

| ID | 状态 | 差距 |
| --- | --- | --- |
| QA-01 | 缺 | 参考矩阵主要来自资产/源码清单；尚未细化全部属性默认值、函数签名、显示/隐藏/禁用条件和运行实例。 |
| QA-02 | 待验 | 917 个安装版资产只采集 registry metadata，均未采集完整行为；不能以资产数量证明功能覆盖。 |
| QA-03 | 待验 | 1193 条原矩阵：1188 pending、5 in-progress、0 verified，缺逐条 UE referenceCase + 实现 + 示例 + 通过证据。 |
| QA-04 | 待验 | 截图尚无构建哈希、同语言/尺寸/DPI/重置布局配套数据；本轮不能给出像素差百分比或 1 DIP 合格判定。 |
| QA-05 | 待验 | 菜单状态、快捷键、拖放、多文档、全部模块与资产家族的桌面人工/自动交互验收未完成。 |
| QA-06 | 待验 | 创建→模块/图/曲线编辑→保存重开→场景播放→独立运行包，全类型创作流程缺完整证据。 |
| QA-07 | 待验 | Windows/DX12 有局部设备结果，但整个核心矩阵未完成；Windows Vulkan 不能替代 Linux/Vulkan 真机验收。 |
| QA-08 | 待验 | Linux/Vulkan、macOS/Metal 的构建与真实设备验收未分别完成；生成平台包/SPIR-V 不是目标设备运行。 |
| QA-09 | 待验 | 至少 30 分钟混合压力、长时间编辑/编译/关闭/播放及大量实例稳定性未验收。 |
| QA-10 | 待验 | CPU 1千/1万/10万，GPU 1万/10万/100万及大量实例，与 UE 匹配负载的 CPU/GPU/内存数据不完整；后续 10% 回归阈值缺稳定基线。 |
| QA-11 | 部分 | 一些文档仍写“12 个模块”“事件/Ribbon 未实现”等旧状态，历史覆盖率也存在不同统计口径；需要按当前源码/证据统一状态，不能沿用旧百分比。 |

本轮运行 `Tools/VFX/Test-NiagaraParity.ps1`，输出 `1193 of 1193 Niagara requirements remain unverified.`，退出码 **1**，符合当前未验收状态。默认入口已按用户要求切换并不改变该结果。

## 具名参考清单附件

完整附件 `NiagaraGapAudit-2026-10-19-requirements.csv` 保留原矩阵的 **1193 行**，包括 ID、类别、名称、参考文件/哈希、原状态、实现/示例/UE 证据、本地验证、可见/弃用元数据及名称候选。它是待对照全集，**不是“1193 个确认缺失功能”清单**。

| 原矩阵类别 | 条目数 | 尚缺的判定 |
| --- | ---: | --- |
| Modules | 276 | 按版本与输入/公式/条件/阶段建立对应并验证；含隐藏、废弃等不同条件。 |
| DynamicInputs | 297 | 逐资产的签名、类型、参数与实际结果。 |
| Functions | 250 | 逐资产的调用、类型/重载与实际结果。 |
| DefaultAssets | 87 | 默认资产/模板的角色、版本、创建流程与可运行示例。 |
| ParameterDefinitions | 7 | 定义资产、订阅同步、覆盖与运行传播。 |
| data-interface | 110 | 原生声明细分、接口/辅助类识别、签名、提供者与设备条件。 |
| renderer | 8 | 含 1 个基类；7 种实际 Renderer 的属性/绑定/输出。 |
| stateless | 26 | Stateless 原生模块及完整创作运行工作流。 |
| command | 50 | 命令作用、快捷键、可见/可用条件和状态反馈。 |
| panel | 21 | 面板内容、显示条件、联动与停靠。 |
| layout | 2 | Default / Scalability 布局及恢复。 |
| style | 37 | 已提取的颜色 token；尚未覆盖字体、所有 brush、控件状态和度量。 |
| acceptance | 22 | 各领域行为/平台/稳定性/性能验收。 |

## 尚未找到同名内置候选的 61 个库可见非废弃模块

下面仅是可核对的检索结果，**没有同名候选不等于不存在组合实现**。完整输入/输出/公式/版本对照仍然必须做。
| 模块 | 参考资产 |
| --- | --- |
| FillBoolArray | `Content/Modules/Arrays/FillBoolArray.uasset` |
| FillColorArray | `Content/Modules/Arrays/FillColorArray.uasset` |
| FillFloatArray | `Content/Modules/Arrays/FillFloatArray.uasset` |
| FillIntArray | `Content/Modules/Arrays/FillIntArray.uasset` |
| FillPositionArray | `Content/Modules/Arrays/FillPositionArray.uasset` |
| FillQuaternionArray | `Content/Modules/Arrays/FillQuaternionArray.uasset` |
| FillVector2DArray | `Content/Modules/Arrays/FillVector2DArray.uasset` |
| FillVector4Array | `Content/Modules/Arrays/FillVector4Array.uasset` |
| FillVectorArray | `Content/Modules/Arrays/FillVectorArray.uasset` |
| SampleParticlesFromOtherEmitter | `Content/Modules/AttributeReader/SampleParticlesFromOtherEmitter.uasset` |
| SpawnParticlesFromOtherEmitter | `Content/Modules/AttributeReader/SpawnParticlesFromOtherEmitter.uasset` |
| UpdateParticlesFromOtherEmitter | `Content/Modules/AttributeReader/UpdateParticlesFromOtherEmitter.uasset` |
| InitializeAudioPlayer | `Content/Modules/Audio/InitializeAudioPlayer.uasset` |
| PlayAudio | `Content/Modules/Audio/PlayAudio.uasset` |
| PlayLoopingAudio | `Content/Modules/Audio/PlayLoopingAudio.uasset` |
| PlayPersistentAudio | `Content/Modules/Audio/PlayPersistentAudio.uasset` |
| SetInitialAudioParameter | `Content/Modules/Audio/SetInitialAudioParameter.uasset` |
| UpdatePersistentAudio | `Content/Modules/Audio/UpdatePersistentAudio.uasset` |
| AlignParticlesWithCollisionPlane | `Content/Modules/Collision/AlignParticlesWithCollisionPlane.uasset` |
| NiagaraDistanceFieldCollisions | `Content/Modules/Collision/NiagaraDistanceFieldCollisions.uasset` |
| PopulateNeighborGrid | `Content/Modules/Collision/PopulateNeighborGrid.uasset` |
| SceneDepthTest | `Content/Modules/Collision/SceneDepthTest.uasset` |
| SetupRigidBodyDI | `Content/Modules/Collision/SetupRigidBodyDI.uasset` |
| CalculateLinkConstraint | `Content/Modules/Constraints/CalculateLinkConstraint.uasset` |
| Decal_Attributes | `Content/Modules/Decal/Decal_Attributes.uasset` |
| EmitterBounds | `Content/Modules/Emitter/EmitterBounds.uasset` |
| SpawnParticlesFromSimCache | `Content/Modules/Emitter/SpawnParticlesFromSimCache.uasset` |
| ExportParticleDataToBlueprint | `Content/Modules/ExportParticleData/ExportParticleDataToBlueprint.uasset` |
| SampleLandscape | `Content/Modules/Landscape/SampleLandscape.uasset` |
| Light_Attributes | `Content/Modules/Light/Light_Attributes.uasset` |
| AvoidDistanceFieldSurfaces_GPU | `Content/Modules/Math/AvoidDistanceFieldSurfaces_GPU.uasset` |
| FindScreenSpaceBoundingBox | `Content/Modules/Math/FindScreenSpaceBoundingBox.uasset` |
| MoveToNearestDistanceFieldSurface_GPU | `Content/Modules/Math/MoveToNearestDistanceFieldSurface_GPU.uasset` |
| PlaceParticlesOnDepthBuffer_GPU | `Content/Modules/Math/PlaceParticlesOnDepthBuffer_GPU.uasset` |
| ApplyTraverseSkeletalMeshUpdate | `Content/Modules/Mesh/ApplyTraverseSkeletalMeshUpdate.uasset` |
| InitializeTraverseSkeletalMesh | `Content/Modules/Mesh/InitializeTraverseSkeletalMesh.uasset` |
| PrepareTraverseSkeletalMesh | `Content/Modules/Mesh/PrepareTraverseSkeletalMesh.uasset` |
| TraverseSkeletalMesh | `Content/Modules/Mesh/TraverseSkeletalMesh.uasset` |
| SampleRuntimeVirtualTexture | `Content/Modules/RVT/SampleRuntimeVirtualTexture.uasset` |
| InitialDecalOrientation | `Content/Modules/Spawn/Decal/InitialDecalOrientation.uasset` |
| SampleParticlesFromSimCache | `Content/Modules/Spawn/Location/SampleParticlesFromSimCache.uasset` |
| SocketLocation | `Content/Modules/Spawn/Location/SocketLocation.uasset` |
| Spawn_MS_VertexAnimationTools_MorphTarget_Particles | `Content/Modules/Spawn/Location/Spawn_MS_VertexAnimationTools_MorphTarget_Particles.uasset` |
| StaticMeshLocation | `Content/Modules/Spawn/Location/StaticMeshLocation.uasset` |
| ShapeLocation | `Content/Modules/Spawn/Location/V2/ShapeLocation.uasset` |
| SkeletalMeshLocation | `Content/Modules/Spawn/Location/V2/SkeletalMeshLocation.uasset` |
| Initialize_MeshReproductionSprite | `Content/Modules/Spawn/MeshInterface/Initialize_MeshReproductionSprite.uasset` |
| UpdateDecalOrientation | `Content/Modules/Update/Decal/UpdateDecalOrientation.uasset` |
| AerodynamicDrag | `Content/Modules/Update/Forces/AerodynamicDrag.uasset` |
| CurlNoiseForce | `Content/Modules/Update/Forces/V2/CurlNoiseForce.uasset` |
| SampleSkeletalMesh | `Content/Modules/Update/MeshInterface/SampleSkeletalMesh.uasset` |
| Update_MeshReproductionSprite | `Content/Modules/Update/MeshInterface/Update_MeshReproductionSprite.uasset` |
| RecreateCameraProjection | `Content/Modules/Update/Position/RecreateCameraProjection.uasset` |
| Update_MS_VertexAnimationTools_MorphTargets | `Content/Modules/Update/Position/Update_MS_VertexAnimationTools_MorphTargets.uasset` |
| SamplePseudoVolumeTexture | `Content/Modules/Update/Textures/SamplePseudoVolumeTexture.uasset` |
| SampleTexture | `Content/Modules/Update/Textures/SampleTexture.uasset` |
| SubUV_TextureSample | `Content/Modules/Update/Textures/SubUV_TextureSample.uasset` |
| WorldAlignedTextureSample | `Content/Modules/Update/Textures/WorldAlignedTextureSample.uasset` |
| AsyncGPUTrace | `Content/Modules/Update/Utility/AsyncGPUTrace.uasset` |
| DebugDraw | `Content/Modules/Update/Utility/DebugDraw.uasset` |
| SampleGBuffer | `Content/Modules/Update/Utility/SampleGBuffer.uasset` |

## 审计文件与快照

- [逐项差距 CSV](D:/Code/Nullus/Docs/Plans/NiagaraGapAudit-2026-10-19-gaps.csv)
- [1193 条完整待对照记录](D:/Code/Nullus/Docs/Plans/NiagaraGapAudit-2026-10-19-requirements.csv)
- [158 个库可见非废弃模块的名称候选](D:/Code/Nullus/Docs/Plans/NiagaraGapAudit-2026-10-19-module-candidates.csv)

下列哈希用于标识审计源文件快照，不代替构建或运行证据。

| 文件 | SHA256 |
| --- | --- |
| Docs/Plans/NiagaraParity.json | 5f1f8ea863401acab6f2e7c13282c7c058a163e14ebbc31740de48713f030372 |
| Project/Editor/Panels/VfxEditor.cpp | 8091e4b02c8de8695cc7879a8da233b6492b951648e9eda43afd82ddd4ad534e |
| Project/Editor/Panels/VfxLibraryEditor.cpp | e66198ed6d2c0fc2bdef66cf3faa12634aca0f697a532ccb68a7894cd6733c10 |
| Project/Editor/VFX/Editor/VfxInspector.h | 598e1327c61f7827212c662ad7e2cd9c67cb90b3c4b902d2f768dd7c8302bd36 |
| Project/Editor/VFX/Editor/VfxStyle.h | 0db862a5f3d3013284a973b0f2596461d717cd61800b94ea5b091afd14def156 |
| Runtime/VFX/Authoring/VfxAuthoring.cpp | eb06356dbe5c8af7ec2088f0bb38c0aded5d2d44d59b2fd52883e7d794ff1e9b |
| Runtime/VFX/VfxGraph.h | 198f61387e3594c995f8b595c30a3fa865f823e813d6c372643c69201360291e |
| Runtime/VFXCompiler/VfxStackCompiler.cpp | 9284c7690a2247f1a9ea0286d7ce35dd95f6a13319cb702dd92ee012a673cc31 |
| Runtime/VFXCompiler/VfxCompiler.cpp | 669b370ae5bf06198dc093ec0fb945802c964380d0ca93377a23352b3d65a8df |
| Runtime/VFX/SystemInstance.cpp | a830bff320aba4c726d189c793f83975f3953b7472bfe350dbb509c2039be05f |

本报告共 184 条差距/待验收发现，跨 18 个分组。各条不是等权功能点，也不能换算为完成百分比。
