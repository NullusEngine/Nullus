# 粒子系统实施记录

基线：UE 5.7.4 Niagara。当前已建立 CPU/GPU 模拟、节点编辑、平台字节码工件，以及支持自定义片元材质、动态参数、SubUV、混合与深度淡出的 Sprite/Mesh 场景播放链路，完整 Niagara 对齐仍在实施。

## 2026-10-25 预览窗口闪烁修复

- **原因**：`VfxSystemPreview::View` 换图后会回收上一张图片的 UI 注册。渲染线程可能尚未准备引用旧图的 UI 快照；虽然 `ResolveForFrame` 允许这些待完成帧继续引用旧图，`EnsureBindingSet` 却直接跳过所有待回收项，导致首次准备时没有绑定，预览图片绘制被跳过。
- **修复**：共享 UI 纹理注册表的绑定准备接收快照帧号，复用与 `ResolveForFrame` 一致的有效范围判断。待完成帧可以创建和保留所需绑定，超出回收边界的快照仍无法使用旧 ID，GPU 完成后继续按既有流程释放资源。预览、Scene 相机、网格及坐标轴的绘制逻辑不变。
- **验证**：修复前两个新增用例稳定失败，连续 36 次换图、延迟 3 帧准备时均漏画。修复后 UI Overlay/纹理注册专项 **68/68**，包含 DX12 真设备连续 12 帧交替图片的像素读回和 D3D12 错误检查；粒子面板/预览/共享相机及辅助绘制回归 **103/103**，合计 **171/171**，0 失败、0 跳过。报告为 `build/validation/vfx-preview-flicker-20261025/{before,after-device,vfx-panel}.xml`。
- **交付**：Debug 与 Release 正常 Editor 构建部署通过，应用目录渲染 DLL 与构建目录 SHA256 一致，日志及哈希位于上述验证目录。需要重启既有编辑器进程以加载新库。本批验证覆盖自动化时序和设备像素，用户原工程的桌面闪烁现场未直接复验；完整 Niagara 对齐验收状态不变。

## 2026-10-25 原点坐标轴复用 Scene 绘制

- 将 Scene 的世界原点 X/Y/Z 轴生成提取为 `BuildEditorOriginAxes`，保留红/绿/蓝、1 像素线宽、5000 单位两侧延伸及深度测试策略。Scene/Asset View 的线程辅助 pass 与粒子预览均调用共享 `EditorGridRenderer::PrepareAxes`，消费既有 `DebugPrimitive.hlsl`；非线程路径仍将同一份轴数据提交到既有 DebugDrawService。线程路径清理旧的持久轴组，避免重复绘制。
- 删除粒子预览中的 ImGui 轴线绘制，`Preview.OriginAxis` 开关直接进入不可变渲染输入及暂停重绘判定。坐标轴可在网格隐藏时独立显示；按实际深度遮挡，跨近裁剪面的线段由 GPU 裁剪。每次绘制保留独立常量与资源所有权，相机变化和后续帧不会修改旧图像。
- 新增几何/样式契约及 DX12 RTX 5070 Laptop、Windows Vulkan Radeon 610M 设备用例，验证三个方向的颜色、深度遮挡、共享直接绘制与预览逐字节一致、暂停关/开、隐藏网格、相机变化和旧图像保留。原网格设备用例继续通过；专项 **5/5**，报告 `build/vfx-shared-axes-focused.xml`，像素证据 `build/vfx-shared-axes-evidence/`。
- 最终面板/相机/Scene 辅助回归 **103/103**，0 失败、0 跳过，报告 `build/vfx-shared-axes-delivery.xml`，日志 `build/vfx-shared-axes-delivery-tests.log`。Debug Editor 与面板测试程序构建部署通过（`build/vfx-shared-axes-delivery-build.log`），实际 Editor `--help` 通过（`build/vfx-shared-axes-editor-help.log`）。并行工作区更新运行库后，已同步测试目录并核对构建/测试/应用三处 DLL 哈希，再执行最终回归。本批验证包括设备像素与自动化交互，桌面人工操作未逐项验收；完整 Niagara 对齐状态不变。

## 2026-10-24 预览控制与 Scene 网格复用

- **相机控制**：粒子预览直接使用 Scene/Asset View 的 `Core::CameraController`。给现有控制器增加独立视口输入区域和非 GameObject 聚焦点接入；平移、环绕、飞行、滚轮、快捷键绑定、鼠标捕获、增量抑制和插值继续走同一实现。`VfxPreviewCamera` 只持有引擎 `Render::Entities::Camera`、Transform、聚焦状态，并生成粒子渲染/模拟快照；删除原有 Orbit/Look/Pan/Dolly/Move 和 `VfxEditor::PreviewNavigation`，投影由引擎 Camera 计算，适配层只转换粒子要求的深度范围。
- **状态与配置**：每个 System 文档保留独立相机和聚焦点，使用 Scene 的相机速度配置。切换文档、隐藏/关闭预览及关闭文档时释放自身捕获；失去窗口焦点停止处理输入。视口内导航时由共享控制器取得按键优先级。输入不可用时保留实际视口高度，避免拖出窗口后平移速度突变。
- **网格**：Scene/Asset View 的线程网格 pass 与粒子预览共用 `Rendering::EditorGridRenderer` 和现有 `App/Assets/Editor/Shaders/Grid.hlsl`。原有网格密度、距离/角度淡出和深度测试由同一着色器执行；预览的 ImGui 网格线循环已删除，网格颜色读取 Scene 设置。绘制捕获独立常量缓冲并保留到 GPU 完成，暂停时改变开关/相机也会重新绘制。Scene 的非线程旧后端仍通过原有材质入口使用同一 Grid 着色器。
- **设备修复与验证**：DX12 使用 Scene 的完整 `Geometry::Vertex` 输入格式，保留 Grid 着色器完整输入签名；直接记录网格时沿用粒子渲染器的显式附件转换，避免 Vulkan 后续 pass 丢弃网格内容。DX12 RTX 5070 Laptop 与 Windows Vulkan Radeon 610M 的测试验证可见像素、深度遮挡、同一后端的直接绘制/粒子预览逐字节一致、暂停开关及旧图像保留。像素捕获位于 `build/vfx-shared-viewport-evidence/`。
- **最终结果**：`NullusVfxEditorPanelTests` 完整回归 **97/97**，0 失败、0 跳过（`build/vfx-shared-viewport-delivery.xml`、`build/vfx-shared-viewport-delivery-tests.log`）。包含既有模块/多文档/预览设备用例，以及 Scene 相机输入/聚焦/状态机、辅助 pass 与管线状态检查。EditorProject 已完整重建，正常 Debug Editor 已部署并通过 `--help`；日志为 `build/vfx-shared-viewport-EditorProject-rebuild.log`、`build/vfx-shared-viewport-delivery-build.log`、`build/vfx-shared-viewport-editor-help.log`。桌面直接拖拽、捕获释放和快捷键路由尚未逐项人工验收；本批不改变完整 Niagara 对齐状态。

## 2026-10-24 UI PSO 渲染目标布局修复

画面停更现场中，Windows 事件轮询、ImGui 帧和 UI 快照仍在更新，窗口持续显示旧 Scene View。相同的 DX12 资源析构采样栈不能据此判断死锁。独立 Release 诊断实例捕获到 D3D12 验证错误 `RENDER_TARGET_FORMAT_MISMATCH_PIPELINE_STATE`（613）：UI PSO 没有声明颜色目标，而实际绘制目标为 RGBA8 交换链。

- UI 资源准备现在必须接收实际渲染目标布局。交换链路径从已获取的 backbuffer view 传入颜色格式、颜色空间及采样数；缺失颜色目标时返回明确错误。
- UI PSO 在设备或目标布局变化时重新创建；每个已准备帧保留对应 PSO，避免其他帧切换目标格式后使用错误管线。
- Release、Debug `NLS_Render` 重建通过；Release 运行库已部署到 `App/Win64_Release_Runtime_Shared`，构建和部署 SHA256 一致。新符号使用独立输出目录，避开原调试会话占用的 PDB。
- 新增 4 项回归通过：目标格式/颜色空间/采样数变化、帧间 PSO 保活、缺失目标拒绝，以及 RTX 5070 Laptop 真设备连续两帧从红色到绿色的像素读回。既有 UI/帧生命周期专项 **314/314**，合计 **318/318**，0 跳过。
- 四个既有 Debug 特效测试程序加载本轮新渲染库后 **225/225**、0 跳过：Authoring **98**、Panel **47**、Runtime **63**、Editor **17**。本批构建、XML、诊断栈、原始二进制备份、部署哈希和交互记录在 `build/validation/vfx-open-hang-20261024/`。
- 全量 `NullusUnitTests` 构建仍受既有图标字符串断言、预览相机测试 API 和 Game 测试链接错误阻断；本批渲染专项通过临时 MSBuild 筛选独立链接，未修改这些无关源码。原工程的最终交互复验结果见上述目录的 `validation.md`。

## 2026-10-24 Typed 单一路径收口

本批按要求删除旧编译/执行后端；此前“Typed 主路径 + 旧解释器回退”的迁移状态由本节覆盖。源资产仍使用现有 Graph/模块格式，运行时 `Program` 直接是 `Typed::BytecodeProgram` 的类型别名。

- **统一编译**：`CompileGraph` 只执行源图/聚合展开 → Typed SSA → 优化/布局 → Typed Bytecode 和 Typed HLSL。删除旧 `GenerateHlsl`、旧 Instruction/Program 存储和 `FromLegacyProgram` 桥接。Emitter 字面量求值也改为同一 Typed 常量求值，移除单独的标量图解释器。源值类型与 Typed 类型的映射改名为 `FromValueType/ToValueType`，不再出现迁移后端选择。
- **统一执行**：System Spawn/Update、Emitter Spawn/Update、事件策略与碰撞脚本、CPU Spawn/Update/Event 粒子脚本均执行存储的 Typed Bytecode。Emitter Spawn 初始化与 Reset 在同一持久状态行执行，支持精确 Int32 状态；GPU 的 CPU 调度脚本也消费同一字节码。删除 `ExecuteProgram`、`m_useTyped*`、重复程序副本和旧寄存器工作区；Typed 构造/属性绑定失败会拒绝初始化或返回编译诊断，不再切换后端。
- **验证与语义**：Typed 前端校验整个源图（含断开的非法节点）、类型/宽度、显式转换、阶段限制、重复写出、参数默认值和依赖关系。所有表达式读取阶段输入，最后统一提交输出。字节码在加载和执行前验证版本、寄存器/参数/属性布局与 DI 绑定；坏字节码在写入粒子前拒绝。移除旧后端后补齐了 Typed HLSL 的 Cross/Length/Dot/Normalize 分量宽度、零长度保护、Bool 数值转换与阶段随机流，并统一常量折叠/VM 的 Int32 回绕、除零与 INT32_MIN/-1 规则。
- **独立参考**：移除前一次性采集 118 个引擎模块的输出字段、活跃参数和逐粒子数值，保存到 `Tests/Reference/VfxCompilerBaseline.json`；测试只读该固定文件，检查源资产语义哈希与全部模块结果。该快照来自本项目移除前的解释器，不冒充 UE 实机结果。保留 `Niagara574ArrayContracts.json` 的 UE 源码契约、手写数值期望及 DX12/Vulkan 设备对照。删除临时 dump 测试的写文件行为，改为断言实际 Typed 事件内核。
- **运行包/缓存**：运行包统一为 **7**，内部 `bytecodeVersion: 1` 保存真实寄存器、输入类型、参数/属性布局、常量与 DI 元数据；不再在加载时解释/转换旧指令，格式 1–6 必须从源资产重新编译。编译器版本 **5**，磁盘 DDC envelope/schema **3**，使既有编译缓存失效；GPU shader 资源布局继续为 **12**。反序列化失败保持调用方旧程序。
- **验证记录**：本批源码起始快照、构建日志、四套回归、部署哈希和独立补丁位于 `build/validation/vfx-typed-only-20261024/`。最终四套完整回归 **225/225**、0 失败、0 跳过：Authoring **98/98**（24.591 秒）、Panel **47/47**（33.916 秒）、Runtime **63/63**（55.396 秒）、Editor **17/17**（81.295 秒）。包含 118 个模块固定参考、DX12/Vulkan 的 Bool→Float 数值对照和既有全部设备/场景用例。VFX/编译器、Engine/UI、EditorProject/GameProject/LauncherProject、四个测试程序、工具及 Editor/Game/Launcher Debug 重建通过；VFX 与编译器 DLL 在构建、两处测试和应用目录 SHA256 分别一致。Editor/Game `--help`、模块生成 **118 graph / 0 native-only**、生产旧符号零残留检查与本轮空白检查通过。新增用例覆盖无源码运行包的 System/Emitter Spawn/Update/Reset、Int32 精度与回绕、坏字节码/旧格式拒绝、断开节点诊断与阶段输入读取顺序。

单一 Typed 编译/执行路径收口不改变完整 Niagara 行为对齐的验收范围：UE 真实输出/诊断对拍、更多 DI 提供者与创作控件、前述事件/调度边界及非 Windows 真机仍保留为后续工作。

## 2026-10-24 Struct / Data Interface 运行通路

本批接通源资产、模块内联、编译、CPU/GPU 运行、无源码运行包和动态数据更新。下方此前的 StructMake/StructGet/DataInterfaceCall 未接通说明由本节覆盖；这仍不代表完整 Niagara 节点/提供者/编辑器全集已经验收。

- **源类型契约**：Graph 增加 `structs`、`dataInterfaces` 和 `typedSchema: 1`；新增 `StructMake`、`StructGet`、`DataInterfaceCall` 源 opcode，保留既有编号。结构体支持嵌套、按字段名读取、Select、User 参数及聚合属性读写；递归类型、缺失/重复字段、类型不一致、声明冲突、过深依赖和展开上限产生诊断。模块、函数与阶段图内联保留同一声明，字段默认值与数组 Int32/Bool 数据按位比较，避免 NaN 位模式被浮点相等比较误判。
- **唯一图语义**：`VfxAggregateLowering.h` 把结构体转成字段 SSA；User 结构体成为 `User.Name.field` 参数，聚合粒子属性成为字段列，复用现有粒子 SoA。模块整个结构体输出也经同一字段展开进入后续模块的 SSA。源码与 Typed IR 的结构体值分别由公共展开器处理，原有解释执行与 Typed 后端消费相同的结果，不增加原生模块执行分支。
- **Typed VM 与 Shader**：VM 实现 StructMake/StructGet 的嵌套值与字段路径，校验字段数、类型、寄存器依赖；直接构造的 Typed IR 也能生成 GPU 字段代码。DataInterfaceCall 在程序与 bytecode 中保留真实调用、绑定索引和签名。兼容解释执行、Typed VM 和 Typed HLSL 共用 Array/Curve 契约；模拟 Spawn/Update、Simulation Stage 与事件粒子脚本已进行设备对照。
- **当前提供者**：Array 的元素为 Float/Float2/Float3/Float4/Int32/Bool，支持 `Get(Int32)`、`Length()`、`IsValidIndex(Int32)`；浮点数组另有本项目扩展 `Sample(Float)`，把归一化坐标夹至 `[0,1]` 后线性插值。Curve 使用严格递增的 `(time,value,0,0)` 线性关键帧，支持 `SampleCurve(Float)`（`Sample` 为别名），区间外取端点。当前每绑定固定长度最多 4096 条、每系统最多 64 个绑定，单图最多 16384 个 uniform 记录；可写数组、运行时改变长度、RichCurve 三次插值/完整 LUT 烘焙和其他 Niagara 数据提供者未实现。
- **UE 源码校正与契约测试**：核对 5.7.4 的 `NiagaraDataInterfaceArrayImpl.h::VMGetValue` 与 `NiagaraDataInterfaceArrayTemplate.ush::Get` 后，修正 Array.Get：非空数组的负数/超大索引夹到首尾，空数组返回元素默认值；`IsValidIndex` 独立返回原索引是否合法。`Tests/Reference/Niagara574ArrayContracts.json` 保存四个 UE 参考源的 SHA256、函数签名与固定边界用例；测试分别验证兼容解释执行与 Typed VM，并在 DX12/Vulkan 上验证同样的边界。这是源码取证的契约回归，尚未生成 UE Editor 实际执行/完整编译诊断的 golden 对拍。
- **运行时绑定更新**：`SystemInstance::SetDataInterface(name, storageType, values)` 原子替换同尺寸数据，使用现有不可变 `SystemFrame::parameters` 快照下发，CPU/GPU 读取相同记录；修改数据不需要重新编译 ShaderMap。类型、数量或 Curve 时间顺序不合法时保留旧状态，直接参数批量更新也校验 Curve 的完整候选数据。旧 CPU/GPU 快照在后续更新及代理销毁后仍可读回。
- **运行包与缓存**：程序格式 **6** 保存接口定义、默认数据、调用参数数目和绑定；缺失/错配的 uniform 声明、错误索引/函数/签名及不受支持的旧格式均被拒绝，并保持调用方旧程序。编译器版本升为 **4** 使旧编译缓存失效。沿用 shader 布局 **12** 与 DDC envelope/schema **2**，因为资源仍是原有参数缓冲。剥离图节点/HLSL 的 DX12/Vulkan/Metal 输入 SPIR-V 平台包与跨实例 DDC 已验证。
- **验证**：本批报告、构建日志、部署哈希与任务开始快照位于 `build/validation/vfx-types-20261024/`；最终四套完整回归 **222/222**、0 失败、0 跳过：Authoring **95/95**（23.665 秒）、Panel **47/47**（33.883 秒）、Runtime **63/63**（53.927 秒）、Editor **17/17**（82.105 秒）。VFX/编译器、Engine/UI、EditorProject/GameProject/LauncherProject、四套测试、工具及 Editor/Game/Launcher 已按 ABI 依赖重建；VFX 与编译器 DLL 在构建、两处测试和应用目录的 SHA256 分别一致。Editor/Game `--help`、模块生成 **118 graph / 0 native-only** 及本轮空白检查均通过。专项覆盖源脚本往返、嵌套结构体 User 参数、整结构体输出、极值 Int32/Bool 位模式、空数组/越界索引、动态 Array/Curve 更新、坏数据原子拒绝、无源码运行包、事件、Simulation Stage、缓存命中和旧快照。

编译/运行通路已具备以上类型与提供者范围；专用 Struct/DI 创作控件、任意宿主接口注册/第一类接口引用、可写与变长数据、UE 全节点/诊断对拍、Linux/macOS/Metal 真机，以及前节列出的事件与调度边界继续保留。单独生成 Metal 输入 SPIR-V 不构成 Apple 运行验收。

## 2026-10-24 GPU 事件闭环与平台磁盘 DDC

本批继续编译器替换与运行时渐进迁移。下方 2026-10-20 的 GPU 接收端仅支持 SpawnedParticles、GPU→GPU 未实现、跨平台 shader 缓存未实现等描述是当时状态，由本节覆盖；完整 Niagara 对齐仍未验收。

- **GPU→GPU 事件**：共享缓冲按发射器划分，每个模拟子步最多保留 256 条事件，超出部分丢弃；Typed Spawn/Update/AdvanceSpawn 通过原子追加发布 Spawn/Death，GPU 接收内核直接读取位置、速度、法线、种类与源索引。粒子 ID 的高低 32 位按位保存，SingleParticle 选择不经过浮点转换。发布、消费与重置使用 UAV 屏障，帧包保留事件缓冲和绑定资源。
- **独立 handler 与执行模式**：每个 handler 独立编译策略和粒子图，统一使用 Typed IR/HLSL。GPU 源事件在 GPU 上执行策略；CPU 源事件继续在 CPU 求出接收策略并下发。SpawnedParticles、EveryParticle、SingleParticle 均执行各自的粒子图；支持动态接收数量、出生位置/速度覆盖，以及逐粒子随机初始年龄；事件 Random 使用事件 ID 作为 tick，随机年龄的 Update 保留整数参数位模式，越界年龄按 CPU 规则回退。按 handler 计数的 Max Events Per Frame 跨同一世界帧内的子步保留，多个源共享此上限。
- **子步顺序与诊断**：`GpuEventSchedule.h` 按发布者→接收者拓扑排序，固定步/预热的每个 GPU 子步按此顺序执行；资产数组重排不改变源绑定。自引用、缺失源与循环依赖在编译时拒绝。同一子步的多个出生 batch 追加到同一发布缓冲；下一子步清零，避免陈旧事件重放。CPU 事件队列在提交后才消费，失败录制重试从不可变帧重新设置整个事件批次。
- **CPU SingleParticle 修复**：原实现实际按 `particleId % count` 选粒子；现查找完整稳定 ID，不存在时跳过，与 GPU 路径一致。UE 5.7.4 的 `NiagaraEmitterInstanceImpl.cpp` 中 SingleParticle 实现已被注释禁用，因此稳定 ID 选择是本项目已选定的扩展语义，不作为 UE 现行执行行为的对照通过证据。
- **平台 ShaderMap/字节码 DDC**：`CompilePlatformPrograms` 从 `cacheRoot/ShaderMaps` 跨实例复用完整平台包。键覆盖可执行程序/设置、全部生成 HLSL、编译器版本、shader 布局、schema、目标后端/格式/profile 和实际 DXC 工具链指纹；取消/失败保持调用方旧平台包。`PlatformCompileCacheStats` 区分命中、未命中与真实 shader 编译次数。
- **磁盘完整性与 ABI**：缓存文件包含完整键和内容摘要，损坏、截断、格式不匹配均作为 miss 重建；复用带文件锁的原子写入设施，避免跨实例临时文件冲突。修复低层资产的空 handler 图阶段与复制发射器的重复图 GUID，替代身份按发射器/槽位确定性生成，保证产物可往返和缓存键稳定。shader 布局升为 **12**，编译器与 DDC schema 升为 **2**，新增 `ResetEvents`/`ReceiveEvents` 两个内核，旧布局需重新编译资产。
- **移除不完整雏形**：删除旧单 handler 生成接口、浮点参数按整数位读取、接收端错误缓冲偏移，以及修改粒子 Size/打印读回值的调试代码；常规出生与事件出生使用同一 Typed 粒子初始化函数。
- **验证**：完整四套测试与 ABI 构建记录见 `build/validation/vfx-compiler-20261024/`。设备用例覆盖 DX12/Vulkan 的反向资产顺序、多个子步、多个独立 handler、三种执行模式、CPU/GPU 源、消费上限、溢出丢弃、死亡/重置、次帧不重放与旧快照保留；DDC 用例检查跨实例命中时 shader 编译次数为零、损坏重编译及源码失效。最终四套完整回归 **215/215**、0 失败、0 跳过：Authoring **90/90**（21.318 秒）、Panel **47/47**（34.870 秒）、Runtime **61/61**（72.571 秒）、Editor **17/17**（114.706 秒）。最终整数参数/随机年龄设备专项另行 **2/2** 通过（`typed-event-age.xml`）。Engine/UI/EditorProject/GameProject/LauncherProject、Editor/Game/Launcher、VFX/编译器/工具和四个测试程序 Debug 重建通过；VFX 与编译器 DLL 在构建、两处测试和应用目录中的 SHA256 分别一致。Editor/Game `--help` 启动通过，模块生成 **118 graph / 0 native-only**，空白检查通过。

仍有边界：事件消费内核当前串行执行，GPU 动态事件出生按最坏容量预留空间与 ID，性能和精确计数分配尚未优化；CPU 源事件仍按世界帧汇总后在首个 GPU 子步投递，尚未携带各自的子步时间戳。GPU handler 的可写 Emitter 局部状态当前按接收 dispatch 初始化，尚未跨 GPU 接收调用持久化。Simulation Stage 内导致的死亡尚未发布事件；GPU→CPU 事件回读、GPU Collision 数据接口、自定义事件 schema 与跨 System 事件总线均未接入。StructMake/StructGet/DataInterfaceCall 的端到端资产/VM/HLSL 路径仍未实现；动态完成条件、GPU per-frame/per-unit、多动态 SpawnBurst 通道隔离及桌面人工、Linux/macOS/Metal 真机验收继续保留为后续工作。

## 2026-10-20 内建模块资产化（UE 对齐第一/二阶段）

