# 物理现状与规划

当前状态为**部分实现**：3D 使用 PhysX 5.6.1，已实现刚体与独立形状所有权、复合碰撞、基础动力学、查询及碰撞事件聚合；2D 已接入 Box2D 3.2.0，提供独立世界、基础刚体、碰撞体和原生查询。完整资源、角色、约束、脚本 API 与编辑器验收仍未完成。

## 静态碰撞体

1. 给场景对象添加 **Physics / Collider** 组件。
2. 选择 Box、Sphere、Capsule 或 Cylinder，设置 Half Extents、Radius、Half Height、Center、Layers、Collision Mask、Friction、Restitution 和 Trigger。
3. 运行场景。场景在组件更新前同步查询几何，移动、禁用或删除对象后相应查询体也应更新或释放。

Half Extents 是盒子的半尺寸。3D Collider 关联自身或最近同维度父级 Rigidbody；嵌套刚体建立独立 Body，没有 Rigidbody 时生成静态体。每个 Shape 保留独立 Collider 身份。Rigidbody 支持质量、重力、线速度、角速度、阻尼、CCD、锁轴和睡眠配置；编辑这些属性保持运行时 Body 身份。2D Trigger 已影响求解与查询，完整 2D Enter/Stay/Exit 脚本事件仍待接入。

3D Rigidbody 的 Constraints 使用位掩码：位置 XYZ 为 1/2/4，旋转 XYZ 为 8/16/32，63 锁定全部自由度。Auto Mass Properties 默认开启，由实际形状计算质心和惯量；关闭后使用 Center Of Mass、Inertia Tensor 和 Mass Frame Rotation。手动值以刚体坐标系的米和 kg*m² 表示，Transform 缩放只改变几何，不缩放这些手动值。新增字段支持保存、克隆和旧场景缺省加载，实际 Inspector 点击仍待验收。

Start Awake 控制初始状态，Sleep Threshold 控制自动睡眠阈值。C++ 的 WakeUp、Sleep、ApplyTorque、AddForceAtPosition 与原有力命令在固定步安全边界按序消费一次；IsSleeping 读取最近提交的状态。力、力矩及位置施力均支持 Force、Acceleration、Impulse、VelocityChange。形状重建保留当前睡眠状态，不会让曾被唤醒的刚体因初始配置再次丢失速度。新增反射元数据不代表 C# / Lua 便捷接口已全部覆盖。

## 2D 场景组件

重新构建 Editor 后，组件注册入口为 **Physics 2D / Rigidbody** 和 **Physics 2D / Collider**。Collider 支持 Box、Circle、Capsule、Polygon、Edge、Chain，Size 是完整尺寸，单位为米；Center 使用局部坐标，Local Rotation 以度编辑。胶囊的 Segment Half Height 不包含圆端半径。

Polygon 的 Vertices 数组使用局部米制坐标，支持 3–256 个顶点的单一简单轮廓，顺逆时针均可，
不包含孔洞或重复的闭合起点。凹轮廓先拆分为凸片，保留缺口，所有凸片共享同一 Body 和 Collider。
Edge 恰好使用两个顶点，双面碰撞。Chain 使用 2–257 个开放顶点，启用 Loop 时为 3–256 个；
碰撞正面朝向轮廓前进方向的右侧，并使用相邻辅助顶点抑制内部接缝碰撞。Chain 只支持静态、
非 Trigger 刚体；组件镜像后保持原来配置的碰撞正面。

自交、重复或过近的顶点、非法数值和不支持的修改会产生 LastError，并保留整组旧碰撞体。
轮廓及辅助顶点间距至少为 0.01 米。查询返回真实凸片或线段及 `QueryHit2D.subShapeId`；
构建器在每份几何修订内从 1 编号。`GetShapeHandles()` 返回全部成员，`IsQueryReady()`
检查每个句柄。Vertices、Loop 作为配置保存和克隆。自动化覆盖真实质量/冲量、凹形查询、
单向碰撞、接缝滑动、子对象变换和整组生命周期。多边形绘制工具、多轮廓 Composite、孔洞、
Tilemap 集成及实际 Inspector 操作仍待完成。

