# 最小完整示例

本例把构建、场景、资产、脚本与 Prefab 串成一次可复查的练习。完成后得到一个保存过的场景、一个持续旋转的对象，以及一个可重复实例化的 Prefab。无需下载示例资源。

**状态：使用已有能力。** 下列操作依赖 Windows 编辑器及已启用的 C# 运行时。源码核对、代码编译、运行时测试与完整桌面操作的证据分开记录在[调试与测试](../quality/debugging.md#本轮文档验收范围)。

## 准备工程与场景

1. 按[环境准备与构建](../getting-started/build.md)生成 `Editor`、`Launcher`。
2. 按[第一个项目](../getting-started/first-project.md)创建 `DocsDemo`，新建基础场景，把 Cube 放到 `(0, 0, 5)`，相机放在原点。
3. 保存为 `Assets/Scenes/FirstScene.scene`。后续 Prefab 操作要求场景已经保存。
4. 创建并挂载快速入门中的 `Rotator.cs`，进入 Play，确认 Cube 旋转且 Console 出现 `Rotator awake`，然后 Stop。

## 验证一次源资产导入

在磁盘上的 `Assets/Models` 新建纯文本文件 `Triangle.obj`，写入以下内容。它定义三个顶点和一个面，顶点已经位于相机前方：

```obj
o DocsTriangle
v -1.0 -1.0 5.0
v 0.0 1.0 5.0
v 1.0 -1.0 5.0
f 1 2 3
```

1. 回到编辑器，等待 Asset Browser 出现该文件并完成导入。当前 `Import Here...` 未启用，直接把源文件复制到 `Assets/` 是可用入口。
2. 展开模型的生成子资产，确认有网格、材质与 Prefab；把生成 Prefab 拖到 Hierarchy 空白处。
3. 确认实例 Transform 没有再次向 `+Z` 平移，因为 OBJ 中的顶点已经包含位置。可先关闭 Cube，避免两个物体重叠。
4. 在 Scene View 中观察三角形。单面网格从背面可能不可见；默认材质不包含纹理，这是本例的预期。
5. 修改 OBJ 的一个顶点并保存，必要时使用 Asset Browser 的 `Reimport`，观察重新导入日志，再保存场景。

模型的生成 Prefab 为只读。导入失败时，按[资产管线](../assets/pipeline.md)检查日志和依赖，不要手改 `Library/` 工件。

## 保存与复用 Prefab

1. 在编辑态重新启用 Cube，将它命名为 `RotatingCube`，保存场景。
2. 打开 `Assets/Prefabs`，把 Hierarchy 中的 `RotatingCube` 根对象拖进目录。
3. 把生成的 Prefab 再拖到场景中，把第二个实例移到侧面，然后保存场景。
4. 双击 Prefab 进入 Stage，改一个可见属性后按 `Ctrl+S`。切换 Stage 前显式保存。
5. 回到实例检查 Override 的 Apply/Revert；当前可操作入口与 Variant/Unpack 的边界见[Prefab 工作流](../prefab/workflow.md)。

## 可选：添加界面

按 [UI Designer](../ui/designer.md)创建 `.ui` 文档、添加 Panel 并保存，然后把它赋给 `UICanvasComponent` 的 Document 字段。先用 Screen Overlay 验证布局，再考虑相机与世界空间路径。设计器只暴露文档格式的一部分字段。

## 完成标准与排错

| 检查点 | 预期结果 | 失败时先检查 |
| --- | --- | --- |
| 工程重开 | 场景和资源引用恢复 | 保存路径、`.meta` 是否保留 |
| 脚本 Play | 对象旋转，Console 有 Awake 日志 | 编译错误、对象行挂载与激活状态 |
| Stop | 回到编辑态 Transform | Play 沙箱与编辑场景的区别 |
| OBJ 导入 | 生成网格、材质和 Prefab | 源文件编码、面索引、Console |
| Prefab | 两个实例引用同一资产 | 创建前是否保存场景、是否已连接 |
| UI（可选） | Panel 可见 | Document 引用、尺寸与渲染模式 |

这些检查点构成桌面验收清单。仅编译脚本或通过子系统测试，不代表已经逐项操作完成。

## 下一步

- [功能状态矩阵](index.md)：把本例用到的能力与未来方向对应起来。
- [脚本 API 参考](../scripting/api.md)：扩展对象行为前确认签名和语言支持。
