# 距离场

距离场首期支持 Windows DX12 上的静态三角网格、刚体变换、Mesh Distance Field、Global Distance Field 和 Scene View 调试显示。粒子碰撞、距离场阴影、AO、骨骼变形、Morph、材质顶点位移和地形尚未接入。

## 启用与导入

项目默认关闭生成。可在 Scene View 左上角 **Distance Fields** 菜单中启用 **Generate Mesh Distance Fields**，或在项目 `.nullus` 文件中设置：

```ini
generate_mesh_distance_fields=true
```

模型导入设置保存在源文件旁的 `.meta` 中：

| 设置 | 默认值 | 含义 |
| --- | --- | --- |
| `DISTANCE_FIELD_GENERATION` | `inherit` | `inherit` 继承项目，`enabled` 强制启用，`disabled` 关闭 |
| `DISTANCE_FIELD_RESOLUTION_SCALE` | `1` | 分辨率倍率，范围 `0.125` 到 `4` |
| `DISTANCE_FIELD_TWO_SIDED` | `false` | 为开放薄片生成无负值内部的表面距离 |

修改设置后重新导入模型。Editor 与命令行 Cook 共用 CPU 构建器，输入为应用导入缩放后的 LOD0，距离场不跟随渲染 LOD 切换。Opaque 与 Masked 三角形完整参与，Transparent 排除。开放或非流形网格会产生质量诊断；显式启用后的构建失败会中止发布并保留上一份有效结果。

MeshRenderer 的 **Affect Distance Field** 控制实例是否参与。**Mostly Static Distance Field** 选择静态缓存；移动、修改网格或关闭参与仍会更新旧边界覆盖的页面。

## 存储与运行时

模型场使用软件间接寻址、物理 `8³` Brick 和每轴 7 个有效间隔。三个 mip 分别构建，使用 R8 UNORM 编码及每 mip 解码范围；全正和全负饱和区域拥有不同常量标记。主 Artifact 带元数据和最低 mip，两个高 mip 独立加载。模型 manifest 一次提交网格与全部距离场产物，运行时资源包依赖包含这些独立 payload。

同一逻辑网格的材质分段共同生成距离场。完整且变换相同的分段实例共享联合场；单独移动、禁用或删除某个分段时，剩余实例使用各自的场，避免保留已删除几何。

每台设备共享模型 Atlas 与全局物理页面池，各自预算 128 MiB；模型上传每帧最多 8 MiB，按 DX12 对齐后的上传字节计费。最低 mip 优先于高 mip，预算不足时降级或标记分配失败。视图分别持有页表与 clipmap，Scene、Game 和预览视图互不共用视图状态。

全局场包含四级 clipmap，半径为 16、64、256、1024 场景单位，每级 20³ 个页面、140³ 个有效间隔。优先使用支持所需 UAV 操作的 R16F，否则使用 R32F 并减少页面容量。分页、对象分桶、分配回收和合成由 graphics queue 执行；相机按页滚动，静止且没有资源变化的视图不合成脏页。无效页面可回退到有效粗级。

非均匀缩放和镜像使用最小奇异值给出保守距离界；奇异或非有限变换排除。Vulkan 和 Metal 当前报告距离场功能不受支持。

## 调试与采样

Scene View 的 **Distance Fields** 菜单提供 Mesh/Global 模式及 Surface、Trace Steps、Signed Slice、Normals、Clipmaps、Pages、Residency 显示。Signed Slice 的高度为世界坐标 Y。Mesh Residency 以绿、黄、红标识实际采样的三个 mip；Global Residency 在水平切片中标识有效细级、粗级回退及无效页面。菜单显示对象、Brick、页面、上传、分配失败、CPU 准备时间和最近完成的 GPU 更新时间；**Rebuild Distance Fields** 强制失效页面并重试失败加载。

公共 GPU 资源由 `DistanceFieldGpuResources` 提供。`DistanceFieldSampling.hlsli` 暴露世界单位距离、梯度、体素尺度和有效性，调用者必须检查有效性后使用结果。

## 验证

在现有构建目录中构建 `NullusDistanceFieldTests` 和 `NullusDistanceFieldIntegrationTests`。前者覆盖构建精度、稀疏数据、体纹理上传、反射、场景更新及实际 DX12 合成；后者覆盖 Editor/Cook 一致性、项目设置缓存失效、失败发布保留及移走松散 Artifact 后的 `.npak` 独立读取。

实际 Editor 验收还应覆盖模型设置和重导入、调试模式切换、对象移动和删除、相机滚动、视图关闭重开，以及鼠标点击是否被正确的窗口接收。