同一 Rigidbody 可以关联自身及子对象上的多个 Collider；嵌套 Rigidbody 开始独立的刚体。没有 Rigidbody 的 Collider 创建静态体。同一对象不能同时由 2D 和 3D Rigidbody 驱动。2D 模拟只修改 XY 和绕 Z 的旋转，保留 Z 位置；Sprite 像素到世界单位的 PPU 转换由 2D 内容模块负责。

编辑态同步查询几何而不推进模拟。Play 使用独立场景世界；动态姿态由物理回写，父对象平移不自动传送动态子刚体。非法倾斜、退化变换或剪切会产生组件诊断。原生及场景生命周期测试已在 Windows Debug 通过，实际编辑器鼠标操作验收尚未完成。

场景物理配置现已支持保存与克隆：包括两个维度的重力、Box2D 求解子步、工作线程数和可选渲染插值。缺少配置的旧场景使用默认值。原生 `Scene::SetPhysicsSettings` 可立即更新重力和求解子步；线程数在下一次创建世界时生效。专用编辑器配置入口仍待实现。

启用 `interpolate=true`（默认关闭）后，渲染在最近两个固定姿态间采样，不改写权威 Transform 或查询世界。网格、灯光、天空盒代理、Sprite、粒子视图变换和游戏相机使用此表现路径。视觉子对象继承最近活动刚体的表现姿态，嵌套刚体独立处理。传送、状态恢复、形状重建、暂停和单步会清理插值历史。实际运动平滑度与 GPU 验收独立于自动化代理/相机测试记录。

## 原生查询接口

`PhysicsWorld` 保留旧注册接口，并提供 CreateBody 与独立 Attach 形状接口，支持盒、球、胶囊、SDK 自定义圆柱、凸网格、静态三角网格和真实采样高度场。动态凹三角网格会被拒绝。高度场的米制行主序数据沿 X/Z 排列，SDK 使用自动比例的 16 位高度采样；完整 Terrain 创作工具不包含在此入口中。通过 `Read(QueryFilter)` 获取读取视图，可执行 `Raycast`、`RaycastAll`、`SweepSphere` 和 `OverlapSphere`。同线程修改前先释放读取视图。

每个场景拥有独立查询世界。[Particle System](../audio-vfx/particles.md) 默认连接拥有场景的世界。碰撞体形状、尺寸、层和 Trigger 配置可通过反射/脚本访问；通用查询接口目前仍是原生 API，不等于已提供完整 C# / Lua 物理查询库。

3D 读取视图还提供 SweepShape、SweepShapeAll、SweepShapeNonAlloc、OverlapShape 和 OverlapShapeNonAlloc。QueryGeometry3D 可描述球、盒、Y 轴胶囊、Y 轴圆柱，或借用同一世界的现有凸形状；借用方式不会重新 Cook，并自动忽略来源刚体。GetShapeWorldPose 返回含原生局部对齐的完整姿态，作为查询姿态传入。静态三角网格和高度场可被查询，作为来源体积会明确返回 UnsupportedGeometry。结果状态区分合法未命中、输入错误及失效句柄；缓冲接口报告 count、total 和 truncated，空缓冲可统计容量。基础形状默认使用 SDK 精确扫掠路径，凸网格和圆柱遵循 SDK GJK 容差。起点重叠以 initialOverlap 标记，使用接触位置和法线前必须检查 hasPointAndNormal。

`PhysicsWorld2D` 提供安全的 Body/Shape 句柄、复合刚体、质量/密度、力与冲量、CCD、运动锁、睡眠，以及 Raycast、RaycastAll、CircleCast 和 OverlapCircle。角度及角速度的原生单位是弧度。Overlap 只返回对象身份；读取 `QueryHit2D` 的接触位置和法线前检查 `hasPointAndNormal`。

