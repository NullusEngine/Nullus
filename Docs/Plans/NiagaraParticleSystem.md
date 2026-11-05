# UE5.7.4 Niagara 粒子系统实施方案

状态：设计与实施规划，本文不表示相关功能已经实现。

当前实施与验证记录见 [VfxImplementationStatus.md](VfxImplementationStatus.md)。

2026-10-17 编辑器重建采用已安装 UE 5.7.4 CL 51494982 为行为基线；完整决定与候选版边界见
[NiagaraEditorRebuild.md](NiagaraEditorRebuild.md)。新旧入口并存，使用 Nullus 原生坐标/单位，允许重建
源资产格式；全部功能、交互、画面和三平台设备验收通过后一次替换默认入口。旧 `.vfx` 不自动迁移或改写。

## 目标与确定的约束

- 以本地 UE5.7.4 Niagara 源码、内置模块及实际执行行为为基线，建设完整核心粒子系统与编辑器。
- 最终验收覆盖 Windows/DX12、Linux/Vulkan、macOS/Metal；各平台分别记录构建、运行及设备验证结果。
- 使用 Nullus 原生资产格式、ImGui 编辑器及 C#/Lua API，对齐功能、语义和创作流程；UE 资产迁移不在本轮范围内。
- 包含 CPU/GPU 模拟、节点脚本、模块栈、数据接口、Simulation Stage、Data Channel、Stateless 发射器、SimCache、核心渲染器及生产工具。
- Niagara Fluids 作为后续扩展；核心网格数据接口、模拟阶段和 Volume Renderer 仍属于本轮。
- 缺失的物理、动画、音频、地形等子系统建设到足以支持上述粒子能力的深度。
- ImGuiNodeEditor 源码直接放入引擎、由现有 UI 模块编译和维护，允许本地定制与扩展。
- 物理射线及场景查询采用 PhysX，作为共享引擎能力供粒子系统消费。

参考源码根目录：`D:\Code\UE\UnrealEngine\Engine\Plugins\FX\Niagara`。

## ImGuiNodeEditor 源码接入

### 编译与维护

沿用现有 `Runtime/UI/ImGuiExtensions` 源码内置约定：

1. 将核心源码、所需内部头文件及许可证放入 `Runtime/UI/ImGuiExtensions/ImGuiNodeEditor/`。
2. 导入基线锁定为上游提交 `1e0e494f1a86885eb332ca518543108ccd88d13e`，通过 `UPSTREAM.md` 记录来源、版本和本地修改。
3. 在已有 ImGui 扩展注册文件中调用 `nls_register_imgui_extension(NAME ImGuiNodeEditor)`，源码直接进入 `NLS_UI`。
4. 沿用仓库现有 ImGui 实例、分配器和渲染后端；处理 UI 模块共享库的 API 导出及跨模块配置一致性。
5. 只纳入核心源码，示例程序、其专用 ImGui 分支和示例构建体系不进入引擎构建。
6. 本地兼容和功能扩展直接维护在引擎源码中，保留上游版权及修改记录。

不建立独立 `ImGuiNodeEditor` 库目标，也不将它作为 Git 子模块或构建时下载依赖。

### 图文档与编辑行为

- 每个图编辑文档拥有独立 EditorContext，文档关闭时释放，并在 ImGui 关闭前完成清理。
- System、Emitter、Module、Function、DynamicInput 和 Scratch Pad 图共用图模型与文档适配层。
- 资产 GUID 与节点、引脚、连线 ID 建立稳定映射；复制粘贴产生新标识，撤销恢复原标识。
- 图模型负责类型、执行阶段、连接、循环及依赖合法性检查，验证成功后接受画布操作。
- 编辑操作统一走 `EditorUndoService`；节点和分组拖动使用连续编辑会话，一次手势提交一条历史记录。
- 节点位置、分组、注释存入资产编辑器数据；视野、缩放和选择存入用户缓存，通过保存回调接入，禁用默认 `NodeEditor.json`。
- 布局变化不改变程序语义和编译缓存键。
- 缩放、裁剪、DrawList 回调、嵌入式控件输入、Dock 隐藏恢复及 DPI 由集成测试验证。
- 模块栈、曲线/渐变、参数面板、时间线、预览、Gizmo 和编译诊断由 Nullus 实现，与节点画布共享模型和选择状态。

## PhysX 与物理场景查询

### 统一接口

新增共享物理模块及世界查询接口，粒子模块消费引擎类型，不在资产、脚本 ABI 或粒子模块公共接口中暴露 PhysX 类型。

