# Animation P0 / P1 验证记录

日期：2026-10-27。此记录区分实现、自动验证与交互验收。

## 交付内容

- `NLS_Animation` / `NLS_AnimationCompiler`，共享的 TRS、采样、混合、图程序与实例。
- glTF/GLB 与 FBX Reader，Skeleton、Skin、Clip、SkeletalMesh 原生子资源。
- Animator / SkinnedMeshRenderer，反射、脚本控制、场景及生成 Prefab。
- 四影响 GPU 蒙皮、保留的姿态/缓冲、动态包围盒、Static/Skinned Shader 变体。
- AnimGraph、状态机、BlendSpace、BoneMask、曲线、Notify 和动画工作台。
- Working / Saved / Applied、Undo/Redo、失败保留旧程序、Cook 和打包依赖。
- `Templates/Animation` 示例角色、FBX、多 Take、图资源和可运行场景。

## 自动验证

| 目标 | 通过数 | 验证内容 |
| --- | ---: | --- |
| NullusAnimationTests | 23 | TRS/四元数、插值、混合、图编译、状态机、事件、二进制往返和程序替换失败回滚 |
| NullusAnimationImportTests | 12 | glTF/GLB、sparse、编码缓冲路径、ASCII/Binary FBX、多 Take、采样率、厘米/米对照、独立动画绑定、重导入身份、歧义骨骼拒绝、包内 Shader、旧 SPIR-V 反射兼容 |
| NullusAnimationEditorTests | 11 | 文档 Undo/Redo、关闭文档、冷启动规划、场景更新、Prefab、共享 Animator 引用、延迟资源绑定、禁用组件、示例包围盒、冻结依赖 |
| NullusAnimationGpuTests | 4 | DX12/Vulkan 像素读回、非零 joint、UNorm 权重、位置/非均匀缩放法线、stock 属性顺序、延迟绘制资源保留、对象矩阵/实例索引/push constant 绘制、内置延迟光照 Shader 布局 |

GPU 设备：DX12 使用 NVIDIA GeForce RTX 5070 Laptop GPU；Vulkan 使用 AMD Radeon 610M。

Editor、Game、NullusBuild 和上述专项目标在独立 Debug 构建目录构建。验证没有替换正在运行的其他 Editor，也没有提交或暂存工作区改动。

## 导入和打包

示例冷启动预热成功导入 6 / 6 个模型与创作资产；共享 Cooker 成功构建这 6 个资产。场景通过现有场景打包通道处理；共享 Cooker 对 `.scene` 和 README 的 unsupported 提示不代表动画资产失败。

Windows DX12 示例 Player 打包成功，`--package-self-test` 验证资源包、16 项 RuntimeManifest 引用与启动场景。模型、动画图、Clip、BlendSpace、BoneMask 的项目源文件不随项目资源包作为运行时依赖交付。

打包 Player 通过 GPU 读回验证：

- Forward：0.25 秒姿态，动画轮廓蓝色覆盖率约 0.114，PASS。
- 默认延迟渲染：0.75 秒姿态，动画轮廓蓝色覆盖率约 0.114，PASS。
- FBX `Move` Take：0.5 秒姿态，Forward 轮廓蓝色覆盖率约 0.132，PASS。
- Windows Vulkan Player：Forward 0.25 秒姿态，轮廓覆盖率约 0.114，PASS。

复现入口：

```powershell
.\AnimationValidation.exe --animation-validation-output forward.png --animation-validation-time 0.25 --animation-validation-forward
.\AnimationValidation.exe --animation-validation-output deferred.png --animation-validation-time 0.75
```

验证命令复用渲染器的 GPU 读回通道；不会把普通截图或进程响应计数当作播放证据。

## 尚未完成的验收

- 当前桌面由 Windows LockApp 占据，无法取得鼠标焦点；鼠标连线、状态编辑、拖动时间轴、关闭重开和 Apply 的完整实操验收仍待解锁后进行。
- Picking、Selection Outline 与 DepthOnly 已接入蒙皮变体；现有专项 GPU 测试和 Player 轮廓读回不等于这些 Pass 的逐项像素验收。
- Vulkan 默认延迟渲染场景验收失败：GBuffer 与光照管线已成功创建和捕获，实际提交出现 `vkQueueSubmit failed with VkResult=-4`（`VK_ERROR_DEVICE_LOST`）。专项对象缓冲/实例索引/蒙皮直接绘制及完整 Forward Player 均通过，尚未定位延迟场景设备丢失的根因。Vulkan Editor 和全部 Pass 的验收仍未完成。
- Autodesk SDK 配置在本机未启用；其适配代码没有在 SDK 配置下编译或验收。当前 FBX 基准使用 Assimp。
- FBX 已测样例不能代表所有 DCC 的 pivot、Pre/Post Rotation、动画层、轴设置、多材质拆分和异常文件组合；这些组合仍需补充资产验收。

因此当前交付口径为：P0/P1 首版实现与专项自动验证已完成，完整产品验收尚未完成。

## 当前性能与功能边界

首版串行求值，四影响、浮点 TRS；动态包围盒通过 CPU 参考变形计算。角色数量预算与性能基准尚未验收。

Root Motion、Montage/Slot、Marker Sync、IK、Sockets、Retargeting、Morph Target、并行求值、动画 LOD、动态阴影、Velocity 与 Compute Skinning 延续计划中的后续阶段边界。