- 引擎分发的 Module 资产以资产身份/版本独立存在，不再携带 `nativeModule` 或任何代码内建 id；资产可正常参与库搜索、版本选择与实例化。
- 新增 `CreateEngineModuleAsset`：由 `ModuleDefinition` 生成确定性 id（模块/版本/图/输入 id 稳定）的 Module 资产；`ScriptVersion` 保存输入、属性和 `capabilities` 能力标记，`ValidateScript` 只校验资产自身的输出契约与元数据。
- **能力标记**：`ModuleCapability`（`ForceLimit`、`ConsumesForce/Drag`、`Collision`、`SpawnBurst`、`CompleteIfUnused`、`PartitionParticles`、`EngineValue`、`Camera`）随资产序列化。编译器的力顺序、Solver 清通道、碰撞检测、字面量 burst、完成条件、分区和保留参数全部读能力标记，不再按模块 id 特判。
- **图化**：新增 `BuildModuleGraph`（`VfxStackCompiler`），以捕获模式复用同一套内建 lowering——输入转 `Module.*` 参数、写出转 `WriteAttribute`；`NullusVfxCompile generate-modules` 据此生成模块图。当前 118 个模块全部图化（`Collision`/`CollisionQueryAndResponse` 也改为发射器碰撞通道图）。
- **逐 stage 模块图**：`ScriptVersion` 新增 `stageGraphs`（按 `Stage` 键控），序列化为 `stageGraphs` 数组；`ScriptGraph(version, stage)` 在 lowering 时选择对应图，缺失时回退主图。生成器为每个粒子 stage 捕获一次，仅保存与主图不同的变体。FadeOverTime、InterpolateOverTime、InheritSourceMovement 因此图化：Spawn 图写初值，Update 图执行收敛/插值/继承。
- 顺带修复：`UpdateMeshOrientation` 之前读取未声明的 `Axis` 输入，现改为从 `Angular Velocity` 同时取方向和速率；`ApplyRotationVector` 的角速度换算把 3 个输入传给了二元的 `Multiply`，现拆成两级乘法。两处内建 lowering 的 bug 修复后均可图化。
- **Transient 力通道属性化**：新增 `Particles.PhysicsForce`（Float3）、`Particles.PhysicsDrag`（Float）、`Particles.PhysicsRotationalForce`（Float3）三个粒子属性。力模块图通过「读取 + 累加 + 写回」表达，`SolveForcesAndVelocity`/`ApplyInitialForces` 读取并清零；每个相关 Stack 在 lowering 前写入零值种子，保证逐帧重置。`LimitForce` 仍要求前面存在力模块；捕获模式允许生成独立力模块图。这样 Gravity/Drag/Point/Line/Vortex/Spring/Noise/Repulsion/Acceleration/Limit/ApplyInitialForces/MeshRotationForce 等均可用资产图执行。
- 编译器按资产的 `capabilities`、`attributes` 和 `requiresBefore` 解析相机/引擎保留参数、属性声明与模块依赖；`InstantiateModule` 用资产名作为显示/诊断标识，Stack 层的 Collision/Spawn/Event/Partition 策略读能力标记。
- 运行时编译器只使用资产图（`LowerScript`），不回退内建 lowering；`DecodeGraph` 的 opcode 上界更新到 `SpawnIndex`，含 SpawnIndex 的模块图可正常序列化/反序列化。
- **引擎内容只读**：`IsPathWithin` + `engineAssetsPath` 判定；`VfxLibraryDocument` 拒绝编辑/保存并给出诊断；库编辑器显示 “Engine content (read-only)” 横幅与 **Duplicate to Project**。`DuplicateScript` 为图化模块生成全新资产/版本/输入/图 id，保留能力标记，复制到 `<project>/Assets/VFX/Modules/` 后可编辑。
- 模块选择器去重：存在引擎资产时按资产名隐藏同名 C++ 表项，新添加的模块以资产身份落盘；旧系统里的裸内建模块解析为确定性资产 id。
- 双击模块（Overview/Details）按 UE 语义打开模块源资产：优先固定 `scriptAsset`，否则用 `EngineModuleAssetId` 解析引擎资产并通过 `VfxLibraryEditor.OpenDocument` 打开；找不到资产时回退展开输入。
- 验证：`VfxLibrary`、`VfxAuthoring`、`VfxStackCompiler`、`VfxStackEditor`、`VfxEditor`、`VfxLibraryDocument`、`VfxLibraryEditor` 与测试 TU 单独编译 0 错误；独立程序遍历 118 个图化模块（115 个粒子图逐指令/写出一致，事件/碰撞图由官方用例覆盖）；`NullusVfxAuthoringTests` 84/84、`NullusVfxEditorPanelTests` 47/47 全通过；`generate-modules` 输出 118 graph / 0 native。新增用例覆盖全部图化模块的写出/指令子集对比（写出按属性名/类型比较）、逐 stage 变体、引擎资产加载/确定性 id、`DuplicateScript` 身份重生成、只读文档拒绝编辑/保存、输入元数据往返与校验、参数驱动 Emitter 运行时程序。
- 未完成：GPU 碰撞数据接口、动态完成条件、发射器曲线年龄通道、Scratch Pad、版本升级、引擎资产图标与旧工程迁移。

## 2026-10-20 移除 nativeModule

- `ScriptVersion::nativeModule` 字段、JSON 字段、校验和 `ResolveModuleAsset` 的扫描回退全部删除；模块解析只使用 `EngineModuleAssetId`/`EngineModuleVersionId` 的确定性资产身份。
- `ModuleDefinition` 与 `ScriptVersion` 改用 `capabilities` 位掩码；生成器从定义写入资产，编译器读取资产。
- `ScriptVersion::attributes` 作为模块属性契约随资产分发，编译器属性预注册和种子逻辑不再回查 C++ 模块表。
- 依赖改为资产名：生成器把 `requiresModule` 转换为目标模块名，`LowerScript` 将资产 id/名称写入 `seen`，同时保留裸模块 id 兼容默认系统。
- 编辑器选择器按资产名去重；打开源资产优先 `scriptAsset`，否则按确定性资产 id 查找；`InstantiateModule` 用资产名作为显示标识。
- 验证：`NullusVfxAuthoringTests` **84/84**、`NullusVfxEditorPanelTests` **47/47**；`generate-modules` 输出 **118 graph / 0 native-only**。

## 2026-10-20 Emitter 图阶段（UE 对齐第一阶段）

- `ScriptStage` 新增 `EmitterSpawn`/`EmitterUpdate`；`EmitterAsset` 新增 `emitterSpawn`/`emitterUpdate` 图，随 System 与编译产物序列化；旧文件缺少字段时生成空图并保持合法唯一 id。
- 编译器为 EmitterSpawn/EmitterUpdate 栈构建图：`Emitter.SpawnRate`、`Emitter.SpawnPerFrame`、`Emitter.SpawnPerUnit` 累加，`Emitter.Duration`、`Emitter.Loop`、`Emitter.UnusedGracePeriod` 设置；随后对字面量发射器图求值并写入 emitter settings。发射器模块与粒子模块共用 Builder 图模型，不再走独立的原生分支。
- `EmitterState`、`SpawnRate`、`SpawnPerFrame`、`SpawnPerUnit`、`CompleteIfUnused`、`SpawnBurst` 均以图资产分发；字面量 burst 在编译期折叠为调度器时间表。
- **运行时执行**：`EmitterPrograms` 新增 `emitterUpdate` 程序；`EmitterScript` 在 CPU/GPU 的 `EmissionScheduler` 前逐 tick 执行，输出的 `Emitter.SpawnRate` 取代调度器费率（CPU 同时应用 `SpawnPerFrame`/`SpawnPerUnit`）。`SpawnRate` 绑定 `User.*` 参数时不再在编译期烘焙，运行中改参数立即生效。
- **运行时 EmitterState**：`Emitter.Duration`/`Emitter.Loop` 由 EmitterUpdate 程序逐 tick 应用到调度器（CPU/GPU），绑定 `User.*` 参数时运行中修改下一 tick 生效；`CompleteIfUnused` 仍要求字面量。
- **DynamicMaterialParameters 图化**：四个槽位现在始终写 `Particles.DynamicMaterialParameter*`（定义已注册四列，字面量也作为渲染器参数写入），资产图与 builtin lowering 一致。
- **SpawnBurst 数据通道**：动态 `Time`/`Count` 时由 EmitterUpdate 图计算 `Emitter.SpawnCount` 与持久化的 `Emitter.BurstPhase`，运行时按 tick 生成并消费 id；字面量路径继续使用原有 bursts 时间表，保持旧系统行为。
- **Event Handler 图域（第一阶段）**：每个 Event 栈编译为独立 handler 图/程序，写出 `Emitter.GenerateSpawn/Death/Collision` 与 `Emitter.ReceiveKind/SpawnCount/RandomAge`；CPU 每 tick 执行并更新事件生成 mask 与接收设置，`SystemInstance` 派发读取运行时 receiver。字面量事件图仍在编译期烘焙，动态输入运行时生效；事件派发仍为 CPU-only，receiver 的 source emitter 仍是静态配置。
- **逐事件 Handler（第二阶段）**：`CpuEmitter::ReceiveEvents` 现在对每个送达事件执行一次 Event 程序，事件载荷通过运行时参数 `Emitter.EventKind/EventPosition/EventVelocity/EventNormal/EventParticleId/EventSourceIndex` 注入；Handler 可按事件覆盖 `Emitter.ReceiveSpawnCount`、`Emitter.ReceiveRandomAge`、`Emitter.ReceivePosition` 与 `Emitter.ReceiveVelocity`。内置 Receive 模块把事件位置/速度/法线转发到对应 receive 通道。编译器将 `Emitter.Event*` 识别为保留参数，不当作未知用户参数。新增用例同时验证载荷驱动的 spawn count 与接收位置覆盖。
- **多 Event Handler（第三阶段）**：`EmitterAsset::eventHandlers` 为每个 Event Stack 保存独立 `id`、`name`、receiver 策略和 graph；`EmitterPrograms::eventHandlers` 与 `CpuEmitter` 运行时按 handler 分别执行和派发。多个 handler 可以在同一帧独立接收事件并使用不同 spawn count/random age，不再互相覆盖；编译产物按 handler 数组序列化。新增 `MultipleEventHandlersExecuteIndependently` 用例，并验证编译产物往返。
- **事件消费与源绑定**：新增 `EventHandlerProperties` 图资产（`Source Emitter Index`、`Execution Mode`、`Max Events Per Frame`），编译为 `Emitter.ReceiveSourceIndex/ReceiveExecutionMode/ReceiveMaxEvents`，写入 `EventReceiver::sourceEmitter/executionMode/maxEventsPerFrame`。源索引在编译期解析为稳定 emitter id 并拒绝自引用；`SystemInstance` 在每次 System Tick 内按 handler 计数，超出上限的事件被丢弃，0 表示不限制。三项属性随 handler 资产和编译产物序列化。
- **Execution Mode（第一阶段）**：`EventExecutionMode` 支持 `SpawnedParticles`（生成新粒子）、`EveryParticle`（把事件载荷写入接收 Emitter 的所有现有粒子）和 `SingleParticle`（按事件粒子 id 选择一个现有粒子）。新增 `Particles.EventPosition/Velocity/Normal/Kind/ParticleId/SourceIndex` 粒子属性，`SpawnedParticles` 也会把载荷写入新生成粒子。
- **逐粒子 Event 程序**：`EmitterPrograms::eventParticles` 为每个 handler 保存独立的粒子级程序；编译器从 Event 图中提取所有 `Particles.*` 输出及其依赖，编译成粒子程序，并自动把对应属性加入接收 Emitter 布局；没有自定义粒子输出时回退到标准 `Emitter.Event* → Particles.Event*` payload 图。`CpuEmitter` 在目标粒子上逐个执行。编译产物按 handler 数组序列化。
- **Collision 图域**：`Collision`/`CollisionQueryAndResponse` 编译为 `emitterCollision` 图/程序，写 `Emitter.CollisionEnabled/Radius/Restitution/Friction`；CPU 每 tick 应用并供现有 PhysX 碰撞 pass 使用。字面量碰撞仍在编译期烘焙；GPU 碰撞仍无数据接口。
- **Typed 编译基础**：新增 `VfxTypeSystem.h`，提供 `TypeRegistry`、`TypeDesc`/`StructLayout`、`ParameterMapLayout`、`DataInterfaceDesc` 和类型化默认值校验；当前作为新编译器的迁移边界，尚未替换运行时 `ValueType`/`Value` 路径。新增 `TypedStructAndParameterMapLayouts` 用例。
- **Typed SSA IR**：新增 `VfxTypedIR.h`，将已扁平化的旧图翻译成类型化 SSA 节点（标量/向量、属性引用、参数、写入、Struct/DataInterface 预留操作），并验证未知属性、类型宽度和输入数量。新增 `TypedIRTranslatesTheFlattenedSpawnGraph` 用例。当前 IR 尚未接管运行时，下一步是 Translator 优化和 CPU/GPU 后端。
- **Typed IR 优化器**：`OptimizeGraph` 支持常量折叠（标量/向量算术、比较、逻辑、Select、Convert、Length/Dot）、DCE、参数表构建和属性读写布局收集；新增 `TypedIROptimizerFoldsConstantsPrunesAndBuildsLayouts` 用例。
- **Typed CPU Bytecode**：新增 `VfxTypedBytecode.h`，将优化后的 Typed IR 编译为寄存器字节码（参数/属性索引、目标寄存器、输入类型），并提供标量 VM 执行器；已支持常量、参数、属性读写、算术、Clamp/Lerp、三角函数、Length/Dot/Cross/Normalize、比较/逻辑、Select、Convert、Random、NormalizedAge 及 Stage/Spawn 上下文。新增 `TypedIRCompilesToExecutableCpuBytecode`、`TypedBytecodeVmImplementsVectorAndMathOps` 用例。
- **Typed 验证接入编译链**：`CompileSystem` 现在对 SystemSpawn/Update、Emitter、Event Handler、Collision 和 Simulation Stage 图执行 Typed IR 翻译、优化、布局和 Bytecode 编译验证；旧运行时后端仍是主执行路径，但每个编译图都必须先通过新编译前端。
- **Typed GPU HLSL / ShaderMap 运行时**：新增 `VfxTypedShader.h`，从 Typed IR 生成完整 compute kernel（`BeginSpawn`/`SpawnParticles`/`UpdateParticles`/`AdvanceSpawn`/Simulation `Main`）、`VfxEvaluate`、参数/属性绑定和 `TypedShaderMap`（FNV 键 + entry/source）；支持比较/逻辑、数值 Convert、NormalizedAge、Random 和常用数学操作。逐项对齐了 `DeltaTime`（必须走函数参数，`AdvanceSpawn` 的初始年龄与帧步长不同）、`Random` 的 ID/stream/tick、`SpawnIndex` 的 birth ordinal、参数顺序绑定、`Convert` 语义、属性读取的补零，以及 **除法必须走安全 `VfxDivide`/`VfxIntDivide`**（旧后端的 0/0 保护，缺失会让位置变成 NaN 从而整帧不可见）。Typed source 现已作为 GPU spawn/update 的实际运行时来源，DX12/Vulkan 设备用例逐粒子、包围盒与预览像素对比通过；含 Event Handler 的 GPU Emitter 也已切到 Typed 路径，Typed spawn kernel 会用 `Emitter.EventActive/EventPosition/EventVelocity` 覆盖事件出生粒子的位置与速度（旧后端只声明未消费这三个参数）。
- **Typed Simulation Stage**：GPU Simulation Stage 的 compute kernel 也由 Typed 前端生成（`Main` 入口、`StageIndex`/`StageIteration` 取 `VfxPadding`、Age 不额外推进），不再回退旧 HLSL；DX12/Vulkan 设备用例逐粒子与包围盒对比通过。
- **Typed Shader 标量广播修复（真实缺陷）**：Typed 寄存器的标量只写在 x 分量，而算术表达式直接按 `float4` 寄存器相乘/相加，导致「向量 × 标量」只缩放 x 分量（例如 Simulation Stage 的 `position * 0.9` 把 y/z 写成 0）。现在所有二元算术、`Clamp`/`Lerp`/三角函数都会按输出宽度广播标量操作数（`float4(r.x,r.x,r.x,r.x)`），`Select` 仍与旧解释器一致地整寄存器复制。设备用例 `Dx12/VulkanEmitterLifecycleAndFrameGraphSpritesMatchCpu` 逐粒子对比由失败转为通过，新增 `TypedShaderBroadcastsScalarOperandsInVectorArithmetic` 用例锁定该行为。
- **GPU 事件脚本**：GPU receiver 现在与 CPU receiver 语义对齐——(1) 接收方的 Event Handler 程序在 CPU 上逐事件执行（`Emitter.ReceiveSpawnCount/ReceiveRandomAge/ReceivePosition/ReceiveVelocity` 覆盖生效）；(2) `GpuEventSpawn` 扩展出 normal/kind/particleId/sourceIndex/randomAge/overrideVelocity，GPU 事件出生使用随机初始年龄（同一批次共用一个随机值，CPU 为逐粒子随机）并按 CPU 规则决定是否覆盖速度；(3) Typed spawn kernel 内新增 `VfxEvaluateEvent`，在事件出生时执行 handler 的逐粒子程序（写 `Particles.Event*` 与自定义粒子属性）；(4) 事件 payload 参数（`Emitter.Event*` 与 `Emitter.EventOverrideVelocity`）在 spawn 程序中声明，shader 与运行时共用同一参数布局。GPU 接收端仍只支持 `SpawnedParticles`，GPU→GPU 事件仍未实现。
- **Typed 与旧后端差异测试**：新增 `TypedCompilerMatchesLegacyForSystemAndEngineModules`，用手写图做 **运行时逐粒子逐分量对照**（legacy `ExecuteProgram` 与 Typed `ExecuteBytecodeParticles` 在相同粒子数据上比较 Position/Velocity/Size/Age），并对 **118 个引擎模块资产**逐个做结构对照：legacy `CompileGraph` 与 Typed 前端都成功时，写出属性集合与「存活参数集合」（参数节点中真正喂给写出的那些）必须完全一致；两边都拒绝未声明的属性写出。用例把模块图所需的属性并集构造成「属性宇宙」，因此 118 个模块全部可独立编译（对比 118 / 拒绝 0），而不是只覆盖默认系统。
- **Typed CPU 粒子批量执行**：`ExecuteBytecodeParticles` 将 `BytecodeProgram` 绑定到 `ParticleDataSet` 属性，按粒子执行字节码并直接读写数据列；`CpuEmitter` 现在把 legacy Spawn/Update 以及 Event particle Program 自动转换为 Typed Bytecode，并优先使用 Typed VM 执行；转换失败时保留旧解释器回退。新增 `TypedBytecodeExecutesOverParticleData` 用例，完整 Authoring/VFX 测试在新路径下通过。
- **脚本哈希 / 编译缓存**：新增 `VfxCompileCache.h`，提供 FNV-1a 脚本/系统哈希、`CompileCacheKey`（source/compiler/shader-layout/schema/backend）、线程安全 `CompileCache` 和命中统计；`Compile(...)` 新增可选 `CompileCache*`，命中时跳过 `CompileSystem` 并复用 `CompiledSystem`。`CompileCache(directory)` 支持磁盘 DDC，使用原子临时文件写入和跨实例反序列化读取；`Compile` 缓存键现在包含依赖脚本语义哈希。新增 `ScriptHashingAndCompileCacheKeys`、`CompileCacheReusesCompiledSystem`、`DiskCompileCacheRoundTripsAndHitsAcrossInstances` 用例。完整版本迁移、依赖失效传播和跨平台 Shader 缓存仍未实现。
- **System Spawn/Update 阶段**：`SystemAsset` 新增 `systemAttributes`、`systemSpawn`、`systemUpdate` 图；编译产物新增系统级 `Program`，`SystemInstance` 在初始化和 Reset 时执行 SystemSpawn，每个 System tick 执行一次 SystemUpdate，并通过 `SystemFrame::systemAttributes` 发布系统属性。当前是资产/运行时能力，System 栈的编辑器 UI 和模块库仍待接入。
- **GPU 事件接收（第二阶段）**：CPU emitter 发布的事件按 GPU receiver 的 handler 配置收集到 `SystemFrame::gpuEvents`；`SystemRenderProxy` 在 GPU 录制前转发给 `GpuEmitter`。接收方的 handler 程序在 CPU 上逐事件执行（spawn count / random age / 出生位置与速度覆盖与 CPU receiver 一致），payload（normal/kind/particleId/sourceIndex）随事件下发，GPU spawn kernel 覆盖出生位置/速度、执行 `VfxEvaluateEvent` 的逐粒子 payload 程序并支持随机初始年龄。事件 spawn 作为额外 batch 写入 GPU 粒子，不经过 CPU 回读。当前只支持 `SpawnedParticles`；GPU 源事件（GPU→GPU）仍需事件缓冲。
- 限制：动态完成条件尚未运行时驱动；GPU 路径尚未应用 per-frame/per-unit；发射器曲线使用 `NormalizedAge` 仍缺发射器年龄通道；多个动态 `SpawnBurst` 模块共享 `Emitter.SpawnCount`/`BurstPhase` 通道，尚不支持同发射器多动态 Burst。
- 测试：新增 `EmitterUpdateGraphsDriveEmitterSettings`、`ParameterDrivenEmitterScriptRunsAtRuntime`（参数驱动费率 25→10 的逐 tick 生成）、`ParameterDrivenEmitterStateRunsAtRuntime`（参数驱动 duration=0.5s、非循环）、`ParameterDrivenSpawnBurstRunsAtRuntime`（动态 Count 的 per-tick burst）、`EventHandlerRunsPerEventWithPayload`（逐事件载荷驱动 spawn count）以及 `GpuEventReceiverRunsTheHandlerScriptAndPayloadProgram`（GPU 接收端在 CPU 上执行 handler 脚本并在 spawn kernel 内运行 payload 程序）；`NullusVfxAuthoringTests` 87/87、`NullusVfxEditorPanelTests` 47/47、`NullusVfxTests` 59/59、`NullusVfxEditorTests` 17/17；引擎资产 118 图化 / 0 native，事件与碰撞图由官方用例覆盖。
- 未完成：GPU 碰撞数据接口、动态完成条件与发射器曲线年龄通道。

## 2026-10-20 单一模块路径迁移

- 运行时编译不再走内建模块 lowering：`Builder::Lower` 在非捕获模式下只接受资产图；裸内建模块 id 会在 `Compile` 前按确定性资产 id 解析为引擎模块资产，空图资产直接报错。
- `SourceLibrary` 缺失引擎模块时从 `App/Assets/Engine` 加载一次作为兜底，因此默认 `CreateSystem`、旧 `.vfx` 与空库测试也走资产图；内建 lowering 仅保留给 `BuildModuleGraph`/`generate-modules` 的捕获路径。
- `SpawnBurst` 的字面量 `Time`/`Count` 在所有目标上做常量折叠，写入调度器 burst 时间表；动态输入仍由 EmitterUpdate 图写 `Emitter.SpawnCount`，`QueueBurst`/`BurstPhase` 保持跨 tick。
- 引擎资产仍为 **118 graph / 0 native**；`NullusVfxAuthoringTests` **84/84**、`NullusVfxEditorPanelTests` **47/47**。GPU 设备用例全部恢复。

## 2026-10-20 模块输入元数据（DT-08，部分）

- `InputMetadata` 新增 `displayName`、`tooltip`、`unit`、`visibleIf`、`hasRange`/`minimum`/`maximum`/`step`，由 `InputDefinition` 与 `ScriptInput` 共用；脚本资产按需序列化 `metadata` 对象，旧文件无该字段时保持默认值。
- 校验：字符串长度上限、范围有限且 `minimum <= maximum`、`step >= 0`，`visibleIf` 必须引用同一版本中存在的输入；反序列化失败时保留原资产不变。
- 引擎 Module 资产由 `CreateEngineModuleAsset` 复制定义元数据；模块编辑器从当前脚本版本的输入读取元数据（Duplicate 到工程后的模块同样生效），行标签显示单位，悬停显示提示/范围/步进；`visibleIf` 控制的行在控制输入为字面量 0 时隐藏（非字面量时 fail-open），字面量浮点值按范围钳制。
- 已为力/朝向/约束/时间等约 30 个高频输入补单位（`cm`、`cm/s`、`cm/s2`、`deg`、`deg/s`、`1/s`、`1/s2`、`x`）与可见条件（例如 `Point Force` 的 falloff 依赖 `Use Falloff`，`Pendulum Constraint` 的角度依赖 `Use Angle Constraint`）。
- 验证：`NullusVfxAuthoringTests` **84/84**（新增元数据往返/非法引用拒绝/引擎资产继承用例）、`NullusVfxEditorPanelTests` **47/47**；引擎资产生成现为 118 图化 + 0 native-only。
- 未完成：完整导入 UE 每个输入的可见条件/EditCondition/枚举与范围；多条件表达式、Advanced 折叠区、静态开关在模块行上的显式标记；其余模块的逐输入单位/提示填充。

## 2026-10-20 输入差异与重置（DT-09，部分）

- 模块输入行在「值/绑定与定义默认不同」或「存在继承覆盖」时显示 **Reset** 按钮；悬停提示区分 `Reset to default` 与 `Clear inherited override`。
- 本地模块重置写入字面量默认值；继承模块通过新增的 `ResetVfxModuleInput` 删除 `emitter.overrides` 中对应输入，覆盖项为空时移除整个 override，父资产保持不变；两者都走单次撤销提交。
- 差异判定覆盖字面量值、参数/曲线/脚本/动态输入绑定，并与 DT-08 的输入默认值/元数据一致。
- 测试：新增 `ResetVfxModuleInputClearsInheritedOverride`（继承覆盖清除 + 本地模块重置）；`NullusVfxEditorPanelTests` **47/47**、`NullusVfxAuthoringTests` **84/84**。
- 未完成：多级继承的逐级差异来源展示与逐级恢复、模块/阶段级 diff 汇总、跨 System 继承覆盖迁移。

## 2026-10-20 多选、剪贴板与注释/分组（OV-08/09）

- 多选模型：`VfxSession` 新增追加式 `SelectEmitter/SelectModule`、`Is*Selected`、`SelectedEmitters/Modules`、`ClearSelection`、`SelectModules`，以及单次撤销的批量操作 `SetModulesEnabled`、`SetEmittersEnabled`、`DeleteModules`、`DeleteEmitters`、`DuplicateModules`；`ReconcileSelection` 会修剪已删除的 id。
- 跨 System 剪贴板：新增 `VfxClipboard`（编辑器数据 JSON 快照 + 选中 id），支持模块与发射器的复制/剪切/粘贴；粘贴重新生成模块、动态输入图、发射器、阶段 id，可跨文档/跨系统；新增 `Edit.Copy/Cut/Paste` 命令与 Ctrl+C/X/V，Details 与 Overview 右键菜单均提供入口。
- 交互：Details/Overview 的模块与发射器行支持 Ctrl/Shift 点击追加选择；多选时右键菜单提供「启用/禁用/复制/删除 N 个」，Details 头部显示已选数量；Overview 发射器节点新增启用/禁用/删除/隔离上下文菜单。
- 注释与分组（OV-09）：`author::System` 新增编辑器专用 `comments`/`groups`（位置、尺寸、颜色、成员发射器），仅在 `editorData=true` 时序列化，`SemanticSource` 与运行包不包含；Overview 中通过 System 节点右键创建注释/分组，注释支持拖动、右下角缩放、双击改名、右键删除，分组按成员发射器包围盒绘制并支持删除。
- 测试：新增 3 个用例（多选与批量操作、跨系统模块/发射器剪贴板、注释/分组往返且不进入语义源）；`VfxSession.cpp`、`VfxClipboard.cpp`、`VfxEditor.cpp`、`VfxAuthoring.cpp` 与测试 TU 单独编译 0 错误。
- 未完成：Overview 画布框选（与画布平移冲突，需要专门选择模式）、注释颜色菜单、分组改名/拖入成员、跨 System 的继承覆盖迁移。

