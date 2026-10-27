# 动画制作工作流

Nullus Animation 使用 CPU 骨骼姿态求值与 GPU 顶点蒙皮。glTF/GLB 和 FBX
导入结果共用 Skeleton、SkinBinding、AnimationClip 与 SkeletalMesh 数据。

## 制作流程

1. 将模型与动画源文件放入项目 Assets。FBX Reader 沿用模型导入器的设置；
   显式选择 Autodesk Reader 时，需要启用并提供 Autodesk FBX SDK。
2. 在 Animation Workbench 中打开角色源文件，选择 Clip/Take，播放、暂停或拖动时间轴。
3. 使用 Create Clip from Preview Take 创建 `.animclip`，编辑裁剪、曲线和 Notify。
4. 创建 `.blendspace`、`.bonemask`、`.animgraph`，通过 GUID 与子资源键选择依赖。
5. Compile / Preview 使用当前工作文档；Save 写创作数据；Apply 发布编译成功的程序。
   已运行的 Animator 保留 Applied 程序，重新导入 Saved 数据不会直接替换它。
6. 将动画图绑定到角色 Animator。Skinned Mesh Renderer 引用蒙皮网格和 Animator，
   同一骨架的多个身体或服装网格可以共享 Animator。

Animation Preview 使用独立场景与相机，经过同一 Animator、Pose 与蒙皮提交路径。
预览 Notify 写入事件列表；游戏中的 Notify 在场景动画 Finalize 阶段派发。

## 资源与脚本

持久引用采用 `源 GUID#子资源键`，例如 `GUID#animation:animation/0`。
模型生成的骨架、绑定和原始 Clip 保持为导入子资源。可编辑资产使用
`.animclip`、`.animgraph`、`.blendspace`、`.bonemask`，Cook 后写入原生资源容器。
AnimationProgram 包含已验证的依赖快照，Player 无需读取创作图或模型源文件。

Animator 的脚本接口包括 `Play`、`Pause`、`Stop`、`Seek`、`SetLooping`、
`SetPlaybackSpeed`、`SetFloat`、`SetBool`、`SetVector`、`SetTrigger` 和 `GetTime`。
接口通过现有反射及 ScriptApi 清单提供给 C# 与 Lua。

FBX 支持 ASCII/Binary、线性蒙皮、多个 Take、Take 筛选及采样率覆盖。
元数据设置包含 `MODEL_FBX_READER`、`MODEL_IMPORT_SKELETON`、
`MODEL_IMPORT_ANIMATIONS`、`MODEL_ANIMATION_SAMPLE_RATE`、
`MODEL_ANIMATION_TAKES`（以分号分隔）和 `MODEL_ANIMATION_TARGET_SKELETON`。
动画独立文件绑定目标时，会验证骨骼名称/路径与层级；不同骨架的重定向另行建设。

## 验证与当前边界

测试目标为 `NullusAnimationTests`、`NullusAnimationImportTests`、
`NullusAnimationGpuTests`、`NullusAnimationEditorTests`。GPU 测试提供 DX12
和 Vulkan 像素读回检查，覆盖权重、位置、法线及保留资源后的绘制。
测试目标的存在不代表所有设备或后端已经通过验收。

示例位于 `Templates/Animation`，包括合成双骨骼 glTF、双 Take ASCII FBX、
动画图、BlendSpace、BoneMask 与带脚步事件的 Clip。
`AnimationSample.scene` 可直接用于 Player；图的初始 Speed 为 1，
Preview Parameters 可调整 Speed、IsGrounded、UpperBodyWeight 或发送 Jump Trigger。

复现步骤、GPU/打包检查和交互验收要求见[动画验证](validation.md)。

首版使用四骨骼影响与浮点 TRS，串行求值。动态包围盒目前使用 CPU 参考变形计算，
蒙皮几何与 palette 的 GPU 数据独立保留到提交完成。多角色预算、压缩、
Root Motion、Montage、IK、重定向及 Morph Target 属于后续阶段。
