# 距离场

距离场首期支持 Windows DX12 上的静态三角网格、刚体变换、Mesh Distance Field、Global Distance Field 和 Scene View 调试显示。粒子碰撞、距离场阴影、AO、骨骼变形、Morph、材质顶点位移和地形尚未接入。

## 启用与导入

项目默认关闭生成。可在 Scene View 的 **Draw Mode > Distance Field Settings** 菜单中启用 **Generate Mesh Distance Fields**，然后点击 **Build Missing / Outdated** 为当前场景使用的模型补建过期或缺失的数据，或在项目 `.nullus` 文件中设置：

```ini
generate_mesh_distance_fields=true
```

模型导入设置保存在源文件旁的 `.meta` 中：

| 设置 | 默认值 | 含义 |
| --- | --- | --- |
| `DISTANCE_FIELD_GENERATION` | `inherit` | `inherit` 继承项目，`enabled` 强制启用，`disabled` 关闭 |
| `DISTANCE_FIELD_RESOLUTION_SCALE` | `1` | 分辨率倍率，范围 `0.125` 到 `4` |
| `DISTANCE_FIELD_TWO_SIDED` | `false` | 为开放薄片生成无负值内部的表面距离 |

距离场设置可单独 Apply，随后在后台构建；普通模型导入设置仍走正常导入。**Build Missing / Outdated** 直接复用已发布的 LOD0，不重新解析模型或导入材质、贴图、渲染 LOD 和 Prefab。旧资产缺少构建描述时只迁移一次所需元数据。构建基于最后发布的几何，尚未应用的普通导入设置不会被标记为已导入。仅开启项目开关或切换显示模式不会为旧资产补建距离场；没有数据时视图会显示提示。

Editor 与命令行 Cook 共用 CPU 构建服务，输入为应用导入缩放后的 LOD0，距离场不跟随渲染 LOD 切换。Opaque 与 Masked 三角形完整参与，Transparent 排除。开放或非流形网格会产生质量诊断；构建失败保留已发布的有效产物，过期或取消的任务无法覆盖新版本。

MeshRenderer 的 **Affect Distance Field** 控制实例是否参与。**Mostly Static Distance Field** 选择静态缓存；移动、修改网格或关闭参与仍会更新旧边界覆盖的页面。

## 存储与运行时

模型场使用软件间接寻址、物理 `8³` Brick 和每轴 7 个有效间隔。三个 mip 分别构建，使用 R8 UNORM 编码及每 mip 解码范围；全正和全负饱和区域拥有不同常量标记。主 Artifact 带元数据和最低 mip，两个高 mip 独立加载。模型 manifest 一次提交网格与全部距离场产物，运行时资源包依赖包含这些独立 payload。

同一逻辑网格的材质分段共同生成距离场。完整且变换相同的分段实例共享联合场；单独移动、禁用或删除某个分段时，剩余实例使用各自的场，避免保留已删除几何。

导入时记录 `MeshDistanceFieldSource` 构建描述，包括几何摘要、逻辑分组和材质参与规则。独立构建先完成全部逻辑网格的最低 mip，再补充备用分段和精细数据；单分段联合场复用同一份计算结果。交互导入按同一几何版本提交网格和最低 mip，精细 mip 在发布后继续生成。Player 只加载产物。

`Library/DistanceFieldCache` 按构建器版本、几何、有效设置、查询后端和 mip 保存结果；进程内复用 BVH。重复生成不执行距离查询，分辨率变化复用几何加速结构，取消后可以续建。三个 mip 仍独立计算。源模型几何变化时只重建受影响的输入，实例移动不执行 CPU 烘焙。

离线查询优先使用配置的 Embree 4 SDK；未配置时使用内置 SAH BVH。Windows SDK 的 `include`、`lib`、`bin` 可通过 `NLS_EMBREE_ROOT` 指定；默认 oneTBB 版本需包含 `tbb12.dll` 和 `tbb12.lib`。构建工具的运行库清单会部署依赖及许可文件。自带内部任务调度的 SDK 可设置 `NLS_EMBREE_NEEDS_TBB=OFF`。后台线程池、距离带裁剪、符号射线提前结束和相邻 Brick 共享样本均保留现有质量标准。