两个维度均可用 `BeginMutationBatch()` 批量提交修改，阻止查询读取重建过程的中间状态。先释放同线程的 ReadAccess，再修改世界。
原生 `PhysicsCommandBuffer` 可先记录刚体创建、形状装配/替换、动力学配置和释放，再按顺序批量提交；输入几何由队列持有，结果票据检查队列身份和世代。接触回调中新增/删除组件或提交运动命令会延迟到安全边界，`Scene::TrySyncTransforms()` 在该回调范围内返回 false。回调可以继续使用已持有的查询视图；直接修改原生世界仍须先释放视图。Stop 会清空未消费的运动命令并释放旧世界中的对象。
原生世界重力和 Box2D 求解子步也遵循这一提交边界。回调中调用 `Scene::Stop()` 会记录停止请求，在安全同步后停止并取消本帧剩余补跑步骤。场景管理器在回调内拒绝立即加载、卸载或丢弃当前场景；可用 `LoadAndPlayDelayed` 排队，待回调返回后执行 `Update()`。自动化验证覆盖了双维度 Prefab 对象图实例、Collider 重挂父级、嵌套动态刚体独立姿态，以及保存后的运行场景反复替换；完整 Prefab 资产和编辑器鼠标操作仍需单独验收。
两者还提供调用方数组查询：RaycastNonAlloc 和对应 Overlap NonAlloc，返回已写入数、总命中数及 truncated 标记。射线容量不足时保留最近命中，空数组可统计所需容量。两个原生世界支持状态捕获/恢复，要求世界、形状拓扑及动力学配置未改变；该功能不恢复求解缓存，也不承诺跨平台严格确定性。

3D 接触事件按 Collider 对合并复合子形状，每步最多一次 Stay，睡眠接触也保留此契约。事件包含步索引、双方身份及最多四个真实接触采样；读取接触数据前检查 hasContactData。完整脚本碰撞数据接口仍待接入。

## 物理资源与离线烘焙

已接入 `.physmat` 物理材质和 `.collisionmesh` 碰撞资源，沿用 `.meta` GUID、PPtr 稳定引用、ArtifactDB、原生工件容器和运行时 Manifest。共享 Cook 与编辑器导入/重导入调用同一编译器。两个维度的 Collider 都有 `physicsMaterial` 引用；3D 另有 `collisionMesh` 引用及 `COLLISION_MESH` 形状值。未指定资源时继续使用原有内联物理属性。

源文件使用 JSON `version: 1`。材质可分别配置 `staticFriction`、`dynamicFriction`、`restitution`、`friction2D` 和 `restitution2D`；`frictionCombine`、`restitutionCombine` 的 average/minimum/multiply/maximum 作用于 3D，2D 使用 Box2D 自身的系数混合。最小材质文件为 `{"version":1}`。

碰撞资源支持 `kind: "convex"` 的三维点集、`"triangle"` 的顶点/索引，以及 `"heightfield"` 的米制行列采样。可配置 `bakeScale`、顶点焊接参数和凸包顶点上限。网格镜像在 Cook 时修正三角形朝向；高度场要求正缩放。运行时加载已经烘焙的数据，并在共享同一资源的世界间复用原生几何。三角网格和高度场仍仅支持静态、非 Trigger 碰撞体；动态凸网格可正常参与求解。组件上的网格镜像需先烘焙，非法输入和不支持的变换保留原有效形状并报告诊断。

缓存区分源 GUID、内容、烘焙参数、元数据、后端版本和目标平台。取消任务或源数据过期时保留上一份有效工件。原生碰撞 Cook 要求匹配的本机 OS/CPU，跨平台或跨架构请求明确失败；实际平台验证仍分别记录。加载器核对工件 SHA-256、类型、GUID/fileID、版本和运行目标，场景只保存引用。