## 2026-10-20 编辑器 UI 批次完整回归

- 修复共享构建库的链接缺陷：`NLS_Build` 以动态库导出，`BuildPipelineInternal.h` 的 `Detail::DetectHostPlatform/Architecture/Backend` 缺少 `NLS_BUILD_API`，导致 `BuildProfilesPanel.obj` 在测试程序链接期出现 3 个 LNK2019；为这三个跨模块函数补导出宏，其余 `Detail` 函数保持内部。
- `NullusVfxEditorPanelTests` 全量 **35/35 通过、0 失败、0 跳过**（33.5 s，报告 `build/vfx-editor-ui-full.xml`），覆盖本工作流的 10 个新用例（命令/快捷键/面板可见性、相机数学 ×2、模块摘要/搜索、标尺映射、曲线映射、Step 语义）与原有 24 个用例（含 DX12/Vulkan 设备用例）。
- 修正测试夹具：独立 ImGui 上下文必须启用 `ImGuiConfigFlags_DockingEnable`，否则面板内部 DockSpace/DockBuilder 会访问未初始化状态并崩溃。
- `NullusVfxAuthoringTests` **66/66 通过**（3.2 s，`build/vfx-authoring-regression.xml`）。
- `NullusVfxEditorTests`（导入/场景）本轮无法重建：并行会话正在进行的 `AssetIconResolver` 重构在构建时缺少声明（`AssetBrowserPresentation.cpp`、`EditorResources.cpp` 于 09:12 仍在修改，31 个构建进程运行中），与本次改动无关；待其收口后重跑。
- 说明：`VfxEditorPanelTests` 的新用例通过面板公开命令 API 驱动，后续扩展命令时需同步维护 `ExecuteCommand`/`IsCommandChecked` 与测试期望。

## 2026-10-19 Timeline/Curves 交互批次

- 新增纯映射类型 `VfxTimelineView` / `VfxCurveView`（`Project/Editor/VFX/Editor/VfxTimelineView.{h,cpp}`）：可见时间窗 + 画布宽度映射、锚定光标缩放、平移、时间吸附、曲线双轴映射、key 命中测试、拖动夹取（保持相邻顺序与时间域）与 Fit；不依赖 ImGui，可直接单测。
- Timeline 重写：真实时间标尺（按缩放自适应 0.01–10 s 刻度并显示标签）、可拖拽播放头（拖动时显示 seek 预览，释放后 `RequestSeek`；拖动中暂停）、滚轮以光标为锚缩放、中键平移、播放范围右端手柄拖拽裁剪（吸附 0.05 s）、Loop 开关与到达范围末端自动重置、FPS 设置（1–240）与独立 Step/Step Back（使用文档帧率，不乘 playbackSpeed）、发射器轨道（名称、实时粒子数/GPU 标记、S 隔离按钮）。
- Curves 重写：网格与值坐标轴标签、滚轮缩放时间（Ctrl 滚轮缩放值）、中键平移、Fit、通道显隐、吸附、点击/Shift 多选、框选、拖动 key（时间夹取在相邻 key 之间，值随拖动调整）、双击空白插入 key、Delete 删除所选（至少保留一个）、Cubic key 切线手柄拖拽；下方保留逐 key 数值表用于精确编辑。
- `VfxSession::Advance(delta)`：忽略播放速度与暂停状态按精确 delta 推进，供单帧步进/拖动使用；正常 `Tick` 仍乘 playbackSpeed。
- 测试：新增 3 个用例（标尺映射/吸附/缩放锚点与 Fit；曲线映射/命中/夹取/Fit/值缩放；Step 语义与播放速度隔离），连同前几批共 10 个新用例。标尺与曲线映射的独立可执行检查 **23/23 通过**；`VfxTimelineView.cpp`、`VfxSession.cpp`、`VfxEditor.cpp`、`VfxEditorPanelTests.cpp` 单独编译 0 错误。
- 仍未完成：TL-02/03 发射器 Section/生命周期属性轨道（需要 authoring 侧 section 模型）、TL-08 轨道菜单/mute/solo/lock/自动关键帧、TL-09 Sequencer/SimCache 轨道集成、CV-03 完整切线类型/权重/外推、CV-05 渐变条与色标交互、CV-06 CPU/GPU 曲线结果对照；完整构建与测试运行仍需等待并行会话结束。

## 2026-10-19 模块栈 Details/Overview 对齐（输入摘要、版本徽标、紧凑输入行、行级操作）

- 共享行绘制器 `VfxStackRow` 新增 `summary`（右侧暗色输入摘要）与 `badge`（版本小徽标），在动作按钮之前裁剪绘制；`remove`/`muted` 接入模块行。新增两个纯函数：`VfxInputSummary`（参数绑定显示参数名，曲线/动态输入/脚本资产显示标记，与默认值不同的字面量显示为「输入 值」，最多 3 条）与 `MatchesVfxModule`（大小写不敏感、空格分词，覆盖名称、脚本 ID、类别、描述、输入名、属性名与参数绑定）。
- Details：模块行显示输入摘要与版本徽标，本地方案带删除按钮；搜索从「仅名称子串」升级为跨输入/类别/参数/属性的分词搜索，并显示「N of M modules match」。输入编辑改为 UE 风格单行布局——标签 + 值控件 + 绑定模式下拉（Value/Parameter/Curve/Script/Asset），连续拖动仍走文档单一撤销会话；Parameter 列出同类型用户参数，Script 内联资产下拉与版本/实参行，Curve/Dynamic Input 提供跳转按钮。
- Overview：模块行同样显示输入摘要，并支持删除、复制、上下移动、启用/禁用、重置到父级、编辑输入与模块拖放（拖到阶段行追加、拖到模块行插入，仅本地方案/模块）；阶段行显示「N modules / xN 迭代数」；发射器头部显示「CPU/GPU + 渲染器」摘要；System 节点的 Properties、User Parameters(N) 可点击并聚焦 Details/Parameters 面板，并显示 Emitters(N)。
- 测试：新增 2 个用例（摘要规则、搜索分词与参数绑定），与本批之前的 3 个命令/快捷键/面板用例合计 5 个（另 2 个相机数学用例在上一节）。验证：`VfxStackEditor.cpp`、`VfxEditor.cpp`（经临时 shim 绕过并行会话进行中的头文件问题）、`VfxEditorPanelTests.cpp` 分别单独编译 0 错误；完整构建与测试运行仍被并行会话的未完成改动阻塞（`Project/Editor/Icons/IconService.h` 的 `<Maths/Vector2.h>` 包含错误、`BuildProfilesPanel.cpp`/`MenuBar.cpp`/`EditorActions.cpp`），恢复后需重跑并记录报告。
- 仍未完成：DT-06 搜索高亮/结果导航/过滤器、DT-07 Life Cycle/Scalability/All 分段、DT-08 每输入可见条件与单位/步进元数据、DT-09 逐输入默认值重置与差异标记、DT-10 嵌套动态输入树与模式图标、OV-08 多节点多选同步、OV-09 注释/分组/剪贴板、OV-11/12 缩放提示与节点布局持久化。

## 2026-10-19 预览视口 3D 化（Orbit / 自由飞行 / 聚焦选中）

- 新增 `VfxPreviewCamera`（`Project/Editor/VFX/Editor/VfxPreviewCamera.{h,cpp}`）：`target + yaw/pitch/distance` 轨道相机，支持透视/正交、FOV、near/far 与自由飞行速度。矩阵沿用场景粒子渲染器的约定（行主序、列向量乘法、`Projection * View`、裁剪深度 `[0,w]`），并按场景路径同样的方式提取 cameraRight/Up/Forward 与模拟端 `CameraContext`。
- 预览视口改为真实 3D：LMB 拖拽 Orbit、RMB 拖拽 Look（原地旋转，eye 不动）、RMB+WASD/QE 自由飞行（Shift 加速，滚轮调飞行速度）、MMB 平移（按像素换算世界单位）、滚轮 Dolly。视口工具条加入 Perspective/Ortho、FOV、Focus、Fit All；拖拽仅在视口内起始时锁定，避免在其它面板拖动误转相机。
- 聚焦选中：`F` 在视口悬停时聚焦当前选中发射器（CPU 包围盒并集，含 spriteRadius），否则维持总览适配；新增命令 `Preview.FocusSelection`、`Preview.FocusAll`、`Preview.ToggleProjection`。GPU 发射器尚无 CPU 可见包围盒，聚焦只覆盖 CPU 范围（VP-08 的 GPU 范围仍是缺口）。
- 世界空间叠加替换原 2D 绘制：XZ 地面网格（步长随相机距离自适应）、3D 原点三轴（红X/绿Y/蓝Z）、CPU 发射器 3D 包围盒线框；粒子计数/FPS 覆盖层保留。
- `VfxSession::SetCameraContext` 每帧把编辑器相机发布给模拟实例（重建后自动重放），使相机相关模块与 Billboard 使用同一快照。
- 验证：`VfxPreviewCamera.cpp`、`VfxSession.cpp`、`VfxEditor.cpp` 与 `VfxEditorPanelTests.cpp` 单独编译均 0 错误；相机数学的独立可执行检查 23/23 通过（透视目标投影居中、视锥内深度落在 [0,1]、正交 `w==1`、Orbit 保持枢轴距离、Look 保持 eye、Pan/Move 沿相机基向量、Dolly 缩放距离、Focus 取景半径与相机上下文/基向量归一化）。面板新增 5 个用例（2 个相机数学、1 个命令状态、1 个快捷键、1 个面板可见性）。
- 完整 `NullusVfxEditorPanelTests` 构建与运行被并行会话正在进行的 `Runtime/Build` 重构阻塞（`BuildProfilesPanel.cpp`、`MenuBar.cpp`、`EditorActions.cpp` 当前编译失败，非本次改动），恢复后需重跑并记录报告。
- 仍未完成：VP-03/04 环境/天空/地面/灯光与独立场景、VP-06 网格与角落方向轴样式对齐、VP-08 GPU 范围/遮挡/LOD、VP-09 Gizmo/组件 Motion、VP-11 SimCache 录制回放、VP-13 Ribbon 预览仍按 Sprite 设置解析材质、相机视图状态持久化与 UE 视口菜单/像素级对照。

## 2026-10-19 反射属性与共享 Inspector

- Details 的 System、Emitter、Simulation Stage、Renderer、模块字面值/输入来源/参数绑定/脚本实参/版本选择，以及 Parameters 和库输入属性接入共享 `InspectorView` 属性行。属性值通过 `SerializedObject` / `SerializedProperty` 和原生 TypeTree 暂存；布尔、Int32、向量、颜色、枚举和字符串不再在 Details 中逐项选择 ImGui 控件。
- `VfxInspector` 的编辑视图和枚举实际注册到 `ReflectionDatabase`；字段投影从反射字段与 `InspectorMetadataRegistry` 获取名称、提示和控件元数据。动态模块输入继续使用模块/脚本定义中的类型，并映射为原生反射值。编辑视图属于 Editor，运行时资产格式没有升级。
- `ReflectedValueBackend` 只持有编辑值副本与文档弱生命周期，关闭/替换文档后拒绝写入。显示条件和禁用条件进入 TypeTree；只读字段也拒绝通过父对象整体赋值绕过。共享 Inspector 提供嵌入绘制模式，避免全局选择事件覆盖粒子选择；属性预览通过原有 VFX 文档事务提交，连续拖动只生成一次文档撤销，Escape 取消并回退。
- 模块/阶段增删、拖放重排、依赖修复、图编辑、预览隔离等操作保留各自的结构命令。此次是属性编辑链路改造，UE 界面/样式逐项验收、完整时间线及生产工具仍未完成。
- 回归 **148/148**、0 失败、0 跳过：VFX 创作 **66/66**（含新增 5 项反射/生命周期/连续撤销用例，`build/vfx-inspector-authoring.xml`）、原有 Inspector 与 SerializedObject **66/66**（`build/vfx-inspector-shared.xml`）、编辑器/设备/导入 **16/16**（`build/vfx-inspector-editor.xml`）。新增用例实际调用共享 drawer 注册链路，覆盖原生整数精度、反射枚举/显示名、只读/隐藏、父对象绕过拒绝、连续预览一次撤销、取消、保存重开和文档替换失效。现有多文档/三档字体缩放/隐藏恢复，以及 DX12/Vulkan 回读和混合预览像素用例通过。
- Debug EditorProject、Editor 和相关测试目标构建通过，已部署到 `App/Win64_Debug_Runtime_Shared`，实际 Editor `--help` 通过。临时 PDB 配置已恢复；沙箱助手的 `apply deny-read ACLs` 环境故障通过已授权的沙箱外补丁/构建进程处理。构建、部署和恢复日志为 `build/vfx-inspector-EditorProject-complete-build.log`、`build/vfx-inspector-Editor-complete-build.log`、`build/vfx-inspector-deploy.log`、`build/vfx-inspector-editor-help.log`、`build/vfx-inspector-restore-config.log`。尚未进行桌面鼠标逐控件人工验收或 UE 截图比对。

## 2026-10-18 单一新版编辑入口
- 统一移除 `Next` 命名：面板类与文件为 `Panels/VfxEditor.*`，会话/文档/样式位于 `Project/Editor/VFX/Editor/` 下并分别叫 `VfxSession`、`VfxDocument`、`VfxStyle`；矩阵路径与示例目录同步为 `Templates/VFX/Examples`。验收策略字段 `defaultEditor` 也改称 `module-stack`，不再以“Next”描述默认编辑器。历史批次中保留的报告文件名不变，以免把旧证据伪造成新证据。

- 用户明确要求新版成为默认，并移除旧版、只保留一条编辑路径；这覆盖了此前“全部验收后再切换”的入口约束。UE 对齐和平台验收状态没有因此变为通过。
- `Window > Particle Editor` 现在直接打开模块栈编辑器；资产浏览器创建 `.vfx` 使用同一格式，创建后、右键 Edit、双击和组件 System 引用的打开按钮进入同一窗口。CLI 统一为 `new` / `compile`，不再生成旧图格式或提供 `*-next` 双入口。
- 旧 `VfxEditor`、`VfxDocument`、`VfxGpuPreview` 实现及专用 UI/文档测试已移除。现有 `.vfx` 文件不自动转换；旧格式打开会在当前窗口显示明确诊断，保持文件和已打开文档不变。底层运行包和既有导入兼容仍保留。
- 切换后回归 **117/117**、0 失败、0 跳过：运行时 **54/54**（`build/vfx-cutover-runtime.xml`）、模块栈行为 **30/30**（`build/vfx-cutover-behavior.xml`）、编辑器/设备 **16/16**（`build/vfx-cutover-editor.xml`）、导入/场景 **17/17**（`build/vfx-cutover-import-scenes.xml`）。旧文档及旧窗口专用用例随实现移除；Mesh 设置撤销/保存和预览缩放/旧图像保留覆盖迁入模块栈测试，新增默认面板打开、重复打开、旧文件无损拒绝与错误窗口显现检查。
- 最新 Debug `EditorProject`、Editor、VFX、编译器、工具与相关测试程序已构建。`App/Win64_Debug_Runtime_Shared/Editor.exe` 已部署并通过 `--help`，Editor 及 VFX/Engine DLL 的 SHA256 与构建结果一致；未完成桌面菜单/组件按钮人工点击验收。命令行 `new`、`compile --backend all` 和预热示例三后端打包通过；旧格式及已移除的 `new-next` 被拒绝且源文件哈希不变。矩阵导出保留切换授权/本地证据，缺少授权和虚假验收状态会被拒绝。临时 PDB 配置已恢复。构建/部署记录为 `build/vfx-cutover-EditorProject-build.log`、`build/vfx-cutover-Editor-final-build.log`、`build/vfx-cutover-deploy.log`、`build/vfx-cutover-editor-help.log`、`build/vfx-cutover-restore-config.log`。
- 下方按批次记录的“默认旧版”说明是切换前的历史状态。当前默认和授权见 `NiagaraParity.json` 的 `defaultEditor` / `editorCutover`；验收脚本继续对未通过的 UE 要求返回非零状态。
- 按用户要求移除 `Next` 命名：编辑器面板类统一为 `VfxEditor`，会话/文档/样式统一为 `VfxSession`/`VfxDocument`/`VfxStyle`，目录为 `Project/Editor/VFX/Editor`；测试目标改为 `NullusVfxAuthoringTests`（33 项）与 `NullusVfxEditorPanelTests`（16 项），示例目录改为 `Templates/VFX/Examples`。验收策略字段由 `next` 改为中性的 `module-stack`。重命名后重新配置和构建，Editor/EditorProject/工具/测试程序全部通过；运行时/行为/编辑器/导入场景合计 **121/121**，0 失败、0 跳过（`build/vfx-rename-runtime.xml`、`build/vfx-rename-authoring.xml`、`build/vfx-rename-editor.xml`、`build/vfx-rename-import-scenes.xml`）。CLI `new`/`compile`、三后端包、旧格式无损拒绝和已移除别名检查通过；Editor 已重新部署并通过 `--help`，临时 PDB 配置已恢复。旧名仅保留在历史批次记录的既有报告文件名中，未改写旧证据。重命名同时清理了构建目录中旧目标生成的 manifest 与 `build/vfx-next-bin`；源码树与构建工程中已不存在 `VfxNext`、`VFX/Next`、`vfx-next` 命名，历史批次文字里的旧名只指向当时产生的报告文件名。

## 2026-10-18 System 固定时间步与预热

- 新增 System 的 Fixed Tick Delta Time、Warmup Time / Tick Count / Tick Delta。默认值参考安装版 `NiagaraSystem.h`：固定步关闭、固定 delta 0.01667 s、预热 delta 1/15 s；预热时间向下取整为完整 tick。固定步单次追帧上限为参考默认的 100，丢弃超出的完整步并保留小数余量。预热也经过固定步累计；它保留粒子和发射器历史，随后将对外 System age 从零开始。这里是源码语义核对，尚未宣称安装版 UE 行为或界面已验收。
- `SystemInstance` 生成不可变的实际模拟步列表，CPU 的 `TickSteps` 与 GPU 的 `RecordSteps` 逐步执行同一列表；固定步和预热不再被发射器的默认 60 Hz 分步重新切分。常规非固定帧继续保持旧分步策略。GPU 的整批录制在队列接受后才提交，早期帧仍保留自己的属性、ID 和范围。系统发布另外记录包括预热的真实模拟时间，继续检测漏掉的世界帧。
- 初始化/Reset 将预热留待首次活动 Tick（包括 Tick(0)），允许宿主先绑定参数覆盖和碰撞世界。候选编辑器在编译、重置和 Seek 前调用零时间 Tick 完成预热；Pause 不累计时间，Reset 清空余量并按当前参数重新预热。Seek 按已重放的输入时间结束，目标落在两个固定步之间时显示最近完成的模拟步，避免一直等待不可达的 System age。
- Details 的 System 控件接入文档撤销和语义编译；固定 delta 随开关禁用，预热 count/delta 随有效预热显示，并允许恢复被置零的 delta。预览为重复的不可变源帧复用渲染缓存，修复固定步未到期时重复提交同一序号导致预览失败的问题。
- System 源和编译程序使用格式 **5**，运行时 settings 使用版本 **3**；保留旧浮点和 Bool/Int32 源/包读取，带 timing 却伪装成旧格式会被拒绝。Shader 布局保持 **11**，VFX importer 为 **17**。新增 `Templates/VFX/Examples/SystemTiming/WarmupFixedTick.vfx`，红色 CPU 与绿色 GPU 发射器共享半秒预热和 0.03125 s 固定步；三后端无源码包位于 `build/WarmupFixedTick.compiled.json`。
- 最新回归合计 **124/124**、0 跳过、0 失败：运行时 **61/61**（含 5 项新增时间行为，`build/vfx-timing-runtime.xml`）、候选行为 **29/29**（`build/vfx-timing-next-behavior.xml`）、候选编辑器/设备/导入 **15/15**（`build/vfx-timing-next-editor.xml`）及旧版编辑器/场景 **19/19**（`build/vfx-timing-legacy-editor.xml`）。新增 DX12 RTX 5070 Laptop / Windows Vulkan Radeon 610M 用例在 129 粒子、137 上限、初始容量 3 下，按 ID 比较全部属性；Float 容差 `1e-5`，Int32 tick 计数逐位一致。覆盖预热和固定步相位、暂停/重置、漏帧拒绝、参数覆盖、代理销毁后的旧快照；混合预览真实读回红/绿像素，原生导入移除全部源后仍保留 timing。DX12 调试层和 Vulkan Khronos 验证层启用，Vulkan 错误计数不增加，日志无 VUID/验证错误。
- Debug VFX、编译器、Core、Engine、EditorProject、测试程序、CLI 和 Editor 已构建；最新 Editor 的 `--help` 通过。共享 PDB/对象输出竞争曾导致增量构建失败并混入旧结构布局；调试器定位到旧 `VfxDocument` 对象，完整重建后候选行为异常消失。最终预览修复通过选择文件编译与重新归档/链接验证，避免覆盖其他构建占用的输出。本轮测试使用 `build/vfx-timing-verification/Debug` 独立部署，临时编译参数和 CMake PDB 配置已恢复。关键日志为 `build/vfx-timing-NLS_Engine-rebuild.log`、`build/vfx-timing-EditorProject-rebuild.log`、`build/vfx-timing-preview-final-compile.log`、`build/vfx-timing-editor-archive-final.log`、`build/vfx-timing-NullusVfxEditorPanelTests-verification-build.log`、`build/vfx-timing-Editor-verification-build.log`、`build/vfx-timing-editor-help.log` 和 `build/vfx-timing-restore-config.log`。
- 候选限制为 fixed delta 0.0001–1 s、warmup delta 0–1 s，最多 1024 次预热 Tick / 1024 次展开后的模拟步；超出范围给出诊断。完整世界批处理、共享 Simulation 调度相位、数据接口/系统依赖、组件级预热覆盖、全部 UE 条件及桌面交互对照仍待完成。无新增性能基准、30 分钟压力或 Linux/Metal 真机结论；默认入口继续保留旧版。

## 2026-10-18 Bool / Int32 类型贯通

- 新增真正的标量 Int32 与 Bool：Int32 保留全部 32 位，Bool 使用 Niagara 的 `-1/0` 掩码；CPU 属性列、寄存器、参数、冻结帧与 GPU 存储保留原始位模式。JSON 使用整数及 true/false，不经 Float 数值转换；覆盖 `16777217`、Int32 最小/最大值和 Bool 掩码。
- 类型化 IR 支持 Int32 加减乘除、Min/Max/Clamp/Abs，以及标量比较、Bool 逻辑、Select 和显式 Convert；跨 Float/Int/Bool 的算术和赋值需要转换。整数加减乘按 32 位回绕，除零得到零，最小值除 -1 保留最小值。Stage Index/Iteration 可输出 Int32。编译器、运行包解码和解释器共用类型规则，错误类型不替换上一成功程序。
- GPU 模拟改用 uint 属性/参数缓冲读写，整数和 Bool 寄存器使用 uint4，浮点属性显式位转换。设备验证发现 DX12 的浮点表达式路径会丢失 Int32 最小值对应的 `-0` 位模式，已移除该路径；DX12/Vulkan 均逐位对齐 CPU 参考。其他 Float 渲染/剔除缓冲继续消费同一 32 位槽位。
- System、脚本图、模块输入、动态输入/函数实参、静态 Bool 开关和独立库资产支持新类型。编辑器提供整数输入、Bool 复选框、类型选择与新运算节点；源资产和运行包使用格式 **4**，运行时 settings 使用版本 **2**，Shader 布局 **11**，VFX importer **16**。v2/v3 浮点源与兼容的旧布局 10 运行包保留读取，新类型伪装成旧格式会被拒绝；现有源文件不自动改写。
- 阶段启用可绑定 Bool，迭代数可绑定 Int32，运行包保留类型和默认值。`SetIntParameter` / `SetBoolParameter` 进入组件和自动生成的 C#/Lua Script API，类型化入口拒绝不匹配的声明；旧 Float 控制绑定继续兼容。新增 `Templates/VFX/Examples/TypedStages/TypedSystem.vfx`，CPU 与继承 GPU 发射器共用 Int32/Bool 模块及静态开关，三后端无源码包为 `build/TypedSystem.compiled.json`。
- 已通过运行时 **56/56**（含 4 项类型行为）、候选行为 **28/28**、候选编辑器/设备 **13/13**（含 2 项类型设备用例）及旧版编辑器/场景 **19/19**，合计 **116/116**、0 跳过、0 失败。报告为 `build/vfx-typed-runtime.xml`、`build/vfx-typed-next-behavior.xml`、`build/vfx-typed-next-editor.xml`、`build/vfx-typed-legacy-editor.xml`。DX12 RTX 5070 Laptop 与 Windows Vulkan Radeon 610M 在 129 粒子/137 上限及扩容下，整数/Bool 属性按 ID 逐位比较；Float 容差 `1e-5`。覆盖动态启用、0/3 次迭代、重置后溢出、代理销毁后的早期快照和类型错误原子拒绝。DPI 绘制覆盖类型化参数控件；Vulkan 验证错误计数不增加。修复了共享输入解码后曲线重复追加的回归。
- Debug VFX、编译器、Engine 反射/运行库、Scripting/托管程序集、EditorProject、Editor、工具及相关测试目标构建通过；Editor `--help` 通过。记录为 `build/vfx-typed-runtime-final-build.log`、`build/vfx-typed-compiler-final-build.log`、`build/vfx-typed-engine-build.log`、`build/vfx-typed-scripting-build.log`、`build/vfx-typed-editor-project-build.log`、`build/vfx-typed-editor-build.log`、`build/vfx-typed-editor-help.log`。在 `build/vfx-typed-verification/Debug` 独立部署当前运行库验证，保留共享目录中的其他构建与运行进程。临时 PDB 输出配置已移除并重新生成工程（`build/vfx-typed-restore-config.log`）。本批未新增性能基准或长时间压力结果。
- 更多 Niagara 类型、整数向量、完整类型转换/运算库、粒子状态筛选、数据接口与 UE 视觉/交互/行为对照仍待完成。C#/Lua 方法已生成并构建，完整脚本场景尚不构成验收；默认粒子入口继续保留旧版。

