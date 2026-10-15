# 功能状态矩阵

本页汇总各领域的状态，作为跨章节索引。状态定义如下：

| 状态 | 含义 |
| --- | --- |
| 已实现 | 有可运行实现，并在文档中给出使用方式 |
| 部分实现 | 存在可用子集或明确基础，但缺少关键能力 |
| 规划中 | 有目标与依赖分析，尚无实现 |
| 长期愿景 | 方向明确，但依赖链尚未展开 |

状态描述的是**能力**，不是验证范围。某个能力“已实现”不代表所有平台都验证过；平台与证据边界在各章节单独说明。

## 状态总览

| 领域 | 状态 | 说明 | 详情 |
| --- | --- | --- | --- |
| 场景与对象模型 | 已实现 | GameObject、组件、Transform、层级 | [场景、GameObject 与组件](../scene/objects.md) |
| 组件生命周期 | 已实现 | Awake / Enable / Start / Update / FixedUpdate / LateUpdate / Destroy | [生命周期与激活状态](../scene/lifecycle.md) |
| Prefab | 部分实现 | 创建/实例化、Stage 保存、Override 已接入；Variant/Unpack 原生工作流的面板入口未齐 | [Prefab 工作流](../prefab/workflow.md) |
| 资产管线 | 已实现 | 资产数据库、工件清单、依赖跟踪、缩略图 | [Asset Browser 与导入](../assets/pipeline.md) |
| 模型导入 | 已实现 | glTF / FBX / OBJ，生成网格、材质、纹理与 Prefab | [模型、网格与纹理](../assets/models-textures.md) |
| 纹理 Mip 与压缩 | 部分实现 | RGBA8/16F、BC1/3/5/7；BC6H、ASTC、ETC2 仅为扩展点 | 同上 |
| 静态网格 LOD | 已实现 | 导入期构建，运行时选择 | [LOD 与 HLOD](../large-scenes/lod-hlod.md) |
| HLOD | 部分实现 | 代理机制存在，兼容性范围受限 | 同上 |
| 可见性管线 | 已实现 | 空间索引、多模式对照、稀疏句柄输出 | [可见性与遮挡剔除](../large-scenes/visibility-occlusion.md) |
| HZB 遮挡剔除 | 部分实现 | DX12 按能力保守启用，其它后端未验证 | 同上 |
| 资源驻留 | 部分实现 | 预算与去重机制存在，流式世界加载未实现 | [资源驻留与预算](../large-scenes/streaming.md) |
| 渲染路径 | 已实现 | 前向与延迟，FrameGraph 驱动 | [渲染概览](../rendering/overview.md) |
| 着色器工作流 | 已实现 | HLSL 单一源，ShaderLab 属性与变体 | [材质与 ShaderLab](../rendering/materials.md) |
| 图形后端 | 部分实现 | DX12 与 Vulkan 主线；Metal 原生实现；DX11 与 OpenGL 受限 | [图形后端与大场景特性](../rendering/backends.md) |
| 线程化渲染 | 已实现 | Game / Render / RHI 三通道，三种 RHI 线程模式 | [任务系统与线程模型](../architecture/jobs-threading.md) |
| JobSystem | 已实现 | 依赖、批处理、并行 for、后台任务与主线程延续 | 同上 |
| 反射 | 已实现 | MetaParser 生成注册、Inspector、序列化与脚本清单 | [反射与序列化](../architecture/reflection-serialization.md) |
| 对象图序列化 | 已实现 | 场景与 Prefab 共用格式 | 同上 |
| C# 脚本 | 已实现 | CoreCLR、热重载、调试附加 | [脚本系统概览](../scripting/overview.md) |
| Lua 脚本 | 已实现 | Lua 5.4、可选 LuaPanda 调试 | 同上 |
| 脚本 API 覆盖面 | 部分实现 | 覆盖对象、组件、相机、光源、UI、时间、输入；不含物理/音频/资源加载 | [脚本 API 参考](../scripting/api.md) |
| UI 运行时 | 已实现 | UIDocument、布局、输入路由、数据绑定、信号、本地化 | [UI 系统概览](../ui/overview.md) |
| UI 动画 | 部分实现 | 仅不透明度轨道 | 同上 |
| UI Designer | 已实现 | 编辑器内打开与编辑 UI 文档 | [UI Designer 与编辑器扩展](../ui/designer.md) |
| 编辑器脚本扩展 | 部分实现 | Inspector、工具窗口、菜单项；无自定义属性绘制器 | [编辑器脚本扩展](../architecture/editor-extensions.md) |
| 性能分析 | 已实现 | 共享标记、Tracy、Timeline Profiler | [性能分析与图形调试](../quality/profiling.md) |
| RenderDoc 集成 | 已实现 | 应用内抓取、运行器、抓取分析 | 同上 |
| 测试体系 | 已实现 | 行为测试、性能测试、反射冒烟 | [调试与测试](../quality/debugging.md) |
| 骨骼动画 | 部分实现 | 骨骼/蒙皮/片段作为工件保留，无运行时蒙皮组件 | [动画现状与规划](../animation/roadmap.md) |
| 物理 | 规划中 | 无刚体、碰撞、查询与角色控制器 | [物理现状与规划](../physics/roadmap.md) |
| 音频 | 规划中 | 有 `Audio` 工件类型，无播放实现 | [音频与特效现状及规划](../audio-vfx/roadmap.md) |
| 粒子与后处理 | 规划中 | 无粒子系统与后处理栈 | 同上 |
| 视频与时间线 | 规划中 | 无实现 | 同上 |
| 游戏 AI 基础 | 部分实现 | 原生状态机与下推自动机，未暴露给脚本 | [AI 与联网现状及规划](../ai-networking/roadmap.md) |
| 导航与行为树 | 规划中 | 无实现 | 同上 |
| 联网 | 规划中 | 无传输、会话、复制 | 同上 |
| 打包与分发 | 规划中 | 有构建动作与工件，无最终容器与安装包 | [Game 运行时与资产清单](../platform-build/runtime.md) |
| 地形与世界分区 | 长期愿景 | 依赖资源驻留与几何查询基础设施 | [资源驻留与预算](../large-scenes/streaming.md) |
| 2D 工具链 | 长期愿景 | 无精灵、2D 物理与 2D 动画 | 本页 |
| 可视化脚本 | 长期愿景 | 无节点图系统 | 本页 |
| 插件体系 | 长期愿景 | 有编辑器脚本扩展，无第三方插件分发模型 | [编辑器脚本扩展](../architecture/editor-extensions.md) |
| 移动端与 XR | 长期愿景 | 不在当前构建与后端目标内 | [平台支持矩阵](../platform-build/platforms.md) |

## 方向的共同依赖

多数规划项收敛到少数基础设施：

1. **几何查询**同时服务物理、AI 导航与编辑器选取；
2. **运行时蒙皮组件**是动画、角色控制与部分粒子的前置；
3. **平台后端抽象**决定音频、视频与移动端能否以一致方式接入；
4. **资源驻留与预算**决定地形、世界分区与流式加载是否可行；
5. **资产类型与序列化**是所有编辑器侧新功能的共同入口。

因此实现顺序通常不是按“功能吸引力”排，而是按依赖链排。

## 阅读建议

- 想知道怎么用已有能力：读对应章节的操作部分。
- 想评估某个功能能否交付：先看本页状态，再看对应章节的依赖段落。
- 想做实现规划：从“方向的共同依赖”出发，而不是从单个功能出发。

## 下一步

- [引擎概览](../start/overview.md)：能力边界总览。
- [模块结构](../architecture/modules.md)：代码组织方式。