自动化已覆盖真实高度场承托、双维度材质反弹、缓存失效、资源重载和场景克隆；删除源文件、`.meta` 和 ArtifactDB 后，运行时 Manifest 仍可加载并模拟烘焙碰撞。专用资源创作界面、拖放、Undo/Redo、可视化操作和完整 Player Cook 验收仍待完成。

## 可复用复合 Shape

`.physshape` 可在一个 Collider 下组合 1–256 个基础形状或烘焙网格子形状。
选择 Shape Asset（值 5），并设置带类型的 Shape Resource 引用。
源文件使用 JSON `version: 1`，每个子形状必须有唯一的正整数 ID：

```json
{"version":1,"shapes":[
  {"id":7,"type":"box","halfExtents":[1,0.25,0.5],"position":[-2,0,0]},
  {"id":19,"type":"sphere","radius":0.5,"position":[2,0,0]}
]}
```

type 支持 box、sphere、capsule、cylinder、mesh。可选 rotation 为 X/Y/Z/W 四元数，
position、scale 使用 Collider 的局部米制坐标。网格通过 `"mesh":{"guid":"..."}`
引用 `.collisionmesh`，每个子形状可用 `"material":{"guid":"..."}` 引用 `.physmat`
覆盖材质。所有子形状沿用 Collider 的层、Mask 和 Trigger。球要求均匀缩放，胶囊和圆柱
要求 X/Z 径向缩放一致；剪切、未烘焙的网格镜像会报告诊断。三角网格和高度场仍只支持
静态、非 Trigger 用途。

导入、Cook 和运行时 Manifest 沿用现有资源管线，并包含网格、材质的传递依赖。
重载在安全同步阶段整体替换几何组并保留 Body；几何非法或子资源失效时保留整组旧碰撞体。
原生查询返回 `subShapeId`，接触事件用 `firstSubShapeId`、`secondSubShapeId` 标记
代表性接触对子形状。源文件重排不改变子形状 ID，替换成功后旧原生句柄失效；刚体正常旋转
保持几何组。专用资源创作界面、拖放、Undo/Redo 和实际编辑器交互验收仍待完成。

## 依赖与构建

CMake 固定 PhysX 5.6.1 与 Box2D 3.2.0 的版本和归档校验值，要求 CMake 3.22 及以上。离线配置分别使用 `NLS_PHYSX_SOURCE_DIR`（含 `physx/`）和 `NLS_BOX2D_SOURCE_DIR`（Box2D 源码根目录）。SDK 源码与产物放在构建目录。

物理测试已并入 `NullusUnitTests`，用 `--gtest_filter="Physics*.*"` 聚焦。
用例覆盖两个后端、固定步时钟、共享 TaskGraph、查询原子性、场景、序列化和对象释放。测试通过不代表其他平台或交互验收完成。

## 尚未实现

- 完整 3D 资源创作界面、模型自动生成碰撞、约束与角色控制器。
- 2D 多轮廓 Composite 与孔洞、Tilemap 碰撞、完整形状查询脚本接口及单向平台。
- 完整碰撞数据的脚本接口、2D 事件绑定、物理调试面板、Gizmos 和插值的可视化验收。
- 布娃娃、载具、破碎、浮力及可选 CUDA 软体/布料模块。

默认固定步长为 1/60 秒，每帧最多 8 步、累计上限 0.25 秒；Box2D 求解子步独立配置，默认 4。播放单步恰好推进一个固定步，包括 TimeScale 为零的情况，并保留正常播放的不足一步余量。该行为已通过无界面测试，编辑器实际点击尚未验收。[动画](../animation/roadmap.md)提供姿态和表现，不能替代专用角色控制器。

## 相关内容

- [生命周期与激活状态](../scene/lifecycle.md)：固定步长回调。
- [功能状态矩阵](../roadmap/index.md)：能力与验证边界。