## 2026-10-18 Simulation Stage 动态控制

- 新版阶段提供 **Enabled Binding** 和 **Num Iterations Binding**，可选择 System 的标量 User 参数；库发射器可声明绑定名称，实例可独立覆盖或清除绑定并恢复父级。参数面板对启用绑定使用复选框、对迭代绑定使用整数控件；User Parameters 修改临时预览值，不改变文档或编译代际。统计显示当前参数下的实际启用状态和迭代数。
- 每帧在录制任何 GPU 命令前冻结并校验控制值，帧内各子步使用同一控制快照。静态禁用继续禁止执行；动态关闭、零迭代及负的运行时迭代值跳过阶段。单阶段 128、总计 1024 次活动迭代的候选上限继续保留，非整数、非 0/1 的启用值和超预算修改拒绝提交，保留原参数与世界帧。
- `SystemInstance::SetParameters` 与 `GpuEmitter::SetParameters` 原子处理整批参数，避免依次设置导致临时超预算或部分更新。预览重编译、场景热重载和 RenderProxy 参数发布均使用整批设置；新程序无法接受原参数时保留旧组件实例和代理。现有 SetFloatParameter/SetVectorParameter 场景与脚本入口可驱动阶段控制。
- 动态控制和零字面迭代使用 System 源格式 **3** 和运行包格式 **3**，现有 v2 源/运行包仍可读取；带新控制却声明旧格式的输入被拒绝。发射器库的内部 System body 同样版本化，防止旧工具静默忽略绑定。Shader 布局保持 **10**，VFX importer 升至 **15**。
- 新增 `Templates/VFX/Examples/DynamicStages/DynamicSystem.vfx` 与配套库资产。`build/DynamicSystem.compiled.json` 包含 3 套后端、3 个阶段、启用/迭代绑定与默认值，节点和 HLSL 已剥离。
- 本批候选行为测试 **28/28**、编辑器/设备/导入测试 **11/11** 通过（`build/vfx-stage-controls-behavior.xml`、`build/vfx-stage-controls-editor.xml`）。新增默认绑定与继承清除、运行时修改不重编译、原子预算拒绝、旧帧不变、源/运行包版本边界和热重载回退。Windows DX12 RTX 5070 Laptop 与 Vulkan Radeon 610M 实际回读 Position/Size/Age，覆盖动态禁用、3/0/负迭代、重置、放弃录制和录制后改参；属性容差 `1e-5`。两个后端均验证剥离源码包预览像素，Vulkan 验证错误计数不增加。
- 旧版 VFX **52/52** 与编辑器/场景 **19/19** 回归通过（`build/vfx-stage-controls-legacy-vfx.xml`、`build/vfx-stage-controls-legacy-editor.xml`），本轮合计 **110/110**、0 跳过、0 失败。Editor 构建与 `--help` 通过。共享目录中的其他构建继续运行；本轮对已构建依赖使用 scoped MSBuild 编译/链接，在 `build/vfx-stage-controls-verification/Debug` 独立部署当前运行库后验证，避免覆盖其他进程占用的 DLL。对应日志为 `build/vfx-stage-controls-runtime-final-build.log`、`build/vfx-stage-controls-engine-build.log`、`build/vfx-stage-controls-editor-project-build.log`、`build/vfx-stage-controls-tests-build.log`、`build/vfx-stage-controls-editor-tests-build.log`、`build/vfx-stage-controls-editor-build.log`。临时 CMake PDB 输出配置已恢复。
- 参考核对包括 `NiagaraGPUSystemTick.cpp` 的逐帧 EnabledBinding/NumIterationsBinding、非正迭代跳过和 `NiagaraSimulationStageBase.h` 的显示条件；它是源码核对，尚不构成安装版行为验收。完整 Bool/Int 类型、System/Emitter 表达式计算的控制、UE 的更高迭代范围、数据接口迭代/执行筛选、状态筛选和自定义线程布局仍待完成。当前控制存储仍为 Float 标量，不能宣称完整类型对齐；默认粒子入口保持旧版。

## 2026-10-18 新版 Simulation Stage 创作链路

- 新版模块栈可编译为真实 GPU Simulation Stage；保留顺序、禁用项、稳定 ID 和迭代数。每个子步在普通 Spawn/Update 后执行，迭代读取前一轮结果，Age 不额外推进；每轮模块共享按顺序更新的 SSA 属性映射。CPU 目标、缺失参数和错误上下文给出带阶段位置的诊断，编译失败保留上一成功程序。
- 阶段可使用候选力/求解/颜色/尺寸模块、独立版本模块、嵌套函数和动态输入，并读取 User 参数的默认值及运行时覆盖。脚本图可创建 Stage Index / Stage Iteration 节点；模块跨阶段移动时更新内嵌图的上下文。
- Particle Editor Next 与 Particle Library Editor 补齐阶段增删、复制、命名、启停、排序和迭代数；System 工作区支持拖动阶段头部，生成代码与统计包含各阶段。复制阶段和发射器会重新分配内嵌图 ID，操作进入各文档独立撤销。
- 继承的阶段状态、名称和迭代数通过源阶段 ID 独立覆盖，可恢复父级；局部阶段追加到继承阶段之后。同一源的多个实例保留独立且确定的运行图 ID。覆盖引用失效会拒绝编译和导入。
- 新增 `Templates/VFX/Examples/SimulationStages/IterativeSystem.vfx` 及配套 emitter/module/function 源资产，展示循环偏移、禁用阶段、参数和继承迭代覆盖；三后端无源码包生成于 `build/IterativeSystem.compiled.json`，布局 **10**、3 个阶段，Shader/节点源码已剥离。VFX importer 升到 **14**；运行时包 ABI 不变。
- 修复验证发现的预览问题：已有合法后端字节码时直接复用，避免对已剥离 HLSL 的运行包再次强制编译。
- 最新候选行为测试 **24/24**、编辑器/设备/导入测试 **8/8** 通过（`build/vfx-next-stages-behavior.xml`、`build/vfx-next-stages-editor.xml`）。新增 5 项阶段行为和 3 项设备/导入用例；验证模块内 SSA、跨阶段/迭代读取、阶段与迭代索引、禁用、参数默认/覆盖、继承与失效覆盖、复制内嵌图、撤销/保存，以及导入失败保留工件和隐藏全部源资产后加载。DX12 RTX 5070 Laptop 与 Windows Vulkan Radeon 610M 的阶段 Position/Size/Age 回读容差为 `1e-5`，覆盖暂停、重置、后续帧和代理销毁后早期快照；两个后端均绘制出剥离源码程序的绿色粒子。DX12 调试层与 Vulkan 验证层启用，Vulkan 错误计数不增加。
- 相关旧版回归 **52/52** 与编辑器/场景 **19/19** 通过（`build/vfx-next-stages-legacy-vfx.xml`、`build/vfx-next-stages-legacy-editor.xml`），加上候选测试本轮共 **103/103**，0 跳过、0 失败。Editor、候选测试与工具编译/链接通过，Editor `--help` 通过；旧版目标在已构建依赖上禁用 post-build 重新链接，并在独立目录部署最新运行库后测试，避免覆盖原已运行的 `NullusUnitTests` 所占用的公共 DLL。本机 PDB 锁通过临时独立符号目录规避，CMake 的临时 PDB 配置已移除。构建日志为 `build/vfx-next-stages-editor-final-build.log`、`build/vfx-next-stages-legacy-build.log`、`build/vfx-next-stages-legacy-editor-build.log`；工具的公共目录部署曾被锁占用，独立目录的最新工具生成示例包通过。
- 当前阶段仅遍历存活粒子。动态 User 标量控制的后续实现和限制见本页“Simulation Stage 动态控制”；数据接口迭代、执行与状态筛选、自定义线程布局，以及完整 UE 样式/交互/行为对照仍待完成；默认入口继续保留旧编辑器。

## 2026-10-18 Ribbon/Beam 渲染器与 Beam 模块

- 新增 **`RendererType::Ribbon`** 与 `RibbonSettings`（宽度属性、材质/贴图、混合、色调、发光、UV 平铺距离、材质着色器开关），随系统序列化并做范围校验；`ValidateRibbonSettings` 拒绝非法混合模式、非有限色调与非法 UV 平铺距离。
- 新增 `BuildRibbonShader`：顶点着色器 `RibbonVS` 把每粒子 6 个三角形列表顶点映射到一段条带，段连接粒子 `i` 与 `i+1`（末粒子自重复形成退化四边形、不产生像素）。宽度取自标量属性（默认 `Particles.RibbonWidth`），侧向量由段方向与相机基向量叉积得到，因此条带始终朝向相机。为此增加 `AttributeExpressionAt`，让属性表达式按**任意索引变量**取列（原有实现硬编码 `index`，无法读相邻粒子）。
- **复用而非新建绘形状**：每粒子 6 顶点与 Sprite 相同，因此间接参数的 `vertexCount` 语义不变，不需要新的计数器布局或拓扑，也**没有**改动 Shader 布局版本（仍为 11）与运行时包 ABI。`KernelName` 改为按 `RendererType` 解析入口，`CompileSystem`/`VfxShaderCompiler`/`ParticleSceneRenderer` 分别接入 Ribbon 分支。
- 新增 5 个模块（模块库 88 → **93**）：`RibbonWidth`、`SpawnBeam`、`UpdateBeam`、`BeamWidth`、`ScaleBeamWidth`。`RibbonWidth`/`BeamWidth`/`ScaleBeamWidth` 写入 `Particles.RibbonWidth`；`SpawnBeam` 放置初始位置并建立 link order；`UpdateBeam` 每帧按 link order 在端点间重算位置。
- **已知限制（如实记录）**：① UV 目前按段正交归一化（每段 0→1），而非参考实现的"按距离累积平铺"，因此长条带上的贴图会拉伸；`uvTilingDistance`/`tileUvOverDistance` 字段已保留但当前不参与计算——真正的按距离平铺需要在顶点着色器外加前缀和或改用几何着色器路径。② `BeamEmitterSetup`（起点/终点/切线管理）未实现。
- 新增 1 个行为用例（`Tests/Unit/VfxAuthoringTests.cpp`）：`RibbonRendererCompilesAndValidatesItsSettings` 断言 Ribbon 渲染器可编译、着色器文本含 `RibbonVS`/`RibbonPS`、序列化往返保留渲染器与设置、`ResolveRibbonDrawSettings` 映射材质与混合、非法混合模式与零 UV 距离被拒绝、`RibbonWidth` 模块注册宽度列后着色器仍可构建。
- 本轮验证：行为/授权套件 **54/54**（`build/vfx-ribbon-authoring.xml`）、VFX 套件 **55/55**（`build/vfx-ribbon-vfx.xml`）；随后新增 Ribbon 设备像素用例 `VfxSpriteTests.Dx12RibbonStripCoversAContiguousBand` 与 `VfxSpriteTests.VulkanRibbonStripCoversAContiguousBand`，两后端均通过（报告 `build/vfx-ribbon-pixels-vfx.xml`，全量 VFX **57/57**）。测试让 4 个粒子沿 Y 排列，读回 64×64 像素后断言高亮行数大于 8 且首末高亮行之间没有空隙，证明相邻粒子确实连成连续条带而不是绘成孤立四边形。
- 覆盖率：库可见非废弃目标 158 个中已实现 **93** 个（58.9%）。

## 2026-10-18 模块库第十一批（渐变/插值、距离约束、单摆与点力）

- 模块库从 **95** 个扩到 **103** 个，按冻结清单中库可见非废弃目标的保守名称匹配口径，覆盖率从 **73/158** 提升到 **81/158（51.3%）**；本库中的若干别名和扩展入口（例如 `RibbonWidth`、`ColorOverLife`）不对应清单条目，不能计入该百分比。新增 `ScaleRibbonWidth`、`FadeOverTime`、`InterpolateOverTime`、`MaintainA_SetDistanceBetweenPoints`、`PendulumSetup`、`PendulumConstraint`、`PointForce` 和 `InheritSourceMovement`。
- 新增标量状态列 `Particles.FadeValue` 与 `Particles.InterpolatedValue`。`FadeOverTime` 按 `rate × deltaTime` 收敛且用 `Maximum(target, minimum)` 与最终下限钳制保证不会穿越；`InterpolateOverTime` 以固定速率朝目标移动并在跨步时精确落到目标，支持向上与向下。两者在 Particle Spawn 写出显式初始值，避免依赖可被其它模块覆盖的列默认值。
- `MaintainA_SetDistanceBetweenPoints` 把位置投影到目标点周围的球面，再用 `Influence` 在原始位置与约束位置间插值。`PendulumSetup` 写入 pivot、rest axis、length 与 spring 参数；`PendulumConstraint` 在启用角度约束时把自由方向限制在 rest cone 内；可选 spring driver 用显式 `dt × max(tightness, 0)` 冲量把速度拉回 rigid endpoint。`PointForce` 沿从力源向粒子的径向施加力，可选择衰减距离并反转方向，复用统一 force accumulator。
- `InheritSourceMovement` 在 Particle Spawn 用 owner source velocity 写入初始运动并带上 scale；当前只实现该 spawn 语义，尚未提供 UE 版本中的 falloff curve、速度上限和 update-stage 组合选项，不依赖新的 data interface。
- 新增 1 个行为用例（`Tests/Unit/VfxAuthoringTests.cpp`）：`EleventhBatchStateConstraintsAndPointForceUseSharedIr` 断言两种标量状态在 Spawn 的初值、Update 的收敛/不越界/目标吸附与下限钳制，距离约束保持 2 单位长度，单摆按 pivot 的 2 单位半径投影并保留 rest axis，45° 角度约束把粒子限制在 rest cone 内，点力经统一求解器产生加速度，source movement 用 scale 写入初速度（不含 falloff/速度上限），以及 `RibbonWidth → ScaleRibbonWidth` 的 0.25×0.5=0.125 链路。
- 本轮验证：Authoring 全量 **55/55**（`build/vfx-eleventh-batch-authoring.xml`）；VFX 非像素回归 **55/55**（`build/vfx-eleventh-batch-vfx-nongpu.xml`）；DX12/Vulkan Ribbon 像素回归 **2/2**（`build/vfx-eleventh-batch-ribbon-pixels.xml`）。尚未在本轮补 GPU 设备像素或性能对照，新增模块的 CPU/共享 IR 行为已覆盖，GPU 共享编译路径由 VFX shader/设备测试继续覆盖。
- 下一批仍以无需新渲染器/数据接口的库可见模块为先：`CalculateLinkConstraint`、`BeamEmitterSetup`、`SpriteBasedLine`、`Generate*Event` 的剩余生成侧语义，以及数组/音频/碰撞数据接口仍留作较大的独立工作。`NiagaraParity.json` 的 276 个 Module 条目仍全部是 pending；本页的 158 个入口只是库可见非废弃子集，不得把这次扩充表述为 Niagara 全量验收。

## 2026-10-18 模块库第十二批（事件生成策略、Beam Setup、Particle State、Sprite Line）

- 模块库从 103 个扩到 **109** 个，按冻结清单库可见非废弃目标的保守名称匹配口径从 81/158 提升到 **87/158（55.1%）**。新增 `GenerateDeathEvent`、`GenerateLocationEvent`、`GenerateCollisionEvent`、`BeamEmitterSetup`、`ParticleState`、`SpriteBasedLine`。
- 建立独立的 **事件发送策略**：`EmitterAsset` 现在保存 `eventGenerationMask` 与 `hasEventGenerationPolicy`。未声明 Generate 模块的旧资产继续默认发布三类事件；一旦声明 Generate 模块，就只启用对应通道，显式关闭时发布位为 0。CPU 发射器在 Spawn、Death、Collision 三个实际发布点消费 mask，不再只是把设置存在资产里。
- `BeamEmitterSetup` 写入 beam 起止点、起止切线、切线权重、Ribbon link order 与宽度；起终点使用切线权重混合，CPU/GPU 共享同一条 IR。现有 Ribbon shader 继续消费位置、link order 与宽度，没有改变 Shader 布局版本或运行时包 ABI。
- `ParticleState` 把当前 age/lifetime 归一化到 `Particles.NormalizedAge`，支持循环时回卷到段内，并以 lowering lifetime 的方式接入现有死亡压缩路径。`SpriteBasedLine` 写入线段中心、段长尺寸和端点位移方向，沿用当前 velocity-aligned Sprite 拉伸路径。
- 事件、Beam 与状态的序列化往返已覆盖；Generate mask 的非法 bit 会被拒绝。一次独立的运行期事件回归继续证明接收器在 source burst 后按死亡事件生成粒子。
- 新增 2 个行为用例（`Tests/Unit/VfxAuthoringTests.cpp`）：`GenerateModulesSelectPublishChannelsAndBeamStateFeedsTheRibbon` 断言启用/停用 Generate 后的 mask、round-trip、Beam 端点/宽度/link order 与材质无关的 Ribbon 数据链；`ParticleStateAndSpriteBasedLinePublishSharedColumns` 断言 loop/kill、NormalizedAge、线段中心和段长尺寸。
- 本轮验证：Authoring 全量 **57/57**（`build/vfx-twelfth-batch-authoring.xml`）；焦点行为回归 **3/3**（`build/vfx-twelfth-batch-focused.xml`）；VFX 非像素回归 **55/55**（`build/vfx-twelfth-batch-vfx-nongpu.xml`）；DX12/Vulkan Ribbon 像素回归 **2/2**（`build/vfx-twelfth-batch-ribbon-pixels.xml`）。未新增这三个模块的专用 GPU 像素或性能对照；共享 IR/HLSL 编译路径已随完整 VFX shader 与设备套件通过。
- **如实保留的边界**：`SpriteBasedLine` 当前只能写入标量 `Particles.Size`，由 velocity-aligned Sprite 沿速度方向拉伸；单独的横向宽度和自定义 alignment 通道仍需要 Two-axis size/alignment ABI。`ParticleState` 的 emitter-deactivated death 与无限寿命选项尚未实现；Generate 模块的 probability、event payload override 与 location-event rate/movement tolerance 仍待后续批次。

## 2026-10-18 模块库第十三批（Collision 策略、属性插值与事件发布点验证）

- 模块库从 109 个扩到 **112** 个，按冻结清单库可见非废弃目标的保守名称匹配口径从 87/158 提升到 **90/158（57.0%）**。新增 `Collision`、`CollisionQueryAndResponse`、`LerpParticleAttributes`。
- `Collision` 与 `CollisionQueryAndResponse` 做成**发射器级策略**而不是每粒子指令：编译期把启用状态、粒子半径、回弹系数、摩擦与碰撞层写进 `EmitterAsset`，运行期沿用已经存在的 PhysX 射线查询与速度反射路径。非法半径/回弹/摩擦以及非字面量输入都会被诊断拒绝。
- `LerpParticleAttributes` 在共享 IR 中按夹取到 [0,1] 的 Alpha 混合位置、速度与颜色，CPU 与 GPU 走同一表达式。
- **修掉一个事件侧的结构性缺口**：`Generate*Event` 的通道策略此前只落在资产字段上；现在补了发布点验证，确认 mask 为 0 时粒子死亡不再产生 death 事件，只开启 death 位时事件恢复，证明策略在真实发布路径上生效。
- 新增 2 个行为用例：`CollisionModuleConfiguresTheEmitterAndLerpBlendsAttributes`（`Tests/Unit/VfxAuthoringTests.cpp`）断言碰撞字段下发、序列化往返、非法值拒绝、禁用路径与 Lerp 混合结果；`EventGenerationMaskSuppressesDeathEventsAtThePublishSite`（`Tests/Unit/VfxRuntimeTests.cpp`）断言事件发布点的抑制与恢复。
- 本轮验证：Authoring 全量 **58/58**（`build/vfx-thirteenth-batch-authoring.xml`）；VFX 非像素回归 **56/56**（`build/vfx-thirteenth-batch-vfx-nongpu.xml`）；聚焦回归 **4/4**（`build/vfx-thirteenth-batch-focused.xml`、`build/vfx-thirteenth-runtime-focused.xml`）。DX12/Vulkan Ribbon 像素用例本轮未重跑，上一轮结果仍为 2/2。
- **如实保留的边界**：`Collision` 目前只覆盖发射器级球半径查询与反射，未实现 Niagara 的分析平面、GPU 深度/距离场/RayTracing 查询、rest state、最大碰撞次数与穿透修正；`CollisionQueryAndResponse` 仅提供开关，响应参数复用发射器设置。`AerodynamicDrag` 与 `CurlNoiseForce` 因依赖朝向曲线、枢轴偏移、升力通道与噪声场，本轮未实现，不做近似替代。

## 2026-10-18 模块库第十四批（转动冲量与初始力转换）

- 模块库从 112 个扩到 **114** 个，按冻结清单库可见非废弃目标的保守名称匹配口径从 90/158 提升到 **92/158（58.2%）**。新增 `MeshRotationForce` 与 `ApplyInitialForces`。
- 在编译器里新增独立的 **转动冲量通道**（参考实现的 `Transient.PhysicsRotationalForce` 对应物）。`MeshRotationForce` 按 `Rotation × Lever Radius` 累积该通道；`ApplyInitialForces` 在同一个 shared graph 里把它按质量转成 `Particles.RotationalVelocity`，并在消费后清零，保证随后的 `SolveForcesAndVelocity` 不会重复施加同一份冲量。
- `ApplyInitialForces` 同时支持把线性累积力转成速度、以及按 `Positional Force Warmup Time` 的一半时间把力预算成初始位置偏移；未提供旋转/线性输入时不会凭空写出 `Particles.RotationalVelocity` 列，旧资产布局保持不变。
- 新增 1 个行为用例（`Tests/Unit/VfxAuthoringTests.cpp`）：`MeshRotationForceConvertsIntoRotationalVelocityThroughApplyInitialForces` 断言转角与力臂相乘得到的角速度（torque 3 / mass 3 = 1）、下一 tick 重新累积后的增量、以及位置预热偏移 4×0.25×0.5。
- 本轮验证：Authoring 全量 **59/59**（`build/vfx-fourteenth-batch-authoring.xml`）；VFX 非像素回归 **56/56**（`build/vfx-fourteenth-batch-vfx-nongpu.xml`）；聚焦回归 **1/1**（`build/vfx-fourteenth-focused.xml`）。DX12/Vulkan 像素用例本轮未重跑。
- **如实保留的边界**：`ApplyInitialForces` 未实现 Niagara 的“把速度写回本征属性后清零 Transient 通道”之外的力通道语义（如 AdvectionVelocity、PhysicsDrag）、旋转阻尼与外力注入；`MeshRotationForce` 只支持旋转向量乘力臂，未实现按 MeshOrientation 的轴对称力矩分解。`GridLocation`/`PartitionParticles` 依赖尚未提供的“发射序号/执行索引”通道，本轮不做随机数近似替代。

## 2026-10-19 模块库第十五批（发射序号通道与网格放置）

- 模块库从 114 个扩到 **116** 个，按冻结清单库可见非废弃目标的保守名称匹配口径从 92/158 提升到 **94/158（59.5%）**。新增 `GridLocation` 与 `SpawnParticlesInGrid`。
- **补上被多个模块共同依赖的基础能力：发射序号通道**。IR 的 `Operation` 末尾追加 `SpawnIndex`（追加在末尾，既有 opcode 编号与已序列化程序不变），`ExecutionContext` 增加 `spawnIndex`，CPU 的 `CpuEmitter::Spawn`/`ReceiveEvents` 逐粒子传入出生序号，GPU 侧由 `SpawnParticles` 的 `birth` 直接传入 `VfxEvaluate`。`SpawnIndex` 只在 ParticleSpawn 阶段合法，其它阶段会被校验拒绝。
- `GridLocation` 用整数整除/取余把序号解码成 X-major 的 `(x,y,z)` 单元（浮点除法会让余数渗入上层轴，这是我第一版实现的真实缺陷，测试抓出后改为 Int32 运算），再乘以 `XYZ Dimensions` 与可选 cell jitter 得到最终位置。
- `SpawnParticlesInGrid` 写出 `Particles.GridCellIndex` 与 `Particles.GridCellAge`，并把配置的 age 写入 `Particles.Age`，由常规更新路径推进。
- 新增 1 个行为用例（`Tests/Unit/VfxAuthoringTests.cpp`）：`SpawnIndexDrivesExactGridCellsOnCpuAndGpu` 断言 3×2×1 网格下 6 个粒子的精确位置 `(n%3, n/3, 0)`、同一程序能编译出含 `SpawnParticles` 的 GPU spawn shader、以及网格单元序号/年龄列。
- 本轮验证：Authoring 全量 **60/60**（`build/vfx-fifteenth-batch-authoring.xml`）；VFX 非像素回归 **56/56**（`build/vfx-fifteenth-batch-vfx-nongpu.xml`，其中 shader 套件会实际编译含 `SpawnParticles` 的入口）；聚焦回归 **1/1**（`build/vfx-fifteenth-focused.xml`）。DX12/Vulkan 像素用例本轮未重跑。
- **如实保留的边界**：`SpawnIndex` 按**批内序号**定义，未实现 Niagara 的持久 `Particles.ID`/跨帧执行索引，也未实现 `SpawnParticlesInGrid` 的 Spawn Time/Spawn Group 调度与 GridLocation 的 Coordinate Space、Override Random Seed、Placement Randomness Mode。`PartitionParticles` 依赖 IDA 与空间分区统计，仍未实现。
## 2026-10-19 模块库第十六批（持久发射序号与粒子分区）

