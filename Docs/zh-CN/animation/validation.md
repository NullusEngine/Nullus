# 动画验证

本页提供可复现的验证入口。实现、自动测试、GPU 读回、成品运行和桌面交互需要分别验收，一次测试通过不能代替其余检查。

## 前提

按[环境准备与构建](../getting-started/build.md)配置现有构建目录，并启用 `NLS_BUILD_TESTS=ON`。GPU 检查需要实际可用的图形设备和对应后端。示例位于 `Templates/Animation`，复制到项目时保留 `.meta` 文件。

## 自动测试

```powershell
cmake --build build --config Debug --target NullusAnimationTests NullusAnimationImportTests NullusAnimationEditorTests NullusAnimationGpuTests
ctest --test-dir build -C Debug -R "^NullusAnimation" --output-on-failure
```

| 目标 | 主要检查 |
| --- | --- |
| `NullusAnimationTests` | TRS 与四元数、采样、混合、状态机、事件、二进制往返、程序替换失败回滚 |
| `NullusAnimationImportTests` | glTF/GLB、FBX、多 Take、采样率、独立动画绑定、重导入身份与资源依赖 |
| `NullusAnimationEditorTests` | 文档 Undo/Redo、场景更新、Prefab、共享 Animator、延迟绑定与冻结依赖 |
| `NullusAnimationGpuTests` | GPU 蒙皮位置和法线、对象绑定、延迟提交中的资源保留与像素读回 |

记录实际执行的后端、设备、配置、退出状态和跳过项。GPU 专项目标不能替代完整场景验收。

## 成品 Player

1. 用 `Templates/Animation/AnimationSample.scene` 作为项目启动场景，导入模型、动画图、Clip、BlendSpace 与 BoneMask。
2. 按[Player 打包与分发](../platform-build/packaging.md)生成成品；运行 Player 的 `--package-self-test`，检查容器、RuntimeManifest 和启动场景。
3. 在成品目录运行以下命令；若产品使用其他可执行文件名，替换 `Game.exe`。

```powershell
.\Game.exe --animation-validation-output forward.png --animation-validation-time 0.25 --animation-validation-forward
.\Game.exe --animation-validation-output deferred.png --animation-validation-time 0.75
```

检查读回图和退出结果，确认启动无需访问项目模型与动画创作源文件。测试场景必须包含可见的动画角色；普通场景截图不构成蒙皮正确性的证明。FBX 的不同 Take、非均匀缩放和共享 Animator 的多个网格应单独覆盖。

## 编辑器交互

在可操作的桌面中使用鼠标打开 Animation Workbench、切换 Take、拖动时间轴、编辑状态与连线、Undo/Redo、保存、关闭重开，再验证 Apply 更新场景实例。编译失败应保留上一份可运行程序。

Picking、Selection Outline 和 DepthOnly 需要逐项检查；动画轮廓读回不能代替这些 Pass 的验收。Autodesk FBX Reader 需要在启用对应 SDK 的构建中单独测试，Assimp 结果不覆盖该配置。

## 当前边界

首版串行求值，每顶点四个骨骼影响，使用浮点 TRS；动态包围盒通过 CPU 参考变形计算。多角色性能预算、Metal 设备执行、Vulkan 完整延迟场景和各 DCC 的复杂 FBX 组合需要独立验收。

Root Motion、Montage、IK、重定向、Morph Target、动画 LOD 和 Compute Skinning 尚不属于当前工作流。