- `PhysicsWorld` 拥有每个 Scene 的物理查询场景及对象映射，独立预览世界拥有独立查询场景。
- 提供 Raycast、Sweep、Overlap，支持最近命中、多命中、过滤层、静态/动态对象、触发器策略和忽略对象集合。
- 命中结果包含位置、法线、距离、面索引、物理材质和稳定对象标识；异步结果需要验证对象代际。
- 统一单位、坐标、法线、缩放及单/双面查询语义，并提供 C#/Lua 绑定。
- 碰撞形状覆盖盒、球、胶囊、凸包、三角网格及高度场，支持刚性对象和运动学对象的变换更新。
- Mesh Cooking 工件进入现有资产依赖与缓存管线，键包含几何、Cooking 配置、PhysX 版本和目标平台；热重载后的旧资源保留至查询任务结束。
- PhysicsAsset 数据接口消费骨骼附着碰撞形状、姿态与查询结果，所需姿态路径纳入动画前置工作。

### 线程与粒子消费

- 场景增删、变换及形状修改在明确的世界更新同步点提交；提交后发布查询 epoch，CPU 粒子任务只在该 epoch 的只读阶段执行。
- 在查询任务结束前禁止修改或销毁其场景和 Cooking 资源，场景切换与预览关闭需要等待所属任务退出。
- 多粒子查询按任务分块执行，使用每个任务独立的命中缓冲和预算统计，避免逐粒子线程调度。
- 普通物理查询直接使用 PhysX 场景/几何查询。场景可见性空间索引继续承担渲染职责。
- 离线 Mesh SDF 构建明确保留三角形材质、单/双面与符号判定规则，不能将包围盒级候选结果视为三角形命中；适用的精确几何查询复用 PhysX Cooking 数据。

### CPU 与 GPU 的边界

- CPU 粒子碰撞通过上述 PhysX 查询接口执行。
- GPU 粒子碰撞通过 FrameGraph 接入深度/GBuffer、解析形状、Mesh/Global SDF 和设备支持时的硬件射线查询。
- PhysX CPU 查询不会自动构成 HLSL 可调用的 GPU 查询；粒子 GPU 模拟继续沿用 DX12/Vulkan/Metal 的统一计算路径。
- GPU 导出至 CPU 的查询或事件采用显式异步、延迟结果语义，不在常规帧同步读回 GPU 粒子逐个调用 PhysX。

### 三平台前置任务

PhysX 固定为 5.6.1，源码提交为 `09ff24f3279b735e672ff27b155cbf49f6296f4d`。该版本上游构建预设及平台 CMake 目录仅包含 Windows/Linux，三平台可行性门槛如下：

1. 三平台使用同一固定源码版本及共享查询语义，以完整源码提交和归档校验值保证可复现。
2. 完成 Windows/Linux 的 CPU SDK、Cooking、链接与部署集成。
3. 补齐 macOS 所需的平台实现、CMake、编译器/架构配置及 CI 构建；GPU/CUDA 扩展不作为 CPU 查询的依赖。
4. 在 macOS 真机验证查询、Cooking、并发访问、场景释放及资源生命周期。
5. 三平台门槛通过后，PhysX 查询成为 CPU 碰撞和相关数据接口的正式前置依赖；尚未完成的端明确记录为未完成。

## 粒子系统架构

- 新增 `NLS_VFX` 运行时和 `NLS_VFXCompiler` 编译模块；Engine 接入组件、世界更新与场景代理。
- 资产类型包含 System、Emitter、Script、ParameterCollection、EffectType、DataChannel、SimCache，复用 AssetId、反射、序列化、依赖缓存、导入和资源管理。
- 模块栈和节点图共用语义模型，经类型化 IR 分别生成 CPU SIMD 字节码和 GPU HLSL。
- 编译器覆盖 Parameter Map、命名空间、自定义属性、函数、动态输入、静态开关、Scratch Pad 和 Custom HLSL；不支持的目标组合在编译期诊断。
- CPU 数据采用 SoA，按系统及发射器依赖批处理；GPU 使用持久属性缓冲、稳定 ID、存活压缩、排序和间接绘制。
- 支持 System/Emitter/Particle 的生成与更新、CPU 事件、Simulation Stage、预热、循环、插值生成、固定种子和局部/世界空间。
- Game Thread 发布参数与外部数据快照，Worker 执行 CPU 模拟，Render Thread 管理 GPU 资源及代理，RHI 使用已有命令提交路径。
- 一个世界帧只模拟一次，各视图分别剔除、排序和绘制；资源使用强句柄、代际及 GPU fence 管理。
- API 覆盖生成、挂接、激活、停止生成、立即停止、重置、暂停、种子、参数读写及 Seek，并同步开放 C#/Lua。
- Seek 采用重模拟或 SimCache；脚本成功编译后切换完整程序代际，失败保留旧版本，动态参数直接更新。
- 发布工件包含预编译 CPU 程序、GPU Shader 及其版本化资源布局，运行时不依赖编辑器编译模块。

## 其他前置能力