- 模块库从 116 个扩到 **117** 个，按冻结清单库可见非废弃目标的保守名称匹配口径从 94/158 提升到 **95/158（60.1%）**，本方案覆盖率首次超过 60%。新增 `PartitionParticles`。
- 把上一批的 `SpawnIndex` 从“仅 spawn 阶段可用”升级为**持久粒子列**：当 Particle Update 阶段出现 `PartitionParticles` 时，编译器会自动在 spawn 图里补写 `Particles.SpawnIndex`（若 `SpawnParticlesInGrid` 已经写过同一列则跳过，因为属性只允许一个写入者），于是分区结果基于出生序号而不是每帧随机值。
- `PartitionParticles` 用 Int32 整除/取余计算 `Particles.Partition` 与 `Particles.PartitionNormalized`；序号在 `Number Of Partitions × Particles Per Partition` 空间内回绕，因此批次大于容量时复用分区而不是越界。
- 新增 1 个行为用例（`Tests/Unit/VfxAuthoringTests.cpp`）：`PartitionParticlesAssignsStableEvenlySpacedPartitions` 断言 8 个粒子在“4 per partition”下得到 `{0,0,0,0,1,1,1,1}` 与归一化 `{0,¼,½,¾,0,¼,½,¾}`，编译器确实合成了 spawn 阶段的 ordinal 写入，并断言第二个 tick 不会重排分区。
- 本轮验证：Authoring 全量 **61/61**（`build/vfx-sixteenth-batch-authoring.xml`）；VFX 非像素回归 **56/56**（`build/vfx-sixteenth-batch-vfx-nongpu.xml`）；聚焦回归 **1/1**（`build/vfx-sixteenth-focused.xml`）。DX12/Vulkan 像素用例本轮未重跑。
- **如实保留的边界**：`Particles.SpawnIndex` 仍是**批内序号**，不是 Niagara 的持久 `Particles.ID`；因此 `PartitionParticles` 的 IDA 模式、Distance/Color/Direction 分区模式、Partition Duration 与 Minimum Saturation 都未实现，当前只覆盖“按序号均匀分区”这一种参考模式。

## 2026-10-19 碰撞结果落到粒子列

- 模块库条目数不变（**117**，保守覆盖 95/158），本批补的是碰撞链路的一个结构性缺口：此前 `Collision` 只把速度与位置改掉，**碰撞法线与碰撞状态没有任何粒子级输出**，因此依赖"上一个碰撞平面"的模块（`AlignParticlesWithCollisionPlane`）与用户自定义碰撞响应都无法实现。
- `CpuEmitter` 新增两个可选列 `Particles.CollisionNormal`（Float3）与 `Particles.Colliding`（Float）。两列都由 `Collision` 模块在创作期声明，因此未使用碰撞的发射器属性布局与程序标识完全不变——这也是设计约束：属性列只允许单一写入者，不能无条件注入。
- 命中时写入接触法线与 `1`；**未命中时把两列清零**，避免上一帧的法线被误当成当前接触。这一点在实现与测试里都被显式覆盖：碰撞后的下一个子步粒子已离开表面，状态必须回到 0。
- 新增 1 个运行时用例（`Tests/Unit/VfxRuntimeTests.cpp`）：`CollisionResultsReachTheParticleColumnsAndClearOnMiss` 覆盖三种状态——用单步 `TickStep` 命中地板时法线为 `+Y` 且状态为 1；随后的子步失去接触时两列清零；始终未命中（向上运动、远离地板）时同样保持 0。
- 本轮验证：VFX 非像素回归 **57/57**（`build/vfx-seventeenth-batch-vfx-nongpu.xml`，较上轮 +1 即本用例）；Authoring 全量 **61/61**（`build/vfx-seventeenth-batch-authoring.xml`）；聚焦回归 **1/1**（`build/vfx-seventeenth-focused.xml`）。DX12/Vulkan 像素用例本轮未重跑。
- **如实保留的边界**：`AlignParticlesWithCollisionPlane` 本身仍未实现，因为它还需要每粒子 SpriteFacing/Alignment 自定义向量通道，而当前 Sprite ABI 只有渲染器级 alignment 枚举；`Collision` 的 rest state、最大碰撞次数、穿透修正、分析平面与 GPU 端深度/距离场查询同样未实现。碰撞结果目前只有 CPU 路径产出，GPU 端尚无回读。
## 2026-10-18 Spawn Per Unit

- 新增 `SpawnPerUnit`：按所属系统行进的距离比例生成粒子。沿用 `SpawnPerFrame` 的精确路径——`EmitterAsset` 增加独立的 `spawnPerUnit` 密度字段，`CpuEmitter::Step` 用 `System.Velocity` 与该步时长算出位移后累积分数，跨步保留余数，因此慢速移动不会丢粒子。序列化两端接通并校验 0–100000 的上限。
- 该模块同时把 `System.Velocity` 纳入保留参数声明，与第十批的 System 变换上下文复用同一次发布。
- 新增 1 个行为用例（`Tests/Unit/VfxAuthoringTests.cpp`）：`SpawnPerUnitEmitsInProportionToTravelledDistance` 断言 10 单位/秒 × 0.1 秒（1 单位）在密度 2 下产出 2 个粒子、两个 0.025 秒步累积出第 3 个（证明分数被保留而不是丢弃）、系统速度归零后不再生成。
- 本轮验证：行为/授权套件 **53/53**（`build/vfx-spawnunit-authoring.xml`）、VFX 套件 **55/55**（`build/vfx-spawnunit-vfx.xml`）。
- 覆盖率：库可见非废弃目标 158 个中已实现 **88** 个（55.7%）。
## 2026-10-18 模块库第十批（系统变换、系统状态、Complete If Unused）

- 模块库从 83 个扩到 **87 个**：新增 `SystemLocation`、`InheritSystemVelocity`、`SystemState`、`CompleteIfUnused`。
- 建立 **System 变换上下文**：`SystemInstance::SetSystemTransform` 发布所属组件的位置与速度，经参数表同时供 CPU 与 GPU 读取（与相机、Engine 上下文同一机制）。`SystemLocation` 把系统位置与模块偏移加到出生位置，`InheritSystemVelocity` 按比例继承系统速度，`SystemState` 写出系统年龄与按持续时间的归一化值。
- 新增**系统级完成语义**：`SystemCompletionSettings`（enabled + unusedGracePeriod）随系统序列化并升到源格式 v5；运行时每步统计"所有发射器均为空"的持续时长，达到宽限期后 `SystemInstance::IsComplete()` 置位，`Reset()` 清除。此前 `CpuEmitter::IsComplete()` 存在但没有任何消费方——这是它缺失的系统级对应物。
- 语义实现要点：`SystemState` 的归一化年龄夹取到 [0,1]，超过持续时间后饱和而不是越界；`CompleteIfUnused` 只接受字面量宽限期并校验 0–3600 秒。
- 调试过程记录（均为我自己的假设错误，逐个查清后修正）：`CompleteIfUnused` 的测试最初直接写 `emitter.settings.bursts`，但 EmitterUpdate 栈会在降级时重建 bursts 与 spawnRate，必须改用 `SpawnBurst`/清 `SpawnRate` 模块；寿命设成 0.05s 时 0.01s 的 tick 已把它判死，改为 0.5s 并延长观测步数；事件接收用例同样依赖 `settings.bursts`（bursts 被重建为空、spawnRate 被模块加回），改为显式模块配置后恢复通过。
- 新增 2 个行为用例（`Tests/Unit/VfxAuthoringTests.cpp`）：`TenthBatchSystemTransformLocationVelocityAndState` 断言系统位置 (10,20,30) 加偏移 1 得到 (11,20,30)、一半的继承速度得到 2、系统年龄 1/时长 4 归一化为 0.25 且 9 秒时饱和到 1；`CompleteIfUnusedCompletesTheSystemAfterTheGracePeriod` 断言粒子存活期间不完成、清空并越过宽限期后完成、`Reset()` 清除标志。
- 本轮验证：行为/授权套件 **52/52**（`build/vfx-batch10-authoring.xml`）、VFX 套件 **55/55**（`build/vfx-batch10-vfx.xml`）。
- 覆盖率：库可见非废弃目标 158 个中已实现 **87** 个（55.1%）。剩余 71 个中约 35 个属于 Ribbon/Light/Decal/Component/Volume 渲染器相关，约 30 个属于 Audio/Texture/GBuffer/Neighbor Grid/Skeletal Mesh/Landscape/Array 等新数据接口，其余为 `Timeline` 的播放/倒放边界、`FindScreenSpaceBoundingBox`、`RecreateCameraProjection`、`SpawnPerUnit`（需系统位置历史）等。
## 2026-10-18 模块库第九批（首帧、帧计数、时间轴、状态机）

- 模块库从 79 个扩到 **83 个**：新增 `FirstFrame`、`EmitterFrameCounter`、`Timeline`、`TimeBasedStateMachine`。
- 建立 **Engine 上下文发布**：`SystemInstance` 在每步之前把 `Engine.FirstFrame`（首次模拟步为 1）、`Engine.SystemAge`、`Engine.FrameIndex` 写进参数表，与相机一致地同时供 CPU 与 GPU 读取；降级末段的参数裁剪同步放行 `Engine.` 前缀。
- 语义实现要点：`FirstFrame` 由 `Engine.FirstFrame` 比较得出；`EmitterFrameCounter` 用独立的 `Particles.EmitterFrameCounter` 列逐次自增；`Timeline` 输出驱动后的原始时间（回放时可为负）、`clamp(raw/length)` 的归一化值与前向标志；`TimeBasedStateMachine` 用整数转换取整得到循环内时间，再按 `On Duration` 判定输出。
- 过程中修掉三处真实问题：`FirstFrame`/`Timeline` 初版写 Bool 属性，触发"Attribute output requires an explicit type conversion"（IR 的类型规则要求 Bool 输出必须来自 Bool 输入），改为写 Float 标志；`EmitterFrameCounter` 的属性名在模块定义与降级实现里不一致（`Emitter.FrameCounter` 对 `Particles.EmitterFrameCounter`），导致"Invalid particle attribute schema"；`TimeBasedStateMachine` 初版用乘除模拟取整，对 0.125 这类周期位置得到错误结果，改用显式整数转换后正确。
- 另外修正了 `TimeBasedStateMachine` 的状态极性（参考实现是 on 窗口内为高值）与 `Timeline` 的原始位置语义（回放时为负，而不是取模后的正值）。
- 新增 1 个行为用例（`Tests/Unit/VfxAuthoringTests.cpp`）：`NinthBatchEngineContextFirstFrameCounterTimelineAndStateMachine` 断言首帧标志随 `Engine.SystemAge` 从 1 变 0、帧计数连续三次自增到 3、时间轴在半秒/一秒长度下归一化为 0.5 且正向时报告播放、回放时原始位置为 -0.5、状态机在 on/off 窗口分别输出 1/0/1。
- 本轮验证：行为/授权套件 **50/50**（`build/vfx-batch9-authoring.xml`）、VFX 套件 **55/55**（`build/vfx-batch9-vfx.xml`）。
- 覆盖率：库可见非废弃目标 158 个中已实现 **83** 个（52.5%）。
## 2026-10-18 模块库第八批（三角形距离、质量推导尺寸/转动惯量、旋转速度、动量守恒）

- 模块库从 75 个扩到 **79 个**：新增 `FindClosestPointOnTriangle`、`CalculateSizeAndRotationalInertiaByMass`、`AddRotationalVelocity`、`UpdateVelocityOnMassChange`。
- 语义实现要点：`FindClosestPointOnTriangle` 取三角形法线与粒子到顶点的偏移做点积，得到无符号平面距离（对面向三角面的常见情形即最近点距离）；`CalculateSizeAndRotationalInertiaByMass` 由 `size = cbrt(mass/density)` 反推尺寸并写回 `Particles.Size`，再按实心立方体主轴公式 `m·s²/6` 写转动惯量；`AddRotationalVelocity` 把角度制输入转成弧度累加到 `Particles.RotationalVelocity`；`UpdateVelocityOnMassChange` 按 `v·(m_old/m_new)` 缩放速度以保持动量，比值夹取到 [1e-6, 100] 防止退化。
- 新增 1 个行为用例（`Tests/Unit/VfxAuthoringTests.cpp`）：`EighthBatchTriangleDistanceMassInertiaRotationAndMomentum` 断言 XY 平面上方 z=3 与下方 z=-1.5 的距离分别为 3 与 1.5（有符号性）、质量 1/密度 1000 得尺寸 0.1 与惯量 0.01/6、90 度每秒转成 π/2、质量翻倍时速度从 4 降到 2。
- 本轮验证：行为/授权套件 **49/49**（`build/vfx-batch8-authoring.xml`）、VFX 套件 **55/55**（`build/vfx-batch8-vfx.xml`）。
- 覆盖率：库可见非废弃目标 158 个中已实现 **79** 个（50.0%）。这是本方案首次过半。剩余 79 个仍集中在需要新渲染器（Ribbon/Light/Decal/Component/Volume 及其专属模块约 35 个）、新数据接口（Audio/Texture/GBuffer/Neighbor Grid/Skeletal Mesh/Landscape/Array 分区约 30 个）与剩余 System 上下文（SpawnPerUnit、Timeline、RecreateCameraProjection、屏幕空间包围盒等约 14 个）。
## 2026-10-18 View Recycler

- 新增 `ViewRecycler`：把落在相机背后的粒子沿相机轴镜像回视锥前方，使粒子总数在不开新生成的前提下保持稳定。判定用相机前向轴上的深度，镜像关于 `Recycle Margin` 平面进行，边缘情形用 `max(margin, 1e-4)` 兜底避免除零与符号翻转。
- 新增 1 个行为用例（`Tests/Unit/VfxAuthoringTests.cpp`）：`ViewRecyclerMovesOffscreenParticlesBackIntoView` 断言视内粒子（深度 +10）不被扰动、相机后方粒子（深度 -5）被回收到前方，且关于 0.5 的边距平面镜像后精确落在 +6。
- 首轮断言把镜像结果写成 0.5（边距平面本身），实测得 6.0，属我自己的算术错误；镜像语义经复核无误后修正断言，保留了中间量 `> 0` 的检查。
- 本轮验证：行为/授权套件 **48/48**（`build/vfx-viewrecycler-authoring.xml`）、VFX 套件 **55/55**（`build/vfx-viewrecycler-vfx.xml`）。
- 模块库现为 **75 个**（库可见非废弃目标 158 中的 47.5%）。相机类仍剩 `RecreateCameraProjection`（需要场景捕获投影语义）与 `FindScreenSpaceBoundingBox`（需要屏幕空间包围盒输出通道，属新的输出概念）；`SpawnPerUnit` 需要 System 位置历史。
## 2026-10-18 相机上下文接通 GPU 与坐标空间修正

- 打通 **GPU 侧相机模块**：相机快照此前只发布给 CPU 发射器，GPU 发射器读到的仍是声明时的中性默认值。现在 `SystemInstance` 把保留参数写进 `m_parameters`，而世界帧的 `frame->parameters` 正是渲染代理喂给 `GpuEmitter::SetParameters` 的来源，因此 CPU 与 GPU 共用同一次发布，无需第二条通路。发布时只写入程序已声明的名字，避免 GPU 参数绑定拒绝整帧。
- **修正了一个会让 GPU 相机完全失效的既有缺陷**：降级末段的 `if (!typed) lowered.parameters.clear();`（本意是让纯 Float 系统保持旧程序身份）会把刚注入的 `Camera.*` 一起清掉，导致相机模块在 GPU 系统里"能编译但参数未声明"。改为只清除非 `Camera.` 前缀的参数，保留携带逐帧实时值的保留参数。
- 这个缺陷是被新测试直接抓到的：`CameraRelativeModulesUseThePublishedSnapshot`（CPU 侧、手工填参数表）一直通过，说明它**掩盖**了参数声明缺失；于是补写了 `CameraSnapshotTravelsWithTheWorldFrameForGpuEmitters`，它既检查 `compiled.asset.parameters` 里确实声明了 `Camera.Position/Forward/FovDegrees/ViewportWidth`，又检查世界帧携带的数值，还验证未发布快照时保持声明默认值而不会凭空造一个相机。
- 本轮验证：行为/授权套件 **47/47**（`build/vfx-camera-gpu-authoring.xml`）、VFX 套件 **55/55**（`build/vfx-camera-gpu-vfx.xml`）。
- 仍未实现：`RecreateCameraProjection`（场景捕获投影重建）、`ViewRecycler`、`FindScreenSpaceBoundingBox`、`SpawnPerUnit`（需要 System 位置历史）。相机模块目前只覆盖 CPU 与 GPU 的粒子模拟路径，尚未接入编辑器预览的相机发布。
## 2026-10-18 相机相对模块（模拟侧相机上下文）

- 建立**模拟侧相机上下文**：`CameraContext`（位置、右/上/前基向量、FOV、视口尺寸）由宿主通过 `SystemInstance::SetCameraContext` 在 tick 前发布，`CpuEmitter::SetCameraContext` 把它翻译成保留的 `Camera.*` 参数。未发布时保持 invalid，不做静默回退。
- 接入方式特意选择**复用现有参数机制**而不是新增 IR 上下文节点：新增 `Operation` 会改动序列化的操作码枚举、程序格式版本与跨平台工件校验，代价远大于收益。保留参数只在栈里真的出现相机模块时才注入，因此不使用相机的系统程序身份与参数集完全不变（已用既有 46 项行为测试确认无回归）。
- 新增 3 个相机模块：`CameraOffset`（沿粒子到相机的向量反向偏移）、`MaintainInCameraParticleScale`（按相机相对深度与 FOV 正切补偿尺寸，保持视觉大小）、`ViewMask`（把位置投到相机坐标系，写出到视口边缘的渐隐遮罩 `Particles.ViewMask` 与是否在视内的 `Particles.InsideView`）。
- 修复了一个既有的越界缺陷：降级后的参数校验原本只在 `source.parameters` 里查找 `Operation::Parameter` 节点，注入保留参数后会走 `parameter == end()` 分支并让 `parameter-result...begin()` 的指针减法越界（`vector subscript out of range` 断言）。改为在降级后的参数集合中查找。
- 新增 1 个行为用例（`Tests/Unit/VfxAuthoringTests.cpp`）：`CameraRelativeModulesUseThePublishedSnapshot` 断言相机在 +Z、粒子在原点时 0.5 缩放的相机偏移把粒子移到 Z=-1；轴上粒子 ViewMask=1 且 InsideView=1，侧向 100 单位处两者都归零；`MaintainInCameraParticleScale` 在参考距离/参考 FOV 下缩放恰为 1，距离减半时缩放变为 0.5。
- 本轮验证：行为/授权套件 **46/46**（`build/vfx-camera-authoring.xml`）、VFX 套件 **55/55**（`build/vfx-camera-vfx.xml`）。
- 模块库现为 **74 个**（库可见非废弃目标 158 中的 46.8%）。相机上下文已建立，但 `RecreateCameraProjection`（场景捕获投影）、`ViewRecycler`、`FindScreenSpaceBoundingBox`、`SpawnPerUnit`（需要 System 位置历史）仍待实现；GPU 侧相机模块同样未接入，当前只在 CPU 模拟路径生效。
## 2026-10-18 Spawn Per Frame（精确逐子步生成）

- 补上 `SpawnPerFrame` 模块。参考语义是"每帧生成固定数量"，与 `SpawnRate` 的"按时间累积速率"不同，因此没有用 `rate = Count*60` 近似，而是给 `EmitterAsset` 增加独立的 `spawnPerFrame` 字段、由 `CpuEmitter::Step` 在每个子步精确追加该数量。
- `EmissionScheduler` 增加预留 ID 接口（`NextId()` / `ConsumeNextId()`），使逐子步生成与调度生成的粒子 ID 在同一游标下保持唯一，容量丢弃时同样消耗 ID，与既有契约一致。
- 序列化两端接通并校验上限（每帧不超过 100000）；EmitterUpdate 降级把字面量 `Count` 累加进发射器设置。
- 新增 1 个行为用例（`Tests/Unit/VfxAuthoringTests.cpp`）：`SpawnPerFrameEmitsAnExactCountEverySubStep` 断言子步时长 0.01/0.05/0.2 下每次都精确增加 3 个（证明不受 delta 影响）、发射器停止后不再生成。
- 本轮验证：行为/授权套件 **45/45**（`build/vfx-spawnframe-authoring.xml`）、VFX 套件 **55/55**（`build/vfx-spawnframe-vfx.xml`）。
- 模块库现为 **71 个**（含事件批次的 3 个 Receive 模块；库可见非废弃目标 158 中的 44.9%）。仍未实现 `SpawnPerUnit`：它需要 System 级位置历史与跨帧位移，而当前 IR 没有 System 上下文节点，属于与相机/视口同类的底层缺口。
## 2026-10-18 事件处理器（Generate/Receive 链路接通）

- 补上 **Stage::Event 的执行链路**。此前只有事件“采集”（CPU 发射器把 Spawn/Death/Collision 记入 `m_events`），没有任何消费方，编译器对 Event 栈直接报 "This execution context is not implemented yet"。现在事件发射器可以在另一个发射器上生成粒子：
  - `EmitterAsset` 新增 `eventReceiver`（源发射器 ID、事件种类、每次事件的生成数、随机初龄），随系统序列化，解码时校验种类/数量/初龄范围。
  - `CpuEmitter::ReceiveEvents` 在每个事件位置生成粒子并写入事件载荷位置，随机初龄用稳定粒子 ID 派生，生成 ID 走高位段（`0x8000...`）以免与调度器 ID 冲突。
  - `SystemInstance::DispatchEvents` 在**每个子步后**按发射器顺序派发，派发后立即消费事件，保证每个事件恰好投递一次、且接收方在同一步内响应。
  - 新增 3 个 Event 阶段模块：`ReceiveDeathEvent`、`ReceiveCollisionEvent`、`ReceiveLocationEvent`（模块库从 67 增至 70），指令输入为字面量，编译期校验生成数与初龄并写入接收配置。
- 为此把 `SystemInstance` 从"整帧批量 TickSteps"改为"逐子步 TickStep + 逐步派发"。这修正了一个真实缺陷：原实现只在整帧之后派发一次，导致同一事件在多个子步间被重复投递（实测接收数是死亡数的 2 倍），且跨发射器事件延迟到帧末。
- 新增 2 个行为用例（`Tests/Unit/VfxAuthoringTests.cpp`）：`EventReceiverSpawnsFromAnotherEmittersPublishedEvents` 断言无调度的接收方在事件产生前保持 0、单子步内"接收增量 == 死亡增量"、接收粒子位置有限且数量等于累计生成数；`EventReceiverValidationRejectsUnsupportedInputs` 断言非整数/超范围生成数与越界初龄被拒绝并给出诊断，合法配置（2 个/事件、初龄 0.1）能编译。
- 调试过程记录：测试最初失败暴露了四个我自己的假设错误（用 `CreateSimulationStage` 造 Event 栈导致 iterations=0 被校验拒绝、`CreateEmitter` 的 Spawn Rate 模块重新加回调度、`InitializeParticle` 模块写死 Lifetime 覆盖属性默认值、用跨多子步的累计量做对比）。每一个都查清了真实机制再修，没有靠放宽断言通过。
- 本轮验证：行为/授权套件 **25/25**、VFX 套件 **55/55**（`build/vfx-events-vfx-suite.xml`），DX12 调试层与 Vulkan 验证层开启。
- 当前状态：事件链路已打通"发射器 → 同系统另一发射器"的生成路径。尚未实现的是事件载荷的自定义字段覆盖（参考实现允许逐字段改写位置/速度/颜色等）、事件属性读写的 Receive 模块变体、GPU 事件的异步回读、以及跨 System 的事件总线。这些仍记录为缺口。
## 2026-10-18 模块库第七批扩充（派生速度、继承速度、Owner 缩放、噪声输出、浮点弹簧）