每台设备共享模型 Atlas 与全局物理页面池，各自预算 128 MiB；模型上传每帧最多 8 MiB，按 DX12 对齐后的上传字节计费。最低 mip 优先于高 mip，预算不足时降级或标记分配失败。视图分别持有页表与 clipmap，Scene、Game 和预览视图互不共用视图状态。

全局场包含四级 clipmap，半径为 16、64、256、1024 场景单位，每级 20³ 个页面、140³ 个有效间隔。优先使用支持所需 UAV 操作的 R16F，否则使用 R32F 并减少页面容量。分页、对象分桶、分配回收和合成由 graphics queue 执行；相机按页滚动，静止且没有资源变化的视图不合成脏页。无效页面可回退到有效粗级。

非均匀缩放和镜像使用最小奇异值给出保守距离界；奇异或非有限变换排除。Vulkan 和 Metal 当前报告距离场功能不受支持。

## 调试与采样

Scene View 的 **Draw Mode** 菜单提供 Mesh/Global 模式，**Distance Field Settings** 提供 Surface、Trace Steps、Signed Slice、Normals、Clipmaps、Pages、Residency 显示。Trace Steps 只给命中的射线着色，未命中区域保持深色；色相按对数步数从蓝连续过渡到红，亮度保留 Surface 的法线明暗，便于看清形状。视图左下角显示色标，**Red at steps** 可调整红色对应的步数（默认 64，范围 8-512）。Signed Slice 的高度为世界坐标 Y。Mesh Residency 以绿、黄、红标识实际采样的三个 mip；Global Residency 在水平切片中标识有效细级、粗级回退及无效页面。菜单显示对象、Brick、页面、上传、分配失败、CPU 准备时间和最近完成的 GPU 更新时间；**Rebuild Global Pages** 强制失效页面并重试失败加载，不负责生成模型场。

**Build Coarse Preview** 只生成最低 mip；**Force Rebuild Model Fields** 强制重新计算模型场。构建进度与取消位于 Background Tasks，菜单显示后端、BVH/最近点/符号查询耗时和构建、缓存、已有效的 mip 数。增量发布会合并短批次，保留无关的网格、材质、贴图与 Prefab 条目。

Mesh DF 可视化先将射线裁剪到场景体积范围，再用保守距离下界跳过不可能提供最近距离的模型。距离场加载、上传和页面更新结束后，静止视图复用已绘制图像；相机、场景、调试设置或距离场资源 revision 变化会重新绘制。面板的 **View image** 显示当前为 `cached` 或 `updating`，GPU 时间表示最近一次实际更新的耗时。

公共 GPU 资源由 `DistanceFieldGpuResources` 提供。`DistanceFieldSampling.hlsli` 暴露世界单位距离、梯度、体素尺度和有效性，调用者必须检查有效性后使用结果。

## 验证

相同 112 个 Sponza 距离场对象、1280×720、RTX 5070 Laptop GPU、DX12 debug layer 下，20 个稳定帧的模型场可视化平均 GPU 时间从 33.3 ms 降至 2.56 ms。该数据只计距离场 GPU pass，不代表完整 Editor 帧时间；静止图像复用另行减少重复绘制。优化前后的表面轮廓覆盖重合度为 99.94%。

2026-11-04 的本地 Sponza 测量（Ryzen 9 8945HX、Release、Embree 4、430 份唯一几何、1290 个 mip）中，全质量强制重算耗时 136.2 秒，首份粗场发布耗时 0.42 秒；相同设置再次构建耗时 0.71 秒，模型读取、BVH 构建、距离查询和产物发布均为零。该数据用于说明当前测试资产的行为，首次粗场发布不代表整个场景已完成构建。

在现有构建目录中构建 `NullusUnitTests`，用 `--gtest_filter="DistanceField*.*"` 运行距离场用例。
覆盖构建精度、稀疏数据、体纹理上传、反射、场景更新及实际 DX12 合成，以及 Editor/Cook 一致性、项目设置缓存失效、失败发布保留及移走松散 Artifact 后的 `.npak` 独立读取。

实际 Editor 验收还应覆盖模型设置和重导入、调试模式切换、对象移动和删除、相机滚动、视图关闭重开，以及鼠标点击是否被正确的窗口接收。
