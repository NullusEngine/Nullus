# 动画现状与规划

动画已从导入工件扩展到运行时播放、GPU 蒙皮和 Animation Workbench。当前状态为**部分实现**：制作与播放链路存在，高阶角色能力和完整产品验收仍有缺口。

## 已实现的链路

- glTF/GLB 与 FBX 的骨架、蒙皮、多个动画 Take 和原生子资源。
- Animator 与 Skinned Mesh Renderer，CPU 姿态求值、四影响 GPU 蒙皮和动态包围盒。
- Clip、动画图、状态机、BlendSpace、BoneMask、曲线和 Notify。
- Workbench 预览、Undo/Redo、Working / Saved / Applied 状态及失败保留旧程序。
- Cook、冻结依赖和独立 Player 播放；Animator 控制进入反射与脚本清单。

使用步骤见[动画制作工作流](workflow.md)，验证入口见[动画验证](validation.md)。UI 的不透明度动画属于独立系统，不与角色姿态求值混用。

## 当前边界

首版采用串行求值、浮点 TRS 和每顶点四影响；动态包围盒通过 CPU 参考变形计算。多角色性能预算与平台验证需要单独记录。

Root Motion、Montage/Slot、Marker Sync、IK、Sockets、重定向、Morph Target、并行求值、动画 LOD 和 Compute Skinning 尚未完成。角色运动还依赖[物理与交互](../physics/roadmap.md)的查询或求解能力，不能仅凭动画图推断已有角色控制器。

## 验收范围

导入/编译测试、GPU 像素读回、打包自检、成品画面和鼠标交互分别验收。Metal、完整 Vulkan 延迟场景、Picking/Selection/DepthOnly 的蒙皮表现，以及 Autodesk SDK 配置不能由其他路径的通过结果推断。

- [模型、网格与纹理](../assets/models-textures.md)：导入基础。
- [功能状态矩阵](../roadmap/index.md)：跨领域状态。