- 模块库从 62 个扩到 **67 个**：新增 `CalculateAccurateVelocity`、`InheritVelocity`、`ApplyOwnerScaleToAttributes`、`VectorNoiseValue`、`SolveFloatSpringConstraint`。
- 语义实现要点：`CalculateAccurateVelocity` 用 `(position - previousPosition)/dt` 反推速度并在同一步把 `Particles.PreviousPosition` 推进到当前位置，因此约束类模块改写位置后速度能自动跟上；`ApplyOwnerScaleToAttributes` 逐轴缩放位置、尺寸取最大轴以免非均匀缩放下精灵变窄；`VectorNoiseValue` 把稳定随机向量叠加偏移后按强度缩放写入独立列，供后续模块复用；`SolveFloatSpringConstraint` 用半隐式欧拉迭代 `v += (k(rest-x) - c·v)dt; x += v·dt` 求解标量弹簧。
- 新增 1 个行为用例（`Tests/Unit/VfxAuthoringTests.cpp`）：`SeventhBatchDerivedVelocityOwnerScaleNoiseAndSpring` 断言 3 单位位移/0.5 秒得到 6 的速度且 previousPosition 前进、Owner 缩放 (2,3,4) 逐轴作用于位置且尺寸取 4、弹簧在 200 步内单调松弛并在 1400 步后收敛到 1e-2 以内。
- 弹簧用例首轮失败（残差 0.12 未达 1e-3），查明是收敛速度而非实现错误：k=10/c=2 在 dt=0.01 下需要更长的模拟时间，断言改为"先单调松弛、再收敛到 1e-2"，同时保留了中间量的检查。
- 本轮验证：行为/授权套件 **42/42**（`build/vfx-modules7-authoring-suite.xml`）。
- 当前覆盖率：库可见且非废弃的模块目标 158 个中已实现 **67** 个（42.4%）。剩余条目的共同点是依赖尚不存在的能力：Array 分区、Audio、Beam、Collision/PBD、Constraints、Distance Fields、Events、Mesh 骨骼遍历、Neighbor Grid、Skeletal Mesh、Texture/GBuffer/Depth 采样、Camera/Viewport/System 级上下文，以及 Ribbon/Light/Decal/Component/Volume 渲染器。要继续提高覆盖率，必须先补这些底层能力（尤其是相机上下文与 Ribbon/Light/Decal 渲染器），而不是继续堆叠纯属性模块。
## 2026-10-18 模块库第六批扩充（解析杀伤体积、一次性门、质量/能量、区域遮罩）

- 模块库从 57 个扩到 **62 个**：新增 `KillParticlesInVolume`、`DoOnce`、`CalculateMassAndRotationalInertiaByVolume`、`FindKineticAndPotentialEnergy`、`RegionMask`。
- 语义实现要点：`KillParticlesInVolume` 用球面判据加 Invert 标志做异或，命中时把 `Particles.Lifetime` 压到 `Age` 之下以复用既有压缩死亡；`DoOnce` 用 `Particles.DoOnce` 列作为已触发锁存位（0/1），后续模块用比较运算即可复现参考实现的 Execute 语义，而不引入无声明的新属性；`CalculateMassAndRotationalInertiaByVolume` 以粒子尺寸为棱长的立方体估算体积，质量=M³×密度、转动惯量=m·s²/6；`FindKineticAndPotentialEnergy` 写动能 ½mv² 与含势能的总能量；`RegionMask` 做轴对齐盒测试并写 0/1。
- 过程中修掉两个真实缺陷：`DoOnce` 初版写了未声明的 `Particles.DoOnceExecute`（编译期 `Unknown attribute`），改为复用已声明列；`RegionMask` 初版对 float3 直接做比较，触发"Comparison requires matching scalar numeric types"，改为逐分量比较后合并。
- 新增 1 个行为用例（`Tests/Unit/VfxAuthoringTests.cpp`）：`SixthBatchKillVolumeDoOnceMassAndRegionMask` 断言球内粒子死亡/球外存活、DoOnce 锁存从 0 到 1 且保持、无触发时锁存不变、质量与转动惯量按 27 体积×密度 2 得 54/81、动能 ½·2·25=25 且总能量 30、区域遮罩在三点采样的 1/0/0。
- 本轮验证：行为/授权套件 **41/41**（`build/vfx-modules6-authoring-suite.xml`）。
- 当前覆盖率：库可见且非废弃的模块目标 158 个中已实现 **62** 个（39.2%）。剩余未实现的主要是依赖新渲染器或数据接口的类别——Array（数组分区）、Audio、Beam、Collision、Constraints、Distance Fields、Events、Mesh 骨骼遍历、Neighbor Grid、Skeletal Mesh、Texture/GBuffer 采样，以及 Ribbon/Light/Decal/Component/Volume 渲染器相关模块。`Test-NiagaraParity.ps1` 仍返回未通过，默认编辑器保持旧版。

## 2026-10-18 模块库第五批扩充（力原语补齐）

- 模块库从 52 个扩到 **57 个**：新增 `LineAttractionForce`、`SpringForce`、`VectorNoiseForce`、`PointRepulsionForce`、`WindForce`，补上 Forces 类别里所有不需要新数据接口或噪声贴图就能表达的力原语。
- 语义实现要点：`LineAttractionForce` 先求线段上的最近点再按偏移乘强度累加；`SpringForce` 用 `k*(rest-position) - c*velocity` 计算弹簧加阻尼；`VectorNoiseForce` 从稳定粒子 ID 采样随机方向，因此同一粒子在整个生命周期内得到一致的力；`PointRepulsionForce` 按 `1 - distance/falloffRadius` 做距离衰减，超出半径则为零；`WindForce` 按 `scale*dt` 把速度向风速混合并夹取混合系数。
- 新增 1 个行为用例（`Tests/Unit/VfxAuthoringTests.cpp`）：`FifthBatchForcePrimitivesAccumulateAndSolve` 对每个力原语单独建栈并在 `SolveForcesAndVelocity` 后检查速度——线段引力在 (0,0,-2) 位置得到 +Z 上 8 的冲量、弹簧在静止点不产生力而偏离时回拉、点斥力在距离 2 / 半径 10 时得到 6×0.8=4.8、风场产生部分混合而非瞬间贴合。
- 首轮测试失败暴露两个我自己的输入选择错误（把粒子放在线段上导致力为零、把半径设得小于距离导致衰减为零），修正为有意义的采样点后数值与手算逐位吻合。这类失败说明断言确实在检验真实物理量，而不是恒真式。
- 本轮验证：行为/授权套件 **40/40**（`build/vfx-modules5-authoring-suite.xml`）；VFX 套件保持 **55/55**。
- 当前覆盖率：冻结清单中库可见且非废弃的模块目标为 **158** 个，已实现 **57** 个（36.1%）；含废弃/隐藏条目在内的总数仍为 276。Array、Events、Collision、Constraints、Mass、Camera、Scalability、Beam、Texture、Audio、Decal/Ribbon/Light/Component/Volume 渲染器，以及网格/骨骼/地形/邻域/音频数据接口尚未实现。`Test-NiagaraParity.ps1` 仍返回未通过，默认编辑器保持旧版。

## 2026-10-18 模块库第四批扩充（动态材质参数、颜色直写、旋转向量、锥体约束、线段分率）

- 模块库从 47 个扩到 **52 个**：新增 `DynamicMaterialParameters`、`Color`、`ApplyRotationVector`、`IncrementOverTime`、`ConstrainVectorToCone`、`FindClosestPointOnLineSegment`、`ConeMask` 等。
- 修掉第三个"渲染器已消费但无人能写"的结构性缺口：Sprite 着色器一直把 4 个 `Particles.DynamicMaterialParameter*`（float4）作为顶点插值器转发给材质，但模块库无法写入任何一路，因此候选编辑器做不出材质动态参数。`DynamicMaterialParameters` 现在暴露全部四路，且只在某一路携带非字面量绑定时才写该属性，避免无谓地扩张发射器属性表。
- 语义实现要点：`Color` 用标量 `Scale` 同时缩放 RGB 与 Alpha 并组装成 float4；`ApplyRotationVector` 把角速度向量转成增量四元数后左乘到 `Particles.MeshOrientation`，速度为零时用单位四元数避免产生 NaN；`ConstrainVectorToCone` 对超出锥角的向量按 `cos(limit)/cos(current)` 比例插回锥面并严格保持原速率；`FindClosestPointOnLineSegment` 写入沿线段的分率并夹取到 [0,1]；`IncrementOverTime` 按 delta 时间乘速率累积。
- 新增 2 个行为用例（`Tests/Unit/VfxAuthoringTests.cpp`）：`FourthBatchWritesDynamicMaterialParametersAndMathState` 断言四路动态参数列被注册、参数绑定值精确传入、增量计数跨帧累积（0.5s@3/s 得 1.5 再到 3.0）；`ConeConstraintAndLineSegmentFractionClampCorrectly` 断言锥内向量不被修改、锥外向量落在 45° 锥面上且保持原速率、线段分率在 0/0.25/1 三处正确夹取。
- 本轮验证：VFX 套件 **55/55**（`build/vfx-modules4-vfx-suite.xml`）、行为/授权套件 **39/39**（`build/vfx-modules4-authoring-suite.xml`）。
- 当前覆盖率：冻结清单 276 个 Modules 中实现 **52** 个；Array、Events、Collision、Constraints、Mass、Camera、Scalability、Beam、Material 的其余条目，以及 Ribbon/Light/Decal/Component/Volume 渲染器和网格/骨骼/地形/音频数据接口仍未实现。`Test-NiagaraParity.ps1` 仍返回未通过，默认编辑器保持旧版。

## 2026-10-18 模块库第三批扩充（Mesh 属性创作、四元数旋转、纯数学状态）

- 模块库从 38 个扩到 **47 个**：新增 `ScaleMeshSize`、`ScaleMeshSizeBySpeed`、`InitialMeshOrientation`、`UpdateMeshOrientation`、`MeshRotationRate`、`FrameCounter`、`TrackDistanceTraveled`、`ConeMask`。
- 修掉第二个"渲染器已消费但无人能写"的结构性缺口：Mesh 着色器一直读取 `Particles.Scale`（float3）与 `Particles.MeshOrientation`（xyzw 四元数），但模块库没有任何模块能写它们，所以候选编辑器做不出网格朝向与逐粒子尺寸。本批的 5 个 Mesh 模块通过模块属性声明自动引入这两列。
- 新增四元数能力：`QuaternionFromAxisAngle` 用半角正弦/余弦构造旋转，`QuaternionMultiply` 实现 Hamilton 积；`Component` 增加宽度参数，因为四元数取分量必须用 float4 基向量（此前硬编码 float3，导致 `Input vector dimensions differ`）。
- 语义实现要点：`MeshRotationRate`/`UpdateMeshOrientation` 以左乘增量四元数的方式累积自旋，保持单位长度；`FrameCounter` 在输入为真时逐帧自增、为假时保持不变；`TrackDistanceTraveled` 按速度乘 delta 时间累积；`ConeMask` 用方向点积与半角余弦比较，锥内写 1、锥外写 0。
- 有意未实现的模块：`InterpolateOverTime` / `FadeOverTime` 在参考实现里由用户指定要混合的目标属性，而当前类型化输入模型没有"属性目标绑定"能力，写死目标会歪曲语义，因此留空并在源码注释中说明原因，而不是伪造一个近似实现。
- 新增 2 个行为用例（`Tests/Unit/VfxAuthoringTests.cpp`）：`ThirdBatchWritesMeshRendererAttributesAndPureMathState` 断言 Mesh/计数器/距离属性被自动注册、绕 Z 轴 90° 得到 (0,0,sin45,cos45)、两次 update 后计数器为 2 且行进距离为 3、false 增量保持原值；`MeshOrientationCompositionRotatesAboutTheGivenAxis` 断言 90°/s 一秒得到四分之一转、结果保持单位长度、再一秒组合成半转（W≈0）。
- 本轮验证：VFX 套件 **55/55**（`build/vfx-modules3-vfx-suite.xml`）、行为/授权套件 **37/37**（`build/vfx-modules3-authoring-suite.xml`）。
- 当前覆盖率：冻结清单 276 个 Modules 中实现 47 个；Array/Math/Utility 的其余条目、Events、Collision、Constraints、Mass、Beam、Material、Audio、Ribbon/Light/Decal/Component/Volume 渲染器、网格与骨骼/地形/音频数据接口均未实现。`Test-NiagaraParity.ps1` 仍返回未通过，默认编辑器保持旧版。

## 2026-10-18 模块库第二批扩充（Torus/Wedge、Vortex/Align、Limit Force、Color/Size by Speed、Kill）

- 模块库从 26 个扩到 **38 个**：新增 `TorusLocation`、`WedgeLocation`、`RotateAroundPoint`、`VortexVelocity`、`AlignVelocityToRandomAxis`、`ScaleVelocityBySpeed`、`LinearForce`、`LimitForce`、`ScaleColorBySpeed`、`ScaleColorByVelocity`、`SpriteSizeByVelocity`、`KillParticles`。
- 新增两个图构造辅助：`ComposeColor`（float4 版本的基向量组装，供颜色按速度分量缩放使用）与 `Component`（用 `Dot` 投影取分量，IR 没有 swizzle）。`TangentBasis` 为任意轴构造正交切基，并对"轴与 up 平行"的退化情形做分支选择，避免叉积得到零向量。
- 语义实现要点：`TorusLocation` 用大半径圆环加管截面偏移，落点严格位于管面上；`WedgeLocation` 的截面半宽随轴向距离按 `1 + tan(angle)*along` 增长；`AlignVelocityToRandomAxis` 只旋转方向、保持速率不变（与参考描述一致）；`LimitForce` 对已累加的 force 直接缩放并诊断"需要前置力模块"；`KillParticles` 把 `Particles.Lifetime` 压到 `Age` 之下，复用既有压缩语义实现真正的动态死亡，而不是只写一个无效标记。
- 修正第一批遗留的阶段约束错误：`RotateAroundPoint` 属于 Particle Spawn（需要 `InitializeParticle` 前置），而 `VortexVelocity` / `AlignVelocityToRandomAxis` 属于 Particle Update（不应要求 spawn 阶段的模块），否则堆栈无法编译。
- 新增 2 个 CPU 行为用例（`Tests/Unit/VfxAuthoringTests.cpp`）：`SecondBatchModulesLowerWithCorrectSemantics` 断言环面落点到主圆环的距离等于小半径、零随机对齐保持速率为 13、false kill 不改变 Lifetime、true kill 使 Lifetime ≤ Age；`LimitForceRequiresAPrecedingForceAndClampsMagnitude` 断言缺少前置力时报诊断、超限力被压到上限（-100 → -10 在 1 秒积分后速度恰为 -10）。
- DX12 设备对照用例 `VfxGpuTests.Dx12AuthoredShapeForceAndSubUvModulesMatchCpu` 扩展为同时覆盖第二批的 `TorusLocation`、`LimitForce`、`VortexVelocity`，CPU 与设备逐属性比较仍全部一致。
- 本轮验证：VFX 套件 **55/55**（`build/vfx-modules2-vfx-suite.xml`）、行为/授权套件 **35/35**（`build/vfx-modules2-authoring-suite.xml`），DX12 调试层与 Vulkan Khronos 验证层开启。
- 覆盖率仍远低于冻结清单：276 个 Modules 中当前实现 38 个，Functions/DynamicInputs 未批量推进；`Test-NiagaraParity.ps1` 继续返回未通过，默认编辑器保持旧版。Array/Math/Utility/Events/Collision/Mesh/Material/Audio/Beam、网格与骨骼数据接口、以及依赖新粒子属性的渲染器（Ribbon/Light/Decal/Component/Volume）尚未实现。
- 工程结构变更提示：验证期间另一进程把候选编辑器从 `Project/Editor/VFX/Next/` 重组为 `Project/Editor/VFX/Editor/`（`VfxNextSession` → `VfxSession`），测试目标相应从 `NullusVfxNextTests` 变为 `NullusVfxAuthoringTests`，测试套件名从 `VfxNext` 变为 `VfxAuthoring`；本批新增模块与用例在该重组后重新验证通过。

## 2026-10-18 模块库第一批扩充（Location / Velocity / Forces / Orientation / SubUV / Size）

- 候选模块库从 12 个扩到 26 个：新增 `SphereLocation`、`CylinderLocation`、`ConeLocation`、`OffsetPosition`、`JitterPosition`、`AddVelocityInCone`、`AddVelocityFromPoint`、`ScaleVelocity`、`AccelerationForce`、`PointAttractionForce`、`VortexForce`、`SpriteRotationRate`、`SubUVAnimation`、`SpriteSizeBySpeed`，覆盖 Location/Velocity/Forces/Orientation/SubUV/Size 六个类别，每个都带 UE 安装版同名的 Category/Description 语义与类型化输入默认值。
- 新增 **模块声明所需属性** 机制：`ModuleDefinition` 增加 `attributes` 字段，栈降级在构建模块图之前把缺失的属性注册到发射器，类型冲突报诊断。此前 Sprite 渲染器虽然已经消费 `Particles.Rotation` 与 `Particles.SubImageIndex`，但没有任何模块能写它们，因此旋转与 SubUV 动画在候选编辑器里无法创作；现在 `SpriteRotationRate` / `SubUVAnimation` 会自动引入这两列。
- 为支撑形状类模块补了两个图构造辅助：`ComposeVector` 用基向量掩码把三个标量组装成 `float3`（IR 没有向量构造指令，新增指令会改动序列化的 Operation 枚举），`CubeRoot` 用 24 步二分求解均匀体积采样所需的立方根。二分是实测选择的：牛顿迭代在 u→0 时相对误差超过 400%（u=5e-6 处 6 步 maxRel=414%），而 24 步二分的全域相对误差上界为 6.8e-7，远小于 1e-5 对照容差。
- 力类模块沿用既有 `Transient` 约定累加到求解器：`AccelerationForce`/`PointAttractionForce`/`VortexForce` 只写入 force 累加项，由后续 `SolveForcesAndVelocity` 消费；缺求解器仍报"requires a following Solve Forces and Velocity module"，不会静默丢弃。
- 新增 3 个 CPU 行为用例（`Tests/Unit/VfxNextTests.cpp`）：模块库结构校验（25+ 模块、名称/类别/描述/输入非空、阶段自洽、旋转与 SubUV 模块确实声明渲染器属性）；新形状/速度/旋转/SubUV 模块的属性自动注册与数值语义（球面表面采样落点半径、零角度锥体精确跟随方向、180°/s 在 0.5s 得到 π/2、四帧 SubUV 随归一化年龄线性取值），并确认 CPU/GPU 两个目标都能编译；力累加与缺求解器诊断。
- 新增 DX12 设备对照用例 `VfxGpuTests.Dx12AuthoredShapeForceAndSubUvModulesMatchCpu`：同一份系统在 CPU 指令解释器与 DX12 计算管线上各跑一次 spawn+update，逐步比较全部属性列，容差 1e-5，确认新模块不会在两个目标间分歧。
- 本轮验证：VFX 套件 **55/55** 通过（含新增设备对照，DX12 调试层 + Vulkan Khronos 验证层，报告 `build/vfx-modules-vfx-suite.xml`）；`VfxNext` **14/14** 通过。
- 本轮为模块库第一批，覆盖率仍远低于冻结清单的 276 个 Modules / 250 个 Functions / 297 个 DynamicInputs；其余 Location/Forces/Orientation 变体、Array/Math/Utility/Events/Collision/Mesh/Material/Audio/Beam 等类别尚未实现。`NiagaraParity.json` 中这些条目的 `implementation`/`examples`/`evidence` 仍为空，`Test-NiagaraParity.ps1` 继续返回未通过，默认编辑器保持旧版。
- 本机环境提示：验证期间有其它进程在重组 `Project/Editor`（`VfxNextEditor.cpp`、`VFX/Next/*.cpp` 一度从磁盘消失），导致 `NullusUnitTests`/`EditorProject` 的整树编译被 `C1083` 打断；该现象与本批改动无关，VFX 与模块测试目标不受影响。

## 2026-10-18 CPU 粒子更新并行

- CPU 发射器的逐粒子更新内循环已并行化：`AdvanceParticles` 在整批推进（`first == 0` 且覆盖全部存活粒子）且粒子数达到阈值时按区间切分，**只使用** `Jobs::ScheduleParallelFor`；JobSystem 未初始化或没有 worker 时回退串行，不占用渲染 TaskGraph。阈值与上限为每任务 2048 粒子、最多 32 个任务，`NLS_VFX_CPU_PARALLEL=0` 或 `CpuEmitter::SetParallelExecutionEnabled(false)` 可强制串行做 A/B。生成、死亡压缩、碰撞事件收集仍留在主线程串行，因此事件顺序与 ID 保持确定性。随机数按稳定粒子 ID 推导，分片不改变取值。
- `CpuEmitter::LastParallelScheduler()` 显式区分 `Serial` 与 `Jobs`，`LastParallelTaskCount()` 只在实际 fan-out 后报告任务数；两者都只在真正并行执行时上报，便于宿主断言实际走了哪条路径。
- 三个新用例覆盖并行正确性（`Tests/Unit/VfxRuntimeTests.cpp`）：同一资产两份发射器分别跑串行与并行，逐粒子比较全部属性列、ID、统计与事件；多帧用例覆盖死亡压缩与第二批 Burst 重填之间的串行阶段；切换用例在运行中启用/关闭并行，确认 JobSystem 可用时确实走 `ParallelScheduler::Jobs`、不可用时报告 `Serial`，且两种情况结果完全一致。三个用例连续 5 次重复运行通过（`build/vfx-cpu-parallel-focused.xml`）。
- 完整 VFX 套件 **52/52**、编辑器/场景套件 **19/19** 在 DX12 调试层与 Vulkan Khronos 验证层下通过（`build/vfx-cpu-parallel-vfx-suite.xml`、`build/vfx-cpu-parallel-editor-suite.xml`）。设备用例日志无验证层错误或设备丢失。
- RelWithDebInfo / DX12 交错 A/B（`Tests/Performance/VfxParticlePerformanceTests.cpp` 的 `CpuEmitterScalingBaseline`，同二进制仅切换 `NLS_VFX_CPU_PARALLEL`）：100k 规模 120 帧各 3 轮串行 12.04/12.32/12.63 ms、并行 5.84/6.01/6.52 ms；300 帧单容量交错两轮为 12.04/12.01 对 5.70/5.77 ms。收窄为仅 JobSystem 后按 300 帧三容量交错复测（`build/vfx-cpu-parallel-jobs-only`）：100k 串行 13.59/14.71 对 Jobs 6.55/6.70 ms，10k 串行 1.67/1.68 对 Jobs 0.92/0.96 ms，1k 低于 2048 阈值保持串行（0.145–0.159 ms）。原始报告位于 `build/vfx-cpu-parallel-ab`。
- `NLS_VFX_PERF_WORKERS` 可固定基准用的 Jobs worker 数，实测 100k 扩展曲线（300 帧）：0 worker 12.58 ms、2 worker 7.25 ms、4 worker 6.60 ms、8 worker 5.87 ms、16 worker 6.32 ms、32 worker 6.42 ms，重复 0 worker 12.88 ms。收益在约 8 worker 时饱和，继续增加 worker 反而回退，说明剩余成本来自 Tick 内未并行化的串行阶段（生成/死亡压缩、`CalculateParticleBounds` 与每视图可见性扫描）以及调度与唤醒开销，而非可用并行度不足。
- 并行模式下发射器生命周期 soak 60 秒通过：20k 容量、1410 轮、42300 帧，首轮均值 1.472 ms、末轮 1.652 ms，容量始终不超过硬上限。`VfxParticlePerformanceTests` 的 CPU 扫描与 soak 现在显式启动 Jobs 池，否则 `CpuEmitter` 会静默回退串行、使并行收益无法被测量。
- 本轮只并行化单发射器内的逐粒子更新；未覆盖系统级多发射器批处理、GPU 模拟与渲染准备、事件回读和多视图资源创建。加速比受引擎 Job 唤醒与提交开销限制，实测 100k 约 2.0–2.1x（低于满 31 Worker 的理论上限），1M 级规模与更长 soak 尚未测量。旧文件格式与运行时包 ABI 不变。
- Game 宿主接线（2026-10-18）：`Driver` 现在保存并公开它与渲染 TaskGraph 共用的同一份硬件预算（`GetBudgetedJobsForegroundWorkerCount` / `GetBudgetedJobsBackgroundWorkerCount`），Game `Context` 在 Driver 构造后用它通过 `TryInitializeJobSystem` 启动 Jobs 池，不再自行解析 `hardware_concurrency()`；`~Context` 在 `ShutdownThreadedRendering()` 之前先用 `DrainAcceptedWork` 关闭自己拥有的池。`Game::PreUpdate` 与 Editor 一样每帧 `DrainMainThreadContinuations(64)`，否则后台制品加载会走 `StartMeshArtifactLoad` 失败分支而不是 Job 路径。
- 本机启动验证（Debug / DX12 / 32 逻辑核）：`Driver: rendering TaskGraph workers=12 jobsForegroundWorkers=12 jobsBackgroundWorkers=6`，随后 `Game jobs: foregroundWorkers=12 backgroundWorkers=6 ... ownsJobSystem=true`，即两个池来自同一次预算切分而非各自计算（日志 `build/vfx-game-jobs-startup.log`）。
- 新增三个断言用例：`DriverNullDeviceReadinessTests.PublishesTheJobsSplitOfTheRenderingBudget` 锁定公开的 Jobs 切分与 `ResolveExecutionPoolBudget` 一致；`GameObjectAssetImportTests.AsyncMeshArtifactLoadCompletesThroughTheJobPool` 断言请求保持 pending、不进入失败集合、并最终经后台 Job worker 完成；`VfxRuntimeTests` 的并行用例用 `LastParallelScheduler()` 断言粒子 fan-out 只走 `ParallelScheduler::Jobs`，JobSystem 不可用时必须报告 `Serial`。
- 双池占用采样：双池同时运行的 Game 进程在空闲场景连续 10 秒的平均 CPU 占用为整机 **2.4%**（32 逻辑核、60 线程），即 24 个池 lane 常驻但没有可运行工作时基本不消耗 CPU，未观察到空闲自旋或超订。该采样是空闲态基线，不覆盖满载渲染与粒子同时运行的峰值。
- 以共享预算的 12 个 Jobs worker 复测 100k 满占用 tick（240 帧）为均值 6.99 ms / P95 8.65 ms，与编辑器自动池（31 worker，6.5–6.7 ms）差距在噪声范围附近；本轮仍未采集 Game 内嵌粒子系统的端到端帧时间，因此 `kMinimumParticlesPerParallelTask` 与任务上限是否按 Game 实际负载重调仍待 Game 端测量。
- 本轮仍是本机 Windows 验证，未执行跨平台 CI。