| 领域 | 本轮需要建设的能力 |
| --- | --- |
| GPU 基础 | 三后端 DrawIndirect、DrawIndexedIndirect、DispatchIndirect 的录制/翻译/提交；间接参数状态、UAV 同步、存储资源能力查询、异步回读；扫描、压缩、归约、排序和参数生成计算库。 |
| 材质与画面 | 粒子属性绑定、材质排列、软粒子、透明混合、深度淡出、畸变、运动矢量，以及 HDR、色调映射、Bloom 和体积渲染。 |
| 几何与动画 | 骨骼姿态、片段播放、蒙皮、Socket、通知事件、静态/动态网格采样、Spline、VectorField。 |
| 纹理与场景 | RenderTarget、Grid、邻域存储、地形高度场、虚拟纹理、稀疏体积及体积缓存的数据提供者。 |
| 音频 | miniaudio 播放、采样与混音输入，波形及 FFT 分析，为音频响应和 AudioPlayer 数据接口提供真实数据。 |

## 阶段与出口

| 阶段 | 交付内容与出口 |
| --- | --- |
| P0：参考基线 | 准备同版 UE Editor 和参考工程，冻结核心模块、节点、数据接口与工具清单，记录语义、平台限制、前置依赖及验收用例。 |
| P1：公共基础 | 内置 ImGuiNodeEditor 源码，接通文档和撤销；启动 PhysX 三平台门槛，补齐 RHI 间接命令及 GPU 计算基础。 |
| P2：资产与编译器 | 完成资产、参数、图验证、IR、CPU/GPU 编译、缓存、热更新与打包，同一简单图可运行于两种模拟目标。 |
| P3：CPU 闭环 | 完成生成/更新/死亡、事件、预热、循环、插值、空间变换及批处理；编辑器可创建、保存、播放、发布 CPU 特效，PhysX 查询就绪后接入 CPU 场景碰撞。 |
| P4：GPU 闭环 | 完成持久缓冲、ID、生成/更新、压缩、排序、包围盒、Simulation Stage 和 GPU 驱动绘制，常规帧保持异步资源流转。 |
| P5：完整渲染 | Sprite、Mesh、Ribbon、Light、Decal、Component、Volume Renderer，覆盖属性/材质绑定、SubUV、朝向、排序、LOD、阴影及编辑器选择。 |
| P6：完整编辑器 | 系统总览、模块栈、脚本图、Scratch Pad、参数、曲线、时间线、预览/Gizmo、模板、继承/版本、诊断和资产浏览接入。 |
| P7：模块与数据接口全集 | 按冻结清单逐项交付真实输入和示例，接通 PhysX、动画、音频等提供者，以及 Particle Reader、Data Channel、Grid、邻域与导出。 |
| P8：生产能力 | Stateless、EffectType、质量等级、预算/重要性裁剪、池化、SimCache、烘焙、VFX 序列轨道、调试 HUD、属性表及性能分析。 |
| P9：对齐收口 | 完成参考场景、三平台设备测试、独立运行包、模板和文档；验收矩阵中所有适用核心项目完成。 |

各阶段按依赖交错推进。前期闭环属于里程碑，完整目标以 P9 验收矩阵为准。

执行限制与 UE5.7.4 保持一致：GPU Event Script 在编译期诊断；Light、Decal、Component Renderer 使用 CPU 模拟；条件能力由设备查询及参考行为决定。

## 验证与完成标准

- 图编辑器：合法/非法建连、复制粘贴、分组、拖动撤销、多文档隔离、保存重开、Dock 隐藏恢复、文本输入与快捷键、DPI、缩放裁剪及上下文清理。
- PhysX：最近/多命中、Sweep/Overlap、层过滤、触发器、忽略对象、各形状、网格和高度场 Cooking、法线/面索引/材质、变换更新、并发查询、场景切换和资源热重载。
- 执行语义：阶段顺序、插值生成、事件、种子、ID、参数、空间变换、继承升级、热更新及数量上限；共同支持的 CPU/GPU 图使用明确数值容差对照。
- GPU：零粒子、线程组边界、扩容、计数溢出、压缩排序、多阶段读写、跨队列同步、间接参数、异步回读及延迟释放。
- 画面与数据接口：使用火焰、爆炸、雨雪、火花、拖尾、表面生成、碰撞、音频响应及体积等 UE 对照场景，检查前向/延迟、多视图、材质及全部数据接口。
- 性能：CPU 1千/1万/10万、GPU 1万/10万/100万粒子及大量实例，记录同机 UE 对照、CPU/GPU 耗时、峰值内存和显存；后续回归阈值默认10%。
- 稳定性：至少30分钟生成销毁、重载、场景切换和窗口缩放压力测试，检查任务和资源是否有界。
- 分层测试：快速行为测试进入单元套件，GPU/编辑器集成独立运行，长时间测试进入 Performance 套件。
- 最终完成：全部适用核心能力拥有可运行示例和证据，Editor 与独立运行包通过验证，DX12、Vulkan、Metal 各自在真实设备上通过验收。构建通过与设备运行通过分别记录。
