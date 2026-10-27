# 引擎概览

Nullus 是一个面向实时 3D 创作的 C++20 原生引擎。它把运行时、项目启动器、可视化编辑器、资产导入、场景序列化与游戏脚本放在同一条主链路上：同一个运行时既驱动编辑器视口，也驱动最终的游戏进程。

这份手册按“先做出结果，再理解原理”的顺序组织。如果你第一次接触 Nullus，先读[第一个项目](../getting-started/first-project.md)，再回到本页了解能力边界。

## 引擎能做什么

| 领域 | 当前能力 |
| --- | --- |
| 场景 | `Scene` / `SceneManager` / `GameObject` / 组件的组件化场景系统，反射驱动的对象图序列化 |
| 编辑器 | Hierarchy、Inspector、Scene View、Game View、Asset Browser、Console、Profiler、Project Settings、UI Designer |
| 资产 | 项目级资产数据库、模型与纹理导入、工件清单、缩略图与异步预览 |
| 渲染 | 显式多后端 RHI、FrameGraph 驱动的前向与延迟路径、PBR 材质、ShaderLab |
| 脚本 | C# (CoreCLR) 与 Lua 5.4 两套后端，共享同一份脚本 API 清单 |
| 大场景 | 空间索引、可见性收集、LOD 与 HLOD、资源驻留预算、DX12 HZB 遮挡剔除 |
| 任务系统 | 原生 JobSystem 与带优先级、命名通道、work stealing 的 TaskGraph |
| 动画 | Animator、GPU 蒙皮、动画图、状态机、BlendSpace 与 Animation Workbench |
| 特效与查询 | CPU/GPU 粒子、模块栈编辑器、Sprite/Mesh 绘制与静态 PhysX 场景查询 |
| 打包 | Build Profile、目标 Cook、`.npak` 内容包和桌面 Player 组装 |
| 平台 | Windows / Linux / macOS 构建目标，Windows DX12 与 Linux Vulkan 为已验证的运行时主线 |

更细的能力清单与验证状态见[功能状态矩阵](../roadmap/index.md)。

## 当前能力边界

动画、粒子、静态物理查询和桌面打包已有实现，完整交付范围见各章。以下能力仍未完成：

- 动力学刚体、角色控制器、音频和视频播放；
- Root Motion、IK、重定向、完整粒子模块/数据接口及通用后处理栈；
- 导航网格、行为树、多人联网与复制；
- 安装器、自动更新、内容分发服务与移动端交付；
- 2D 工具链、游戏逻辑可视化脚本与插件市场。

材质、动画和粒子编辑器的节点图是专用创作工具，不等于已有游戏逻辑可视化脚本。各章分别说明实现、自动测试、设备与鼠标交互验收范围。

## 适合谁

- **游戏开发者**：用编辑器搭场景、导入资产、写 C# 或 Lua 脚本，见[使用编辑器](../editor/workspace.md)与[游戏逻辑与脚本](../scripting/overview.md)。
- **技术美术**：写 ShaderLab、调材质、管纹理与网格构建管线，见[材质与 ShaderLab](../rendering/materials.md)与[模型、网格与纹理](../assets/models-textures.md)。
- **引擎开发者**：扩展运行时模块、渲染后端或编辑器，见[扩展与引擎架构](../architecture/modules.md)。
- **从其他引擎迁移的团队**：先读[核心概念与术语](concepts.md)，那里给出了与 Unity、Unreal 的对照。

## 版本与兼容

引擎尚未发布稳定版本号，`CMakeLists.txt` 中的工程版本为 `1.0.0`，用于构建和 Player/资源包兼容检查，不代表稳定 API。仓库当前不承诺源码 API 兼容性，升级时请阅读提交记录与[功能状态矩阵](../roadmap/index.md)。

脚本 API 有独立的版本概念：`ScriptApi.json` 当前 schema 版本为 `2`。原生脚本 ABI 版本不匹配时运行时会拒绝加载，详见[脚本 API 参考](../scripting/api.md)。

## 许可

Nullus Engine 使用自定义 Source-Available License：允许查看和修改源码、商业使用以及发布使用 Nullus Engine 制作的游戏；禁止分发引擎源码或使用本项目制作竞争引擎。完整条款见仓库根目录的 `LICENSE`，贡献流程见 `CONTRIBUTING.md` 与 `CLA.md`。

## 下一步

1. [环境准备与构建](../getting-started/build.md)：把源码变成可运行的编辑器。
2. [第一个项目](../getting-started/first-project.md)：创建项目、搭一个场景、挂上第一个脚本。
3. [核心概念与术语](concepts.md)：把 Unity 或 Unreal 的既有认知映射到 Nullus。