## 2026-10-18 独立库资产、函数与继承

- 新增 `.vfxscript` 的 Module/Function/DynamicInput 与 `.vfxemitter` 源资产，以及 **Window > Particle Library Editor**。支持稳定资产/输入/版本标识、版本创建和公开、模块使用阶段、输入默认值、脚本图、函数调用、静态开关及模块前置依赖。资产浏览器 Edit/双击可打开这两类源文件。
- System 可引用固定版本模块和发射器；输入可引用独立 Function/DynamicInput。函数按引用展开到现有 CPU/GPU IR，静态开关在编译期选择分支；未被选择的表达式不进入运行程序。函数/继承循环、丢失版本、类型/阶段不兼容和失效覆盖均报告诊断，不替换上一成功程序。
- 发射器继承保留源模块 ID，实例按显式字段覆盖设置、模块状态和输入。候选 Details 可切换源版本、编辑继承输入、恢复父级和打开源资产；改变 exposed version 不会静默升级已有引用。同一发射器资产的多实例使用不同的确定性图 ID，程序可序列化和独立运行。
- 新增 native Script/Emitter 工件类型及源类型映射；System 导入记录传递脚本/发射器的源 GUID 与哈希，依赖修改使 System 工件失效。候选编辑器异步刷新库，依赖语义改变会重编译，库错误继续使用旧程序。源资产工件与 System 运行时程序分离。
- `Templates/VFX/Examples/Library/ReferencedSystem.vfx` 展示同一发射器的 CPU/GPU 双实例、模块到函数的引用和继承颜色覆盖；其三后端无源码包编译通过（`build/ReferencedSystem.compiled.json`）。仅此编译证据不构成 Linux/Metal 设备验收。
- 追加后本机行为测试 **19/19**、候选编辑器/设备/导入测试 **5/5** 通过（`build/vfx-library-focused.xml`、`build/vfx-library-editor.xml`）。新增覆盖版本持久化与撤销、固定版本、嵌套函数、静态分支、动态输入资产、源文件移动/重复 GUID、继承覆盖/循环、依赖更新和源文件全部移除后运行时加载；DX12 调试层验证带库引用/继承的红色 CPU 与绿色 GPU 同屏像素。
- 绘制验证发现并修复新脚本图和既有候选动态图中 `EndCreate` / `EndDelete` 错误地只在 Begin 返回 true 时调用的问题。节点库要求每次 Begin 均配对 End；修复后库编辑器绘制不再触发断言。
- 独立库是候选功能，完整版本转换脚本、所有输入分布、所有继承编辑操作及全部 Niagara 核心能力尚未完成，默认 Particle Editor 保留。
- 修正模块栈中的 NormalizedAge：它现在读取前序模块更新后的 Age/Lifetime 参数映射，而不是直接读取该阶段入口的原始属性。候选 VFX importer 升到 **13** 以使既有工件重新验证；Shader 布局保持 **10**，运行时包 ABI 不变。
- 本轮相关回归：旧版 VFX **52/52**、编辑器/场景 **19/19** 通过（`build/vfx-library-legacy-vfx.xml`、`build/vfx-library-legacy-editor.xml`）。通用资产/manifest/native 容器/浏览器回归 **334** 项中 **325** 通过、**9** 项环境跳过、0 失败（`build/vfx-library-asset-final.xml`，显式 `DXC_PATH`）；初跑发现材质依赖预处理的提前校验分支返回了不同于已有契约的诊断码，已统一为 `assetdatabase-material-source-native-container-required` 并结束失败任务，原有行为断言通过。

## 2026-10-17 编辑器重建候选版

- 新增 **Window > Particle Editor Next**，原有 Particle Editor、资产创建和双击默认入口保留。v2 System 模块栈与旧文件格式隔离，不自动迁移；禁止覆盖旧格式文件。范围和完整缺口见 [NiagaraEditorRebuild.md](NiagaraEditorRebuild.md)。
- 已实现 System Overview、按阶段组织的模块栈、输入绑定/动态表达式、线性/三次曲线与颜色渐变、参数、时间线、日志定位、多文档独立撤销及视图缓存。当前模块库为 12 个候选实现，不能据此宣称完整 Niagara 模块对齐。
- v2 通过 SSA 属性映射降低到已有 CPU/GPU IR；语义与布局修改分离，编译失败保留上一程序。临时预览参数覆盖与资产默认值分开。UI Seek 分批重模拟，预览连续消费 GPU 世界帧；暂停不重复分配相同画面的渲染资源。
- 完整系统预览复用 `SystemInstance` / `SystemRenderProxy` / `ParticleRenderer`，验证红色 CPU、绿色 GPU 同屏。v2 System 已接入 native 导入、依赖校验、运行时加载和失败保留工件；`new-next` / `compile-next` 可创建和生成既有运行时可消费的包。
- 冻结安装版 **5.7.4 CL 51494982** 的 1193 条参考/验收要求；实际运行 UE Python commandlet 导出 917 个核心资产的 registry 类别、可见性、阶段掩码、建议及弃用标记。受保护的属性未能通过 Python 读取，已显式记录，默认值、函数签名与行为仍需进一步采集。文件清单和 registry 标签不是功能验收，矩阵仍全部待对照验证。
- 本批最新 Debug `Editor`、`NullusVfxAuthoringTests`、`NullusVfxEditorPanelTests` 构建通过；Editor `--help` 通过。新增行为测试 **9/9**、编辑器/设备/导入测试 **3/3** 通过，报告 `build/vfx-next-focused.xml`、`build/vfx-next-editor-focused.xml`。绘制测试覆盖多文档、停靠隐藏恢复和三档字体缩放；它不代表桌面点击或 DPI 截图对齐。
- DX12 RTX 5070 Laptop 启用调试层，通过混合目标像素和 Seek 帧消费验证。v2 Starter 已生成 DX12/Vulkan/Metal 无源码包；Linux/Vulkan 与 macOS/Metal 设备、UE 画面/交互对照、生产工具和 30 分钟混合压力尚未验收。
- `Test-NiagaraParity.ps1` 在全部适用条目提供实现、示例及通过的 UE 对照证据前返回非零状态，并拒绝提前切换默认编辑器。
- 最终相关回归：旧版 VFX **49/49**、编辑器/场景 **19/19** 通过（`build/vfx-next-legacy-vfx.xml`、`build/vfx-next-legacy-editor.xml`），加上新增 **12** 项共 **80** 项、0 失败。旧版用例包含 Windows DX12/Vulkan 设备路径；这不替代新编辑器的 UE 交互/画面对照及其他操作系统验收。两个候选测试目标分别为 `NullusVfxAuthoringTests` 和 `NullusVfxEditorPanelTests`，输出位于 `build/vfx-tests/Debug`。

## 已接入的能力

- ImGuiNodeEditor 核心源码直接由 `NLS_UI` 编译，保留 MIT 许可证、固定上游提交及本地 API 导出记录，没有独立库目标或子模块。
- `NLS_VFX`、`NLS_VFXCompiler`、带稳定节点 ID 的类型化表达式图、版本化 JSON 源资产、CPU 指令程序及 GPU HLSL。
- CPU SoA 属性、固定种子随机数、速率/Burst/循环/插值生成、寿命、稳定压缩、暂停、停止、重置、Seek 和容量统计。整批逐粒子更新在粒子数达到阈值时通过 Jobs 池并行执行（唯一并行方案，不占用渲染 TaskGraph），单发射器内保持逐粒子确定性。
- CPU/GPU 共用发射时钟，按实际出生顺序保留粒子 ID；容量丢弃的请求仍消耗 ID，不在后续帧补发。连续速率生成使用批次描述。
- GPU 默认属性与 64 位 ID 初始化、Spawn/Update、年龄推进、寿命压缩、存活计数及绘制参数生成。GPU 压缩后的槽位顺序不保证稳定，比较和随机数使用粒子 ID。
- GPU 帧包捕获参数和强资源句柄，提交成功后才推进 CPU 侧时钟与存储视图；拒绝重复帧和重复提交，录制失败需丢弃整张命令表。
- DX12/Vulkan/Metal 的单条 `DrawIndirect`、`DrawIndexedIndirect`、`DispatchIndirect` 入口、延迟命令录制、范围校验及能力查询。非零 firstInstance 有独立能力标记。
- FrameGraph 粒子模拟与 Sprite 通道、间接参数读取状态、透明混合、相机朝向、颜色和标量/二维尺寸。多个视图可消费一次模拟的输出。
- 发射器提供 `Sort by View Depth`：CPU/GPU 场景 Sprite 与编辑器预览按世界空间视图深度从远到近绘制，同深度按 64 位粒子 ID、槽位排序。GPU 采用 256 线程共享内存分块与全局双调合并，每视图独立索引及常量，不重排模拟 SoA/ID，也不回读存活数量；非整幂容量补哨兵，NaN/Inf 深度归入末尾有效粒子。排序支持当前 1000 万容量校验上限，性能验收仍待进行。
- 发射器之间的透明顺序：`ParticleSceneRenderer` 为每条透明绘制命令计算视图深度键，并在开启 pass 前按远到近稳定排序，因此多个粒子组件与多个发射器的透明流不再按声明顺序互相覆盖。CPU 发射器随渲染帧发布真实模拟空间包围盒中心；GPU 发射器在帧提交后异步复制 48 字节范围，首份范围立即请求、之后每四个已提交帧刷新一次，只轮询完成令牌而不等待 fence。最新已完成范围按帧号与 Reset 代际发布，同一组件内的 GPU 发射器也能按真实范围排序；首帧或读回失败期间仍以组件原点作为回退。销毁时仍未完成的读回会进入隔离队列，保持目标内存和源缓冲存活直到 completion 完成。同深度保持声明顺序，Opaque/Masked 走写深度路径不参与该排序。
- 场景图形 pass 可在开启 RenderPass 前执行计算分发，覆盖串行和并行录制路径；含计算分发的 pass 保持完整，不做绘制切片或 child command 录制。排序资源随绘制命令及 GPU fence 保留。
- CPU 发射器缓存动态中心范围、最大 Sprite 半对角线、存活数和无效几何数量，随移动/死亡/清空更新并进入不可变世界帧。GPU 每个模拟帧执行 256 线程分组归约及最终归约，生成独立的 48 字节范围缓冲；后续帧与多视图不覆写早期范围。
- `Frustum Culling` 默认开启。每视图先判断发射器范围，再保守测试 Sprite 的六个裁剪平面，以稳定扫描/散射生成可见索引和独立间接参数，随后按需深度排序。模拟属性、ID 和存活计数保持不变；局部中心跟随世界矩阵，Sprite 尺寸保持当前渲染器的世界单位语义，涵盖非均匀/负缩放。位置/尺寸或变换后几何非有限时不产生绘制并单独统计。
- 预览提供 `Show Bounds`、范围数值和 Alive/Visible/Culled/Invalid 统计。GPU 回读仅用于调试，使用非阻塞轮询及独立结果所有权；渲染不等待回读，关闭/切换文档可放弃未完成回读，旧图像资源按原有 GPU/UI 生命周期保留。
- CPU/GPU 区分粒子硬上限与初始分配容量，`Initial Allocation=0` 默认最多 256 槽。CPU 属性列/ID 按需增长，冻结副本只复制已用数据；GPU 按保守存活上界在出生前增长，逐 SoA 列及 64 位 ID 复制，重建 ping-pong 存储，并由录制事务保留旧资源。未提交的扩容录制不推进实例，Reset 保留分配容量；FrameGraph 的逻辑输出随物理缓冲代际重新绑定。GPU 常规帧不回读存活数，保守上界仍可能使长时间运行的发射器增长到硬上限。
- GPU 粒子 Simulation Stage 支持稳定 ID、顺序、启用/禁用、每阶段 0–128 次迭代、每发射器最多 64 阶段/1024 总迭代、参数快照及阶段/迭代索引节点。每个模拟子步在 Update/Spawn 后执行，迭代间可读取前一轮写入，保持每粒子表达式先读后写语义；随机流包含阶段/迭代上下文，Age 只由普通更新推进，阶段之后压缩寿命并更新范围。CPU 发射器配置 Simulation Stage 会给出编译诊断。
- 编辑器提供 Simulation Stages 页签、增删/命名/启停/排序/迭代数、独立节点图、诊断定位、撤销/重做、保存恢复及 GPU 预览，统计显示当前分配容量与硬上限。阶段字节码进入版本化平台包，可剥离节点和 HLSL；`StagedGpuEmitter.vfx` 提供扩容和三个连续阶段的示例。
- Vulkan 异步缓冲区读回，包含子范围复制、GPU fence、映射/缓存失效和延迟资源释放。
- PhysX 5.6.1 固定源码依赖及引擎查询世界；盒、球、胶囊、Cooking 三角网格、过滤、多命中射线、球 Sweep/Overlap、代际句柄和只读访问锁。
- CPU 粒子使用 PhysX 查询碰撞、反射/摩擦响应；每次碰撞记录一条 `ParticleEventKind::Collision` 事件（含粒子 ID、接触位置与命中的物理体）。GPU 启用 CPU PhysX 碰撞会产生编译诊断。**注意：这里只有事件“采集”，没有事件“处理”**——编译器仍拒绝 `Stage::Event`（报 "This execution context is not implemented yet"），因此 Generate/Receive 系列事件模块与跨发射器事件脚本尚未实现。
- 每个 Scene 按需持有 PhysX 查询世界，并在 `Update`/`FixedUpdate` 之前统一同步碰撞体；`PhysicsColliderComponent` 提供盒/球/胶囊（Y 轴胶囊自动对齐 PhysX 的 X 轴几何）、世界缩放、层掩码与 Trigger，随场景序列化并提供可移植脚本访问器。
- 粒子组件在发射器启用碰撞时自动绑定所属场景的查询世界，不再要求手工注入；禁用、移除碰撞体或销毁对象会立即从查询世界移除，物理修订号随每次发布递增。
- Particle Editor 的 Spawn/Update 图、建连/删除、属性、诊断、撤销重做、拖动事务、保存/导入及 CPU 预览。
- 编辑器 CPU/GPU 目标切换、容量、种子、速率、持续时间、循环、插值生成及 Burst 控件。
- 所选 CPU/GPU 发射器共用异步 Sprite 预览，支持播放/暂停/重置和尺寸变化；编辑稳定后编译，完成 GPU fence 后发布图像，关闭或替换文档会取消旧任务。CPU 快照按原始发射器索引映射，混合系统选择 CPU 发射器时不依赖其在 CPU 列表中的位置。
- Sprite 支持稳定 GUID/localId 材质/贴图引用、颜色/UV/采样器/发光参数快照、RGBA 贴图、SubUV 网格及相邻帧插值、相机/速度对齐和弧度旋转。Translucent/Additive/Premultiplied/Masked/Opaque 有独立混合与深度写入规则；场景插入遮罩/实心粒子 pass 时同步重映射 work-unit 索引和依赖，先于透明物体与透明粒子绘制。视锥范围对旋转和速度对齐保持保守。
- Mesh Renderer 支持 1–8 个导入的静态网格子资产槽、每槽材质覆盖，以及 Float/Float3 缩放和 xyzw 四元数朝向绑定。`Particles.MeshIndex` 可换绑：有限值四舍五入后夹到有效槽，缺失属性选择槽 0，NaN/Inf 计为无效绘制。旋转归一化，零四元数采用单位旋转；局部网格中心/半径、非均匀/负缩放与组件世界变换进入保守范围。Game 冻结每槽完整 LOD 链、MinLOD、阈值和强 RHI 缓冲；Render 根据每视图投影与组件变换选择每槽 LOD，或将显式索引夹到有效原始 LOD。当前每槽全部粒子共用该视图的 LOD；逐粒子 LOD、分段材质与光照仍待扩展。
- Sprite/Mesh 共用 `ParticleRenderer` 的材质快照、剔除、排序和录制；Mesh 的每视图计算重建实际几何范围和 20 字节索引间接命令，调用 DrawIndexedIndirect，不覆写模拟计数或其他视图。视图缓冲前 16 字节是统计，16 字节起是主索引命令，多槽命令从 48 字节起连续排列；现有 32 字节统计读回保持兼容。每槽有独立颜色、UV、发光、动态参数、贴图/采样器和材质常量，并保留所有几何/材质资源。Mesh 复用无光照 `ParticleSprite` 片元 ABI 7，使用网格 UV，忽略 Sprite 尺寸/弧度旋转/朝向/圆形遮罩。多槽采用分别绘制和顶点筛选，透明粒子跨槽不保证单一全局顺序。
- 编辑器可切换 Sprite/Mesh、增删网格槽、选择每槽材质覆盖、编辑 MeshIndex/LOD/Scale/Orientation，支持撤销/重做和保存。CPU/GPU Mesh 预览使用真实几何和异步范围统计，暂停时任何槽的材质或 LOD 资源/阈值变化均可重绘。`MeshShards.vfx` 提供基础碎片，`MultiMeshShards.vfx` 与自包含 `Meshes/ParticleShard.gltf`、`Meshes/ParticleChip.gltf` 提供 Spawn 随机选择两种网格的双目标示例。
- `ParticleSprite` ShaderLab 材质通道执行用户 HLSL 片元函数，输入包含颜色、UV、世界位置、速度、年龄/寿命、四组动态 float4；标准 Sprite VS 和材质 PS 共享同一 HLSL 接口，编译器嵌入的接口来自同一个引擎头文件。普通引擎材质的参数快照模式继续兼容。材质通道校验固定 ABI、只读资源空间及布局，不接受通用表面 VS 或可写资源；当前尚无可视化材质图、光照和材质顶点变形。
- 四组动态参数可绑定自定义 float4 粒子属性，缺失属性时按系统参数/默认值回退；材质常量可绑定 Float/Float2/Float3/Float4 系统参数并校验目标反射类型。Game 捕获常量字节及强纹理句柄，Render 创建每次绘制的独立常量资源、材质绑定及缓存 PSO，后续参数变更不覆写早期帧。CPU/GPU 模拟、Forward/Deferred 与预览共用。
- 粒子编辑器提供材质通道开关、动态属性与回退绑定、材质常量绑定及临时预览覆盖，支持撤销/重做与保存；暂停时修改材质参数会生成新图像。组件的 `SetVectorParameter` 与 `SetFloatParameter` 均进入反射脚本 API，绑定参数随系统帧冻结。
- 软粒子按投影系数重建视空间深度，适用于透视/正交投影；Forward/Deferred 与预览共用。RHI 支持深度只读附件同时作为采样纹理，Vulkan 描述符/屏障使用一致布局，Driver 保留组合状态；单采样且有 Sampled 用途的深度目标可用，缺少可采样深度时使用普通透明绘制。预览提供可调深度平面。
- VFX 导入器记录材质/贴图的导入工件和源 GUID 依赖，错误类型或无法解析的引用拒绝发布并保留旧工件；运行时包包含依赖闭包。Windows 编辑器 manifest 可包含本机 `win64-dx12` 贴图，具体打包平台仍要求精确平台匹配。贴图移动保留 GUID/localId，运行时不读取源图。
- Vulkan 验证层检查补齐三处前置修复：仅粒子场景的空通道不能声明颜色目标已写入或作为已完成写入的依赖；后端不支持贴图原地更新时初始上传到替代纹理，上传失败保留旧纹理及视图，兼容已有加载回退行为；Deferred Lighting 的实际深度附件与录制 PSO 布局一致。手工提交 FrameGraph 命令的设备测试也补齐提交后的资源状态发布，与 Driver 的提交契约一致。
- DX12 附件自动转换过滤前后状态相同的屏障，避免目标已在 RenderTarget 状态时生成非法转换并导致命令表关闭失败。设备测试的失败信息会读取可用的 D3D12 InfoQueue，便于追踪后端错误。
- 原生 VFX 源资产识别、CPU 指令及平台 Shader 字节码工件、失败重导入保留旧工件、独立编译工具和 CPU/GPU/混合发射器模板。当前 VFX importer 版本为 17；其他 importer 版本以 `AssetMeta.cpp` 为准。包括全部网格槽/材质覆盖/贴图的类型与稳定依赖检查，错误引用保留旧工件。导入的材质记录 Shader/贴图稳定身份，粒子运行时可通过 manifest 加载对应 Shader 工件，无需源 Shader、`.meta` 或 ArtifactDB。
- 运行时包支持版本 2/3/4/5（动态控制使用 3，Bool/Int32 使用 4，System timing 使用 5），资源布局版本 11，保留兼容的布局 10 工件读取；可剥离图节点及 HLSL，包含 DX12 DXIL、Vulkan SPIR-V 和 Metal 输入 SPIR-V。Mesh 使用 MeshVS，包验证根据 renderer 严格检查入口；Sprite 继续使用 SpriteVS。反序列化检查后端、内核、入口、阶段、字节码编码及布局版本，失败保留旧程序。不兼容的旧布局工件需从源资产重新导入。外部网格/材质/贴图仍须由原生资产依赖闭包打包，VFX CLI 的单个 JSON 不嵌入这些外部资源。
- `ParticleSystemAsset` 通过运行时 manifest 和授权的原生内容工件加载；`ParticleSystemComponent` 使用类型化 PPtr，支持 Inspector 选择、场景序列化、Play On Awake、启动/停止/暂停/重置、参数与热重载。成功替换保留参数和控制状态，失败继续使用旧程序。
- 资产浏览器右键 `Create > Particle System` 在目标文件夹按唯一名称写入默认可编辑 `.vfx`（走现有文件创建撤销与预导入路径），创建后直接打开 Particle Editor；已有的 `.vfx` 源资产可通过右键 `Edit` 或双击打开同一编辑器。空目录背景、网格/列表条目与文件夹树的右键菜单共用该入口。
- `SystemInstance` 在 Game 更新 CPU 发射器并发布不可变参数/属性帧；组件通过 Render 命令管道按顺序推进 GPU。每个设备、每个世界帧只推进一次模拟，多个视图复用冻结输出；GPU 属性、ID、间接参数复制到独立快照，后续帧不覆写早期视图。
- 普通 Forward/Deferred 场景通过原有绘制提交路径消费 Sprite 间接绘制，拥有独立视图常量与强资源句柄；支持仅粒子场景、零粒子帧和组件移除后的在途资源保留。Prefab 引用解析识别 ParticleSystem 类型。
- Activate、Stop、Restart、SetPaused、GetAge、SetFloatParameter、SetVectorParameter、SetIntParameter、SetBoolParameter、ReloadSystem 已进入自动生成的 Script API schema；完整 C#/Lua 使用场景仍待验收。

## 本机验证（2026-10-15 至 2026-10-16）

- 跨发射器透明排序批次（2026-10-16）：Debug `NullusVfxTests`、`NullusVfxEditorTests`、`NullusUnitTests`、Editor、Game 构建通过（`build/vfx-sort-final-build.log`）。VFX 套件 49 项、编辑器/场景套件 17 项在 DX12 调试层与 Vulkan Khronos 验证层下全部通过（`build/vfx-sort-vfx-suite.xml`、`build/vfx-sort-editor-suite.xml`）。新增 `Dx12/VulkanTransparentParticlesOrderEmittersByViewDepth`：两个深度不同的半透明 CPU 发射器按“近者先声明”排列，断言间接参数顺序为远者在前且中心像素合成为近者占优；临时关闭排序后两个断言均按预期失败（`build/vfx-sort-negative.log`），确认用例确实覆盖该修复。
- 粒子性能与稳定性基线（2026-10-16）：新增 `Tests/Performance/VfxParticlePerformanceTests.cpp`（`NullusPerformanceTests`），覆盖 CPU 1k/10k/100k 满占用稳态 tick、GPU 10k/100k/1M 同步帧、以及可配置时长的发射器创建/填充/Reset/销毁 soak；容量列表、帧数、soak 时长与后端均由环境变量覆盖（`NLS_VFX_PERF_CPU_CAPACITIES`、`NLS_VFX_PERF_GPU_CAPACITIES`、`NLS_VFX_PERF_BACKEND`、`NLS_VFX_SOAK_SECONDS` 等），并通过 `NLS_PERFORMANCE_REPORT_DIR` 输出文本报告。
- RelWithDebInfo 基线（RTX 5070 Laptop / DX12 / 32 硬件线程，`build/vfx-perf-release.xml`）：CPU 发射器每帧稳态成本 1k = 0.124 ms、10k = 1.28 ms、100k = 12.68 ms（每粒子每帧约 0.127 µs，模拟缓冲 0.057/0.572/5.72 MiB）；Debug 构建同一用例为 0.656/6.45/61.9 ms。单发射器 CPU 更新在 100k 时已接近 60 FPS 预算，是后续需要并行化的直接依据。
- 同一基线中 GPU 同步帧（record + submit + fence 排空，尚无 GPU 时间戳查询）为 floor(1 粒子) = 4.90 ms、10k = 5.20 ms、100k = 5.59 ms、1M = 6.32 ms；1M 相对 floor 的边际成本约 1.42 ms，说明当前数字主要由同步/提交开销而非粒子模拟本身构成。
- GPU 透明排序范围读回批次（2026-10-17）：DX12 与 Vulkan 的 `TransparentGpuEmittersOrderSameComponentByReadbackDepth` 均通过（`build/vfx-gpu-order-focused.xml`）；测试把近、远 GPU 发射器放在同一组件并按近者先声明，首帧断言回退到声明顺序，第 6 帧断言异步范围就绪后切换为远者先绘制，中心像素合成为近者占优。完整 VFX 49 项和编辑器/场景 19 项在 DX12 调试层与 Vulkan Khronos 验证层下通过（`build/vfx-gpu-order-vfx-suite.xml`、`build/vfx-gpu-order-editor-suite.xml`）。
- 隔四帧范围读回的交错三轮性能对照（RelWithDebInfo / DX12 / 每次 120 帧）：10k 均值 4.69→5.02 ms、P95 5.19→6.34 ms；100k 均值 5.19→5.75 ms、P95 5.90→6.73 ms；1M 均值 7.99→7.88 ms、P95 15.11→16.83 ms。范围读回没有同步等待，开销来自额外的异步拷贝提交；10k/100k 的均值增量约 0.3–0.6 ms，1M 均值差异在当前同步帧噪声内。原始报告位于 `build/vfx-gpu-order-throttle4`。
- 发射器生命周期 soak：默认 30 秒通过（383 轮、11490 帧、首轮 2.617 ms、末轮 2.517 ms）；30 分钟验收（`NLS_VFX_SOAK_SECONDS=1800`）通过：23152 轮、694560 帧、20000 容量，首轮 3.094 ms、末轮 2.423 ms，`Data().Capacity()` 始终不超过硬上限，`build/vfx-perf-soak30.xml`。该 soak 覆盖发射器创建/填充/Reset/销毁与每帧更新，不覆盖场景切换、窗口缩放与资源热重载。
- 资产面板创建入口批次（2026-10-16）：Debug `NullusUnitTests`、`NullusVfxTests`、`Editor` 构建通过（`build/vfx-assetmenu-build.log`）；部署后的 Editor `--help` 启动通过。完整单元套件 3968 项中 3937 通过、31 项环境跳过、0 失败（`build/vfx-assetmenu-unit.xml`）；VFX 套件在 DX12 调试层与 Vulkan Khronos 验证层 49 项全部通过（`build/vfx-assetmenu-vfx.xml`），包含新增的 `.vfx` 创建载荷文档测试与资产浏览器右键 `Edit` 能力测试。菜单点击与桌面人工交互仍需在编辑器内确认。
- 多网格/LOD 收尾批次（2026-10-16）：修复 `uint[8]` 的 HLSL 常量数组步长与 C++ 打包不一致导致的错误网格数量/间接参数，以及多槽共享首槽颜色/纹理/采样器的问题；修复 LOD 的行主序投影、变换缩放、MinLOD 和暂停预览的非首槽材质/LOD 更新。资源布局升级到 10，VFX importer 升级到 12，旧粒子工件需重导入，片元材质 ABI 7 保持兼容。
- 本批 Debug `NullusVfxTests`、`NullusVfxEditorTests`、`NullusUnitTests`、`NullusVfxCompile`、Editor 和 Game 构建通过（`build/vfx-meshslots-integration-build.log`、`build/vfx-meshslots-final-build.log`）；部署后的 Editor/Game `--help` 启动通过。48 项 VFX 专项全部通过（`build/vfx-meshslots-focused.xml`），包含 DX12 调试层、Vulkan Khronos 验证层、8 槽、不同索引数、257 粒子、有限索引夹取/NaN/Inf、CPU/GPU 图像与参考范围计数对照、独立槽贴图及自动/显式 LOD；日志无验证层错误或设备丢失。
- 本批 15 项编辑器/场景全部通过（`build/vfx-meshslots-final-editor.xml`），包含双网格和材质覆盖的类型化依赖闭包、错误引用回滚、撤销重做保存、暂停预览非首槽材质及第二级几何更新、旧图像保留，以及 DX12/Vulkan Forward/Deferred 的混合目标、多槽命令和销毁后的资源保留。首轮场景用例的预期数量已按遮罩/透明 pass 分开修正，最终两后端场景均通过。
- 本批相关资产/材质/场景序列化/RHI 扩展回归单独执行 1014 项：1009 通过、5 项因符号链接环境限制跳过、0 失败，约 135 秒（`build/vfx-meshslots-regression.xml`）。与上述两组专项合计 1077 项、1072 通过、5 跳过、0 失败；相关源码和示例无尾随空白，`git diff --check` 通过。该结果为本机 Windows 验证，未执行跨平台 CI 或性能/长时间压力验收。
- `MultiMeshShards.vfx` 三平台无源码包编译通过（`build/MultiMeshShards.compiled.json`）：2 个发射器、各 2 个网格槽、3 个平台、布局 10，Spawn 按固定随机流选择 Shard/Chip，节点/HLSL 已剥离；导入测试核对两个示例网格的稳定 GUID/localId。Metal 输入 SPIR-V 仍不构成 Apple 设备运行证据；逐粒子 LOD、跨槽透明顺序、桌面人工交互与长时间压力仍在后续范围。
- Mesh 批次（2026-10-16）：`NLS_VFX`、`NLS_VFXCompiler`、`NLS_Engine`、`Editor`、`Game`、`NullusVfxCompile` 和三个测试程序 Debug 构建通过（`build/vfx-mesh-verified-build.log`）；实际部署目录 `App/Win64_Debug_Runtime_Shared` 中 Editor/Game 的 `--help` 启动通过，`git diff --check` 通过。当前包布局 8、VFX importer 10，旧粒子工件需要重导入；粒子片元材质 ABI 7 保持不变。
- 47 项 VFX 专项通过（`build/vfx-mesh-focused.xml`）；最终 Mesh 专项再次在 DX12 调试层与 Vulkan Khronos 验证层通过（`build/vfx-mesh-debug-device.xml`）。设备为 NVIDIA RTX 5070 Laptop / Windows DX12、AMD Radeon 610M / Windows Vulkan，Vulkan 验证错误计数不增加，日志无 VUID/Validation Error。真实索引实例绘制覆盖 CPU 冻结数据与 GPU 模拟图像对照、网格四元数归一化、非均匀/负缩放、偏心网格和世界剪切变换、0/1/257 粒子、跨 256 线程扫描边界、NaN、关闭剔除、CPU 参考范围/可见计数、多视图独立参数及 20 字节索引命令尾字段。
- 最新扩展回归 1076 项：1071 通过、5 项因环境无法创建符号链接而跳过、0 失败，约 253 秒（`build/vfx-mesh-regression.xml`，日志 `build/vfx-mesh-regression.log`）。包含最新全部 15 项编辑器/场景测试；新增网格导入依赖闭包/错误引用回滚、配置撤销重做保存、CPU/GPU 网格预览、Forward/Deferred 中遮罩写深度/透明软深度/自定义材质参数及对象移除后的资源保留。场景使用移除了源网格、Shader、Material、meta 和 ArtifactDB 的运行时内容，验证 manifest 加载；MeshManager 卸载后早期命令仍持有顶点/索引缓冲。
- 暂停预览修改材质和尺寸、Mesh 对象销毁后 CPU/GPU 图像、两后端网格场景最终复验 3 项通过（`build/vfx-mesh-final-editor.xml`）；DX12 调试层的 Mesh 场景另行通过（`build/vfx-mesh-dx12-scene-debug.xml`）。预览窄网格像素测试按预览相机的 clip-Y 偏移定位；桌面人工交互和长时间压力尚未验收。
- `MeshShards.vfx` 三平台无源码编译通过（`build/MeshShards.compiled.json`）：2 个发射器、3 个平台、布局 8，两个渲染入口均为 MeshVS，Spawn/Update HLSL 已剥离；附带自包含 glTF 网格和稳定 meta，导入测试核对 GUID/localId 与示例一致。Metal 输入 SPIR-V 编译仍不构成 Apple 设备运行证据。
- 材质通道批次：43 项 VFX 专项通过（`build/vfx-material-focused.xml`），12 项编辑器/场景通过（`build/vfx-material-editor.xml`）；Vulkan 均启用 Khronos 验证层，验证错误计数不增加且日志无验证错误。DX12 调试层的自定义材质像素与 Forward/Deferred 参数场景分别通过（`build/vfx-material-dx12-debug.xml`、`build/vfx-material-dx12-scene-debug.xml`）。
- 最终材质接口拒绝检查补齐后，2 项 DX12/Vulkan 像素与 4 项导入/暂停预览/场景专项复验通过（`build/vfx-material-final-pixels.xml`、`build/vfx-material-final-editor.xml`），Vulkan 验证错误为 0；接口拒绝可写/未知全局资源与当前不支持的材质常量数组。
- 扩展回归 1069 项：1064 通过、5 项符号链接环境跳过、0 失败，约 261 秒（`build/vfx-material-regression.xml`）。额外材质加载/Shader 导入兼容性 63 项全部通过（`build/vfx-material-import-regression.xml`）；该组需显式指定 `DXC_PATH`，避免测试自己的旧可执行文件探测逻辑误跳过实际可运行的导入检查。
- 自定义材质像素验证覆盖 CPU/GPU 四组动态 float4、独立材质常量与纹理/采样器、常量变化和早期快照保留；场景同时验证 CPU/GPU 自定义材质、Forward/Deferred、视图间修改 `User.Tint` 后读回旧/新常量；预览在暂停时从蓝色材质更新为红色，旧图像仍保留蓝色。
- 原生材质导入验证覆盖固定 HLSL ABI 与不兼容 Shader 拒绝、反射类型绑定、Shader/贴图依赖闭包、剥离源码的 VFX 包，以及移走 Shader/Material 源、`.meta` 和 ArtifactDB 后通过运行时 manifest 捕获材质。无效绑定/Shader 重导入保留旧工件，材质与 Shader 管理器卸载后捕获的字节码及常量仍有效。
- `DynamicMaterialSprite.vfx` 和 `StagedGpuEmitter.vfx` 三平台无源码包编译通过：布局 7、3 个平台，前者保留四组动态绑定和两组材质常量绑定。自定义材质仍作为独立 Shader/Material 依赖工件发布；Metal 仅有输入 SPIR-V 编译证据。Debug Editor/Game/工具已重建并通过 `--help` 检查，`SetVectorParameter` 出现在生成的 Script API；完整 C#/Lua Play 场景和桌面人工材质创作验收尚未完成。
- Sprite 批次：VFX 专项 43 项通过（`build/vfx-sprite-focused.xml`），编辑器/场景 9 项通过（`build/vfx-sprite-editor.xml`）。Windows Vulkan 使用本地 Khronos 验证层运行完整专项及 Forward/Deferred 场景，日志无验证错误，设备测试同时断言验证错误计数没有增加。DX12 调试层下生命周期与新像素测试 2 项通过（`build/vfx-sprite-dx12-debug.xml`）。
- 本批扩展回归 1014 项：1009 通过、5 项符号链接环境跳过、0 失败，约 207 秒（`build/vfx-sprite-regression.xml`）。覆盖原有资产/manifest/场景/RHI 回归，新增贴图生命周期测试共 20 项；Debug Editor/Game/编译工具及测试程序已重建，两个应用 `--help` 启动通过，`git diff --check` 通过。
- 新像素测试分别消费 CPU 冻结数据与 GPU SoA：验证 RGBA 贴图、四帧选择、负/越界帧截断、相邻帧插值、颜色/发光、五种混合、遮罩写深度及其后方遮挡、正交/透视软化、旋转、速度对齐和零速度回退。场景测试覆盖遮罩先于透明粒子、工作单元重映射、采样深度与附件同资源，以及原有多视图/停止/销毁生命周期。
- 资产/预览测试验证示例图集的稳定 GUID/localId、贴图移动后重导入、错误类型保留旧工件、运行时依赖闭包与编辑器数据库解析、删除源图后加载 native 贴图工件、资源管理器卸载后材质快照仍持有 RHI 纹理，以及 CPU 预览的真实贴图像素。
- `AnimatedSpriteAtlas.vfx` 与 `StagedGpuEmitter.vfx` 的三平台无源码包均重新编译通过：布局版本 6，保留 Sprite 设置，前者包含 2×2 图集/插值/旋转/速度对齐/软化示例。配套 `Textures/SpriteAtlas.tga.meta` 必须一同复制以保留引用。Metal 包仍只有输入 SPIR-V 编译证据；预览控件的桌面人工操作与三平台性能验收尚未完成。
- Debug 构建通过：`NLS_UI`、`NLS_Physics`、`NLS_VFX`、`NLS_VFXCompiler`、`NLS_Core`、`NLS_Render`、`NLS_Engine`、`NLS_Scripting`、`GameProject`、`LauncherProject`、`EditorProject`、`Editor`、`Game`、编译工具和测试程序。Game/Launcher 静态模块随场景渲染器 ABI 一起重建。
- VFX/物理/图文档/节点集成/着色器/GPU 专项：35 项通过，报告 `build/vfx-focused.xml`。新增三后端无源码包、损坏/取消回滚及混合系统不可变帧覆盖。
- 编辑器专项：导入回滚、图面板隐藏恢复及 GPU 异步预览 3 项通过。导入测试同时验证类型化组件引用、场景保存/恢复、相对工件路径、热重载、旧程序保留及参数覆盖保留。
- 普通场景专项：DX12、Vulkan 各 1 项通过，覆盖 Forward/Deferred、红色 CPU 与绿色 GPU 粒子的真实图像、捕获后变换、停止清空、不透明网格遮挡、pass 资源所有权及深度附件选择。报告 `build/vfx-scenes.xml`。
- 连续运行 GPU 模拟、场景与 Vulkan 提交测试 7 项通过，验证设备创建前的执行器初始化及 fixture 关闭顺序；报告 `build/vfx-isolation.xml`。
- 本轮扩展回归：975 项中 970 项通过，5 项因当前环境不能创建符号链接而跳过，没有失败。覆盖资产/路径/容器/撤销、PPtr、场景序列化、Prefab、VFX/PhysX、FrameGraph、完整 RenderFrameExecution、RHI 提交与 DX12/Vulkan；报告 `build/vfx-scene-regression.xml`，输出 `build/vfx-scene-regression.log`，约 189 秒。
- 场景物理专项：6 项通过，覆盖碰撞体发布与位姿/世界缩放同步、胶囊 Y 轴与旋转对齐、组件移除/对象销毁释放查询体、粒子自动碰撞，以及场景序列化恢复；报告 `build/vfx-physics-scene.xml`。
- 本轮复跑：VFX/物理/编辑器专项 35 项与编辑器/场景 5 项全部通过（`build/vfx-physics-focused.xml`、`build/vfx-physics-editor.xml`）。
- 本轮扩展回归：981 项中 976 项通过，5 项因当前环境不能创建符号链接而跳过，没有失败；报告 `build/vfx-physics-regression.xml`，输出 `build/vfx-physics-regression.log`，约 174 秒。回归需从仓库根目录运行，否则依赖引擎着色器路径的 DX12 用例会因工作目录而跳过。
- 深度排序批次（2026-10-16）：`NLS_VFX`、`NLS_VFXCompiler`、`NLS_Render`、`NLS_Engine`、`Editor`、`Game`、`NullusUnitTests`、`NullusVfxTests`、`NullusVfxEditorTests` 和 `NullusVfxCompile` Debug 构建通过；Editor/Game 的 `--help` 启动与 `git diff --check` 通过。构建记录 `build/vfx-sort-build.log`、`build/vfx-sort-editor-build.log`、`build/vfx-sort-final-build.log`。
- 深度排序专项：35 项通过（`build/vfx-sort-focused.xml`），在 Windows DX12/Vulkan 真设备验证 0/1/255/256/257/1025/9001 个存活粒子、非整幂容量、256 线程组边界、全局合并、同深度 64 位 ID 顺序、NaN/Inf、非单位世界变换和反向相机。CPU 上传与 GPU 数据均与 CPU 排序参考一致，源属性逐字节、ID 及间接参数保持原值；透明重叠粒子的像素读回随排序方向反转。
- 编辑器/场景 5 项通过（`build/vfx-sort-editor.xml`），覆盖启用排序的异步预览与 DX12/Vulkan Forward/Deferred；组件销毁后仍能读回早期视图的正确排序索引。新增渲染录制专项 1 项通过（`build/vfx-sort-recording.xml`），分别覆盖普通串行、工作单元串行与并行录制，确认计算分发、导出缓冲屏障和图形绘制顺序，且不进入 child command 路径。
- 排序批次扩展回归：982 项中 977 项通过，5 项符号链接环境跳过，0 失败，约 153 秒；报告 `build/vfx-sort-regression.xml`，输出 `build/vfx-sort-regression.log`。
- `SortedTranslucency.vfx` 示例已通过 `NullusVfxCompile --backend all`：运行时包布局版本 3、3 个平台包、每发射器 11 个内核、保留排序开关且无 HLSL（`build/SortedTranslucency.compiled.json`）。三平台包生成不等同于 Linux/Vulkan 或 macOS/Metal 真机验收。
- 动态范围/视图剔除批次（2026-10-16）：`NLS_VFX`、`NLS_VFXCompiler`、`NLS_Engine`、`Editor`、`Game`、编译工具与三个测试程序 Debug 构建通过。最终记录 `build/vfx-bounds-final-build.log`、`build/vfx-bounds-deploy-build.log`；Editor/Game `--help` 启动及 `git diff --check` 通过。
- 范围/剔除专项 37 项通过（`build/vfx-bounds-focused.xml`）：新增 CPU 范围运动/暂停/死亡/重置、六裁剪平面、尺寸和变换测试。DX12 RTX 5070 Laptop、Windows Vulkan Radeon 610M 验证空帧至 65,537 粒子、137/257/2053/65539 容量、跨扫描块、标量/Float2 尺寸、负尺寸、NaN/Inf、非均匀和负缩放、关闭剔除、稳定可见压缩及随后深度排序；GPU 范围和计数与 CPU 参考一致。一个 FrameGraph 模拟供三个视图，完全移出视锥的视图产生零实例和空图像。
- 编辑器/场景 5 项通过（`build/vfx-bounds-editor.xml`）：Forward/Deferred 的 CPU/GPU Sprite 使用独立视图参数；对象移动到视锥外后，存活数仍为 1、可见/绘制数为 0；组件销毁后早期视图数据仍可读回。GPU 预览验证暂停缩放后的图像与异步统计对应、计数守恒和 Clear 后统计释放；包围盒叠加的桌面人工视觉验收仍待进行。
- 本批扩展回归：984 项中 979 项通过，5 项符号链接环境跳过，0 失败，约 158 秒；报告 `build/vfx-bounds-regression.xml`，输出 `build/vfx-bounds-regression.log`。
- 当前 `SortedTranslucency.vfx` 的三平台无源码包已更新验证为布局版本 4、16 个内核、剔除开关保留（`build/SortedTranslucency.bounds.compiled.json`）；Metal 仍只有输入 SPIR-V 编译证据。
- 扩容/Simulation Stage 批次（2026-10-16）：VFX、编译器、Engine、Editor、Game、工具及三个测试程序 Debug 构建通过（`build/vfx-stages-editor-build.log`、`build/vfx-stages-final-build.log`）；Editor/Game `--help` 启动及 `git diff --check` 通过。
- 扩容与阶段专项 40 项通过（`build/vfx-stages-focused.xml`），新增 CPU 属性/ID 扩容与冻结副本、阶段资产/程序序列化、CPU 目标与阶段上下文诊断，以及阶段重排的撤销/重做与保存恢复。在 DX12 RTX 5070 Laptop、Windows Vulkan Radeon 610M 上从 3 槽逐步扩至 257 上限，五个阶段覆盖有序迭代、禁用、参数变更、阶段/迭代随机流、暂停、重置和阶段导致的死亡；每个粒子属性按 ID 与 CPU 指令解释参考比较，容差 `1e-5`。
- 设备测试同时验证：放弃尚未提交的扩容帧不推进容量/时钟；后续扩容、死亡与重置后，早期 GPU 属性/ID/计数/范围快照仍正确；增长与多阶段 ping-pong 后 FrameGraph 输出重新绑定并生成真实 Sprite 图像。
- 编辑器/场景 5 项通过（`build/vfx-stages-editor.xml`）：带五阶段的异步预览、扩容统计、缩放和关闭；DX12/Vulkan Forward/Deferred 通过带迭代阶段的混合场景播放。阶段页签的桌面人工交互与长时间压力仍待验收。
- `StagedGpuEmitter.vfx` 三平台无源码包编译通过：布局版本 5，3 个平台、3 个阶段、19 个内核、初始容量 16，阶段 HLSL 与图节点已剥离（`build/StagedGpuEmitter.compiled.json`）。Metal SPIR-V 编译仍不构成真机运行证据。
- 本批扩展回归：987 项中 982 项通过，5 项符号链接环境跳过，0 失败，约 158 秒；报告 `build/vfx-stages-regression.xml`，输出 `build/vfx-stages-regression.log`。
- NVIDIA GeForce RTX 5070 Laptop GPU / Windows DX12：129 个粒子在容量 137 的数据集中与 CPU 指令程序一致，压缩到 65 个后再次压缩计数正确；验证了 64 位 ID、填充线程边界、参数快照、连续帧生成/死亡/循环/Burst/容量丢弃、暂停、重置和自然消亡。
- AMD Radeon 610M / Windows Vulkan：同样的多帧生命周期与 CPU 按 ID 对照通过，全部属性容差为 `1e-5`。
- 两个后端均验证：一次 FrameGraph 模拟供两个视图绘制，像素读回存在粒子且视图变换独立；GPU 写入的间接分发和索引绘制参数真正执行，缓冲区子范围读回及重复轮询行为正确。
- Spawn/Update、BeginSpawn/SpawnParticles/UpdateParticles/AdvanceSpawn、Compact/Finish/Reset 和 Sprite VS/PS 均通过 DXIL/SPIR-V 编译。
- CPU/GPU/混合模板独立编译通过；`--backend all` 的混合模板包含 2 个发射器、3 个平台包且没有 HLSL。Metal 包的 SPIR-V 编译通过不代表 Apple 后端验收。
- 新增混合系统在两后端均通过 CPU/GPU 按 ID 对照、参数更改、暂停/停止/重置、多次视图消费、推进后早期 GPU 缓冲读回及代理销毁后读回；检测并拒绝漏掉的世界更新。
- Debug `Editor.exe --help` 与 `Game.exe --help` 启动通过；独立 Game 特效工程的资产发布和实际播放仍未验收。
- PhysX 默认固定归档下载/提取配置通过，不依赖机器专用 SDK 路径。
- 桌面编辑器的人工交互、长时间压力、性能对照、Linux 构建/设备及 macOS 构建/Metal 设备尚未验收。

## 下一步依赖与缺口

1. **世界与发布管线**：世界级批处理/Worker 调度、系统/数据接口依赖、共享 Simulation 的调度相位、运行时自动生成/挂接、完整独立应用打包及自动化发布场景。单 System 的固定步/预热已接入 CPU/GPU 同步计划。当前 CPU 更新由组件在 Game 执行，GPU 由 Render 命令按世界帧推进；新增渲染设备必须从初始化帧开始消费更新，检测到漏帧会拒绝推进。
2. **GPU 生产能力**：帧资源池、模拟存活压缩的稳定扫描、事件回读、多发射器依赖和异步队列调度。当前 Simulation Stage 仅遍历存活粒子；已支持动态 User 标量启用/迭代绑定；Grid/数据接口迭代、状态筛选、System/Emitter 计算的控制及自定义线程布局仍待提供者与编译器前置能力。扩容使用保守上界，精确 GPU 计数驱动的分配、内存池和缩容策略未完成。预览、世界快照、范围、视图剔除与排序会创建独立资源；现有基线只测量发射器本身，尚未覆盖这些每视图资源与多视图代价。GPU 发射时序仍由 CPU 调度，未按实际存活数间接分发渲染准备。现有范围服务于 Sprite 的保守视锥剔除，遮挡/LOD/预算及其他渲染器范围待扩展。透明排序已使用异步 GPU 范围读回覆盖同一组件和跨组件发射器，范围最多滞后四个提交帧，首帧/失败回退组件原点；进一步目标是合并同系统多范围拷贝为单次读回，并补齐自定义排序键、基数排序与池化调度。GPU 时间戳查询缺失，使设备侧耗时只能用同步帧近似。
3. **完整渲染**：可视化材质图、光照、材质顶点变形、运动矢量及 Ribbon/Light/Decal/Component/Volume Renderer。当前已支持 Sprite、最多八槽 Mesh、每槽材质与每视图 LOD，以及固定粒子输入 ABI 下的自定义 HLSL 片元材质及动态参数，仍不兼容任意表面 VS/PS；Mesh 的逐粒子 LOD、分段材质、完整 facing/pivot、跨槽透明顺序，MSAA 深度采样/解析、完整 Sprite facing/alignment、材质数组/可写数据接口与材质 PSO/常量资源池仍待完成。
4. **编译器与编辑器全集**：新版已有有序 SSA 模块栈、独立版本脚本/函数与静态开关、曲线/渐变、继承覆盖、多文档及整个 CPU/GPU System 预览；粒子 Simulation Stage 已接入新版创作链路。完整 Parameter Map 图节点、更多类型/命名空间/自定义属性、System/Emitter 与事件阶段、Custom HLSL、完整 Scratch Pad/时间线/Gizmo、全部继承升级操作和 UE 交互/视觉对照仍待完成。
5. **数据提供者**：PhysX 的凸包/高度场及 Cooking 工件、三角网格碰撞体与刚体模拟、动画/骨骼/Spline/VectorField、纹理/网格/邻域/地形/音频数据接口。盒/球/胶囊已通过场景碰撞体组件接入查询闭环，但完整物理仿真与网格/地形提供者仍有缺口。
6. **生产工具**：EffectType/预算/池化、Stateless、Data Channels、SimCache、烘焙、序列与调试分析，以及完整 C#/Lua 粒子 API 和行为验收。
7. **平台收口**：macOS PhysX 平台候选补丁仍需构建和真机查询验证；Metal 间接命令已写入后端但尚未在 Apple 编译/运行；Windows Vulkan 的结果不能代替 Linux 验收。

`NiagaraReference.json` 的 1051 项是带源码哈希的参考文件清单，尚不构成功能验收覆盖率。阶段与最终验收标准见 [NiagaraParticleSystem.md](NiagaraParticleSystem.md)。
