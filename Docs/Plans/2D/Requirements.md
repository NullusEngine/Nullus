# Nullus Engine — 完整 2D 游戏开发系统实施任务

## 一、你的角色与最终目标

你现在担任 Nullus Engine 的高级引擎架构师、C++ 图形程序员、物理引擎工程师、编辑器工程师和自动化测试工程师。

你的任务不是提出概念方案，而是直接基于当前 Nullus 源代码仓库，设计、实现、集成、测试并交付一套完整的、生产级的 2D 游戏开发解决方案。

**最终目标：让 Nullus 具备与 Unity 2D 核心功能体系相当的完整 2D 游戏制作能力。**

应支持以下游戏类型：

- 像素风 RPG、农场经营、俯视角动作游戏。
- 横版平台跳跃、Metroidvania、2D 动作游戏。
- 2D 塔防、策略和 Tilemap 地图游戏。
- 带有法线贴图、动态光照、阴影与粒子特效的高品质 2D 游戏。
- 2D 与 3D 混合的 2.5D 游戏。
- 采用逐帧动画或骨骼动画的 2D 角色游戏。

**最终必须实现从创建 2D 项目、资源导入、场景编辑、角色制作、物理交互、动画制作、特效、脚本编写、运行调试到游戏构建发布的完整工作流。**

不要将任务简化为 SpriteRenderer + Box2D + Tilemap 的演示工程。

不要只生成架构文档、空接口、伪代码或者没有集成到编辑器的组件。

必须真正修改代码、实现功能，并完成验证。

---

## 二、首先审计 Nullus 现有架构

Nullus 是一款已有 3D 渲染、编辑器、场景管理、资源管理和脚本能力的自研游戏引擎。

历史技术背景如下，但必须通过实际仓库代码核实：

- C++20。
- CMake 构建。
- GoogleTest 测试。
- GLFW 平台事件。
- Windows 编辑器部分使用 Win32 ImGui 后端。
- 已有自研 RHI、渲染基础设施。
- 已有 Scene / Entity / Component / Transform。
- 已有资源和序列化系统。
- 已有材质系统，包括 `.mat`、`.shadergraph`、`.shaderfunc`、`.shader` 等设计。
- 已有动画系统或相关开发工作。
- 已有 C++、C#、Lua 脚本集成。
- 已有 RuntimePlaySession，包含 FixedUpdate、Update、LateUpdate 等生命周期。
- 已有或正在迁移的统一 Input Actions 系统。
- 已有 3D 物理系统或相关接口。

这些仅作为审计线索，不能直接假设功能均已完成。

### 2.1 必须检查的模块

实际检索仓库，确认：

1. Scene、Entity、Component、Transform 的结构及生命周期。
2. Scene 序列化、Prefab、资源引用和组件注册机制。
3. RHI、Render Graph、Renderer、Camera、MeshRenderer、Material 的实现。
4. Texture Importer、Asset Database、Asset Handle、资源热重载、资源打包机制。
5. Shader 编译、ShaderLab、材质实例、着色器变体管理机制。
6. 动画剪辑、Animator、状态机、时间轴及动画属性绑定机制。
7. 输入系统、固定时间步、脚本更新顺序。
8. C++、C#、Lua 组件访问与脚本绑定。
9. ImGui 编辑器、Hierarchy、Inspector、Scene View、Gizmos、Undo/Redo。
10. 已有物理、粒子、UI、音频与项目构建系统。
11. Windows、macOS、Linux 支持情况及渲染后端。
12. 当前测试、示例场景和 CI 构建流程。

针对每一项，说明：

- 现有类与文件位置。
- 可以直接复用的能力。
- 需要扩展的接口。
- 缺失能力。
- 潜在的兼容性问题。
- 推荐修改方案。

建立明确的模块集成关系，并将审计结果写入项目文档。

### 2.2 架构约束

**必须采用统一 2D / 3D 引擎架构。**

不要重新创建第二套 Scene、ECS、Transform、Scripting、Asset Manager、Animator、Input System 或 RHI。

2D 应作为可组合的引擎功能存在，而不是一套与 3D 平行的独立 Runtime。

推荐：

- 共享 Scene / Entity / Component。
- 共享 Transform，2D 默认使用 XY 平面、Z 轴旋转。
- Z 轴可参与显示排序，但不参与 2D 物理。
- 共享 Camera，增加 2D / Pixel Perfect 功能。
- 共享 RHI、Render Graph 和 Shader 基础设施。
- 增加 Sprite 专用渲染提交与合批系统。
- 共享材质基础设施，增加 Sprite Unlit / Sprite Lit 材质域。
- 保留独立 Physics2DWorld，与 3D 物理分别模拟。
- 共享 Animator、AnimationClip，增加 Sprite 动画能力。
- 共享 C++ / C# / Lua 脚本运行时。
- 共享 Input Actions、UI、音频、Prefab、资源系统与打包流程。
- 编辑器增加 2D 工作模式及专用工具。

坐标系、矩阵约定、资产句柄、组件存储方式、渲染线程调度等必须遵守现有仓库规范。

如果某个通用功能尚不存在，应优先将其实现为可供 2D / 3D 复用的能力，而不是新增 2D 专用的重复基础设施。

---

## 三、完整功能范围

以下所有模块都属于最终交付范围。可以按阶段开发，但不允许将 P1、P2 自动视为可永久省略的功能。

### 模块 A：2D 项目、场景与摄像机

实现：

1. 2D Project Template。
2. 2D Scene Template。
3. Scene View 2D / 3D 模式切换。
4. 默认正交摄像机。
5. 2D 场景网格。
6. 像素吸附与网格吸附。
7. Camera Orthographic 参数。
8. Camera Pixel Perfect 模式。
9. Reference Resolution。
10. Pixels Per Unit。
11. Integer Scaling。
12. Pixel Snapping。
13. Letterboxing / Crop。
14. 分辨率适配与 Render Texture Upscaling。
15. Camera Follow、Dead Zone、Bounds Confiner 等常用 2D 摄像机辅助组件。
16. 2D 与 3D 混合场景及 Camera 渲染策略。

必须处理物理插值、摄像机平滑移动和像素吸附之间的关系，避免 Pixel Art 出现明显抖动。

2D 项目模板不得改变 Scene 和 Transform 的底层存储格式。

### 模块 B：Sprite 资源与导入

实现完整 Sprite Asset 系统：

- Texture2D 与 SpriteAsset 分离。
- Single Sprite 和 Multiple Sprite。
- Sprite Sheet。
- Sprite Rect。
- Sprite Pivot。
- Pixels Per Unit。
- Sprite Border。
- 9-Slicing 元数据。
- Sprite Trim。
- Original Size / Trim Offset。
- UV 与顶点数据。
- Full Rect / Tight Mesh。
- Flip。
- Sprite 自定义物理形状。
- Sprite 子资源稳定 ID。
- Import Settings。
- 纹理过滤、压缩、Mipmaps、Alpha、sRGB 设置。
- 自动重新导入与资源热重载。
- 缺失纹理或 Sprite 的安全降级。

一张 Texture 必须能够包含多个 Sprite。

Sprite 子资源必须拥有稳定标识，不能因为切片排序变化而破坏引用。

重新导入或 Atlas 重新打包后，Prefab、AnimationClip、Tilemap 和 Scene 中的引用必须保持有效。

编辑器提供完整 Sprite Editor：

- 自动切片。
- Grid 切片。
- 手动切片。
- Pivot 编辑。
- Border 编辑。
- Trim 预览。
- Sprite 物理轮廓编辑。
- Sprite 预览。
- Undo/Redo。
- 保存与重新导入。

需要兼容现有 Asset Database 与 Cooked Asset Pipeline。

### 模块 C：SpriteRenderer 与 Renderer2D

实现：

- SpriteRendererComponent。
- SpriteBatcher。
- Sprite Render Data。
- Sprite Visibility Culling。
- Sprite Sorting。
- Sprite Atlas 引用解析。
- Sprite GPU Instancing 或高效 Dynamic Batching。
- Sprite Unlit Material。
- Sprite Lit Material。
- Sprite Color Tint。
- FlipX / FlipY。
- Simple / Sliced / Tiled Draw Mode。
- Sorting Layer。
- Order in Layer。
- Sorting Group。
- Center / Pivot Sort Point。
- Y-Axis Sorting。
- Transparency Sort Axis。
- Sprite Mask。
- Sprite Picking / Selection。
- Sprite Bounds。
- Sprite Gizmos。
- Sprite Debug Rendering。

底层要求：

1. 继续使用 Nullus RHI。
2. 复用现有 Shader 和 Material 管理。
3. 使用 Render Graph 调度所需渲染 Pass。
4. 使用共享 Quad 或等价的高效几何提交机制。
5. 进行视锥剔除。
6. 对兼容的 Sprite 合批。
7. 提供 Batch / Draw Call / Sprite Count 统计。
8. 支持 CPU 动态批次与可用时的 GPU Instancing。
9. 不强制依赖 Bindless Texture 等可能不存在的硬件特性。
10. 保持透明排序、深度策略和 2D / 3D 混合行为正确。

透明 Sprite 不能为了合批而破坏原本的排序语义。

SpriteRenderer 与 MeshRenderer 可以共享 RHI 资源和部分 GPU 数据结构，但应具有明确的 Sprite 渲染路径。

不要每个 Sprite 都创建独立的 GPU Vertex Buffer。

### 模块 D：Sprite Atlas

实现 Sprite Atlas 生产管线：

- AtlasAsset。
- Atlas Packer。
- Auto Packing。
- Atlas Padding。
- Extrusion。
- Rotation Policy。
- Trimmed Sprite。
- 多页 Atlas。
- 平台相关纹理尺寸和格式。
- Atlas Preview。
- Atlas Rebuild。
- Atlas 依赖管理。
- Sprite 到 Atlas Region 的映射。
- 运行时 Atlas 资源加载。
- Atlas 热重载。
- 构建时 Atlas 生成。
- 打包统计与诊断。

逻辑 Sprite 引用必须和物理 Atlas UV 解耦。

支持素材未打包时的编辑器预览，以及 Cooked Atlas 加载。

需要避免 Point/Bilinear 过滤导致相邻 Sprite 的颜色渗透。

### 模块 E：Physics2D

优先采用官方稳定版 Box2D 3.x。

官方文档：https://box2d.org/documentation/

注意 Box2D 3.x 使用 C API 和 ID Handle，与旧版 C++ API 不同。

集成必须符合实际依赖版本，不得照搬不兼容的旧接口。

实现：

**核心物理对象**

- Physics2DWorld。
- Rigidbody2D。
- Static / Dynamic / Kinematic。
- BoxCollider2D。
- CircleCollider2D。
- CapsuleCollider2D。
- PolygonCollider2D。
- Edge / Chain Collider2D。
- CompositeCollider2D。
- 多 Shape 的 Compound Collider。
- PhysicsMaterial2D。
- Density、Friction、Restitution。
- Gravity Scale。
- Linear / Angular Damping。
- Freeze Rotation。
- Body Awake / Sleep。
- Continuous Collision。
- Sensor / Trigger。

**关节**

- Distance Joint。
- Revolute Joint。
- Prismatic Joint。
- Wheel Joint。
- Weld Joint。
- Motor Joint。
- Mouse Joint。

根据 Box2D 实际版本能力封装，避免在上层泄漏底层 ID。

**查询与交互**

- Raycast。
- RaycastAll。
- OverlapPoint。
- OverlapCircle。
- OverlapBox。
- OverlapArea。
- Shape Cast。
- Contact Filter。
- Collision Layer Matrix。
- Collision Mask。
- Trigger Events。
- Collision Events。
- Physics Queries Debug Drawing。

**生命周期与模拟**

- 每个运行时 Scene 拥有独立 Physics2DWorld。
- Fixed Time Step。
- Max Fixed Steps Per Frame。
- Substeps。
- Transform 同步。
- Rigidbody Interpolation。
- 创建、销毁和重建 Shape。
- Collider 参数热更新。
- 组件启用、禁用。
- Scene / Entity 销毁时安全释放物理对象。
- 碰撞事件缓存与安全派发。

必须明确 Transform 与 PhysicsBody 的权威关系：

- Dynamic Rigidbody 由模拟驱动位置。
- Kinematic Rigidbody 通过规定接口驱动运动。
- Static Body 以静态场景数据为基础。
- Teleport、SetPosition、SetVelocity 具有清楚的语义。
- 父子 Transform、非均匀缩放、负缩放、Collider Offset、旋转必须处理或提供明确有效性校验。
- 2D 与 3D 物理不直接混合模拟。

不要在物理求解过程中执行不安全的 Entity 删除操作。

碰撞事件必须与 Nullus RuntimePlaySession 的 FixedUpdate 生命周期正确衔接。

需要为 C++、C#、Lua 提供一致的 Physics2D API。

### 模块 F：Tilemap 与 Tile Palette

提供 Unity 式完整瓦片地图系统。

**Runtime**

- GridComponent。
- TileSetAsset。
- TileAsset / TileDefinition。
- TilemapAsset。
- TilemapComponent。
- TilemapRenderer。
- TilemapCollider2D。
- Composite Tilemap Collider。
- Rectangular Grid。
- Isometric Grid。
- Hexagonal Grid。
- Tilemap Layer。
- Tile Color。
- Tile Transform。
- Animated Tile。
- Rule Tile。
- Random Tile。
- Auto Tile。
- Tilemap Runtime Get/Set API。
- Chunk-based Storage。
- Chunk Visibility Culling。
- Chunk Mesh / Instance Cache。
- Chunk Incremental Rebuild。
- Chunk Physics Rebuild。
- 大地图分块加载与卸载。

推荐以 32×32 或经过基准测试确定的大小作为默认 Chunk。

Tilemap 必须采用专用数据存储，不允许为每个 Tile 创建一个独立 Entity。

修改单个 Tile 时，只更新受影响的 Chunk 及其必要邻域。

**编辑器**

- Tile Palette。
- Pencil Brush。
- Eraser。
- Fill Brush。
- Rectangle Brush。
- Select。
- Move。
- Copy / Paste。
- Eyedropper。
- Tile Rotation / Flip。
- Grid Snapping。
- 多图层编辑。
- Tilemap Collider 可视化。
- Rule Tile 规则编辑器。
- Tilemap Undo/Redo。
- 批量编辑与保存。

Rule Tile 应支持邻居匹配、旋转/镜像规则、边缘、拐角等自动拼接逻辑。

Tilemap 可被 Scene 和 Prefab 正确引用、复制、保存和重新加载。

### 模块 G：2D Animation

复用现有 Nullus AnimationClip、Animator、Animation Controller 和 Timeline。

新增：

- Sprite Reference Track。
- Frame-by-frame Animation。
- Sprite AnimationClip。
- Sprite Frame Duration。
- Animation Events。
- Loop / PingPong / Once。
- Sprite Animation Preview。
- 自动从 Sprite Sheet 创建 AnimationClip。
- Sprite Library。
- Sprite Resolver / Skin Variant。
- 动画状态机适配。
- 动画属性绑定。
- Sprite Animation Timeline。

必须正确处理 Sprite 引用等离散类型的动画切换，不能把 Sprite 引用当成浮点数插值。

需要保持与通用 Animator 状态机、动画事件和参数系统兼容。

**高级骨骼动画**

实现：

- Sprite Mesh。
- 2D Bone Hierarchy。
- Skin Weights。
- Bone Bind Pose。
- Sprite Skin Component。
- Sprite Deformation。
- 2D IK。
- Bone Gizmos。
- Bone Editor。
- Weight Editor。
- Pose Editor。
- Sprite Swap。
- 骨骼动画 Preview。

根据现有 GPU 和动画架构选择 CPU / GPU Deformation，并明确两者支持范围和性能差异。

这些能力必须使用统一 Animator，不要重复创建完整 2D 状态机。

### 模块 H：2D Lighting、Shadow 与材质

在 Nullus 现有材质系统中增加 Sprite 适用的渲染域和 Shader。

必须支持：

- Sprite Unlit。
- Sprite Lit。
- Global Light 2D。
- Point / Spot Light 2D。
- Freeform Light 2D。
- Sprite Light。
- Light Color。
- Light Intensity。
- Light Falloff。
- Light Blend Style。
- 2D Normal Map。
- Sprite Emissive。
- ShadowCaster2D。
- Polygon Shadow Shape。
- Tilemap Light Occlusion。
- 2D Shadow Rendering。
- Light Culling。
- Light Debug Visualization。
- 灯光的编辑器 Gizmos。

要求：

1. 共享现有 Material 和 Shader 编译流程。
2. Sprite 材质能通过现有 ShaderLab / Material Graph / Custom HLSL 体系使用，按当前仓库实际支持范围扩展。
3. 共享 Render Graph 和 RHI。
4. 支持 Sprite Normal Map 对应的纹理坐标和法线方向。
5. 支持透明 Sprite、2D Shadow 和 3D 场景的合理混合。
6. 提供可配置的光照数量、混合方式和降级路径。
7. 支持批次调试和 RenderDoc 等图形调试工具。

不要为 2D 再实现第二套 Material Manager。

### 模块 I：SpriteShape 与高级地图构建

实现：

- SpriteShapeAsset。
- Spline Control Points。
- Bezier / Linear Segments。
- Sprite Edge Rules。
- Corner Sprites。
- Fill Texture。
- Runtime Shape Geometry Generation。
- SpriteShapeRenderer。
- SpriteShapeCollider2D。
- 编辑器路径 Gizmo。
- 路径点插入、删除、移动。
- 闭合路径和开放路径。
- Undo/Redo。
- 保存、序列化与打包。

目标是能用样条线创建连续的 2D 地形、道路、平台、装饰边缘和曲线轮廓。

不要只实现一个显示样条线的空编辑器。

### 模块 J：高级素材导入

提供：

- PNG / WebP Sprite 导入。
- Sprite Sheet 自动识别。
- Aseprite 文件导入。
- Aseprite Layers。
- Aseprite Tags / Animation。
- Aseprite Slice / Pivot。
- PSD / PSB 分层导入。
- Layer-to-Sprite 映射。
- 分层图像的增量重新导入。
- Layer ID、Sprite ID 稳定引用。
- 可选的自动生成 Sprite Animation。

必须先调查仓库已有图像解码器和第三方依赖。

采用许可兼容且可维护的实现方案。

如果某些格式无法直接可靠解析，可以设计明确的外部转换工具链，但必须真正接入 Import Pipeline，不能只提供手工转换文档。

### 模块 K：2D 特效与其他游戏系统集成

整合已有通用系统：

- 2D Particle Rendering。
- Sprite Sheet Particle Animation。
- Particle Sorting Layer。
- Particle Material。
- Additive / Alpha Blend。
- Particle 与 2D Light 的集成。
- 2D Camera Shake。
- 2D Post Processing。
- Sprite Mask / Render Texture。
- World Space UI。
- Screen Space UI。
- 2D Audio Listener / Source 使用。
- Input Actions 在 2D 游戏中的应用。
- C++ / C# / Lua 游戏逻辑。
- Prefab 和 Prefab Variants。
- Runtime Scene Loading。
- 2D Debug Overlay。
- 2D / 3D 混合场景。

不要重复编写完整输入、音频、UI 和粒子运行时。

应复用现有系统并补齐 2D 所需的适配能力。

---

## 四、编辑器必须达到完整可用状态

Nullus 编辑器使用现有 ImGui 架构。

新增以下能力：

| 编辑器模块 | 需要实现的功能 |
|---|---|
| Project Creation | 2D 项目模板 |
| Scene View | 2D / 3D 切换、网格、吸附、正交视图 |
| Hierarchy | 创建 Sprite、Tilemap、2D Light、Physics2D Object |
| Inspector | 所有 2D 组件编辑 |
| Sprite Editor | 切片、Pivot、Border、Physics Shape |
| Tile Palette | 地图绘制与 Rule Tile |
| Sprite Atlas Editor | Atlas 配置、Preview、Build |
| Animation Editor | Sprite Track、动画预览、骨骼 |
| Physics Debugger | Collider、Joint、Raycast、Contact |
| Light Editor | 2D Light / ShadowCaster 编辑 |
| SpriteShape Editor | 样条线与轮廓编辑 |
| Game View | Pixel Perfect 预览与分辨率切换 |
| Profiler | Sprite Batch、Draw Call、Tile Chunk、Physics2D |
| Project Browser | Sprite / Tile / Atlas 资源预览 |

额外要求：

- 所有重要 2D 资源支持 Drag & Drop。
- 2D 组件支持 Add Component 菜单。
- 编辑器属性变化实时更新预览。
- 支持多选对象以及合理的批量属性编辑。
- 支持 Undo / Redo。
- 支持 Copy / Paste。
- 支持 Prefab 实例和属性覆盖。
- 支持 Scene 保存与重新加载。
- 支持资源热重载。
- 支持 Play / Pause / Step。
- Play Mode 与 Edit Mode 状态隔离正确。
- 编辑器窗口不会依赖运行时尚未初始化的物理或渲染对象。
- 所有新增编辑器功能保持跨平台可编译，不直接绑定到 Win32 特有代码。

编辑器必须真正能够使用新增的运行时组件，不能出现运行时已有功能，但用户只能通过手写 JSON 才能配置的情况。

---

## 五、脚本 API 与 Runtime 集成

统一为 C++、C#、Lua 提供一致的 2D 能力。

至少包括：

**Sprite**

- 获取、设置 Sprite。
- 颜色、翻转、排序层。
- 动态更换 Sprite。

**Physics2D**

- 获取和修改 Rigidbody2D。
- 速度、力、冲量。
- Raycast / Overlap / ShapeCast。
- Collision / Trigger 回调。

**Tilemap**

- 坐标转换。
- GetTile / SetTile。
- 查询 Tile。
- 局部更新 Tilemap。

**Animation**

- 播放 AnimationClip。
- 修改 Animator 参数。
- 切换 Sprite。
- 响应 Animation Event。

**Camera**

- 2D Camera 位置。
- Orthographic Size。
- WorldToScreen / ScreenToWorld。
- Pixel Perfect 设置。

**Light**

- 获取和修改 2D Light 参数。

脚本 API 必须沿用当前绑定、生命周期和类型反射体系。

不得要求 C# 或 Lua 用户直接操作 Box2D 原生 ID、RHI Handle 等底层对象。

---

## 六、资源格式与构建流程

沿用 Nullus 的资源格式约定和 Asset Database，不要单独创建第二套 2D 资源数据库。

建议新增或扩展：

- Sprite Asset。
- Sprite Atlas Asset。
- TileSet Asset。
- Tilemap Asset。
- Rule Tile Asset。
- PhysicsMaterial2D。
- SpriteShape Asset。
- AnimationClip Sprite Track。
- Sprite Skin / Skeleton Asset。
- Sprite Material。

文件扩展名可使用 `.sprite`、`.spriteatlas`、`.tileset`、`.tilemap` 等，但必须先确认现有资源系统是否更适合使用 Texture 子资源和统一资源容器。

不要为了使用指定扩展名破坏现有 Asset Pipeline。

必须支持：

1. 资源版本化。
2. Schema Migration。
3. 稳定 Asset ID。
4. 资源依赖追踪。
5. Import Cache。
6. Incremental Reimport。
7. Hot Reload。
8. Cooked Assets。
9. Platform Variants。
10. 资源损坏和引用失效诊断。
11. Editor-only 资源与 Runtime 资源分离。
12. 正式构建不依赖 Editor、源图片或外部编辑软件。

2D 项目必须能够通过 Nullus 现有打包系统导出独立可运行游戏。

---

## 七、实施阶段

按照以下顺序开发，完整范围不得遗漏。

### Phase 0：架构审计和开发计划

交付：

- 当前仓库架构审计报告。
- 2D 系统技术设计文档。
- 模块依赖关系。
- 组件与资源模型。
- 渲染、物理、动画的更新时序。
- 编辑器集成方案。
- 完整任务清单与验收标准。

此阶段完成后立即推进核心代码实现，不要只提交方案就结束整个任务。

### Phase 1：2D Core + Sprite Renderer

实现：

- 2D Project / Scene。
- Orthographic Camera。
- SpriteAsset。
- SpriteRenderer。
- Sorting Layer。
- Sprite Batching。
- Sprite Unlit。
- 2D Scene View。
- 基础资源序列化。
- Sprite 渲染测试场景。

验收：能够通过编辑器创建 2D 场景，导入图片、拖入 Sprite、编辑属性、保存并播放。

### Phase 2：Sprite 工具链和渲染完善

实现：

- Sprite Importer。
- Sprite Editor。
- Single / Multiple。
- Pivot / Trim / 9-Slice。
- Sprite Atlas。
- Pixel Perfect Camera。
- Sprite Mask。
- Sprite Sorting Group。
- Sprite 资源热重载。
- Sprite Picking。
- 性能分析。

验收：可使用 Sprite Sheet 和 Atlas 开发具有正确像素显示、排序与资源引用的 2D 场景。

### Phase 3：Physics2D

实现完整 Box2D 集成，包括刚体、碰撞体、触发器、查询、关节、固定时间步、脚本回调、编辑器 Gizmo。

验收：可制作角色移动、跳跃、碰撞、动态物体、触发器和移动平台示例。

### Phase 4：Tilemap

实现 Grid、TileSet、Tilemap、Chunk Rendering、Tile Palette、基础编辑操作、Rule Tile、Animated Tile、瓦片碰撞，以及大地图优化。

验收：可以通过编辑器制作、保存、运行和打包实际 Tilemap 关卡。

### Phase 5：2D Animation

实现 Sprite 逐帧动画、AnimationClip 集成、Animator 状态机、Sprite Library、动画编辑器及脚本控制。

验收：可制作完整角色 Idle / Run / Jump / Attack 等动画流程。

### Phase 6：2D Lighting、Material 与 VFX

实现 Sprite Lit、Global / Point / Freeform Light、Normal Map、ShadowCaster2D、Sprite Light、2D 粒子集成与混合渲染。

验收：可制作带动态光照、阴影、法线贴图和粒子效果的 2D 游戏场景。

### Phase 7：高级 2D 功能

实现：

- Sprite Skinning。
- 2D Bones。
- 2D IK。
- SpriteShape。
- Isometric / Hexagonal Tilemap。
- Aseprite Import。
- PSD / PSB Import。
- 高级物理形状和编辑工具。
- 2.5D 混合能力。

验收：各项高级功能在编辑器中具有可操作工具，并有可运行的示例。

### Phase 8：完整产品化和回归验证

实现：

- 完整 2D Demo。
- 工程模板。
- 资源烘焙与平台构建。
- 用户文档。
- 性能测试。
- 自动化测试。
- 示例工程。
- Bug 修复。
- 全系统集成测试。

验收：用户不需要修改引擎源代码，就可以使用 Nullus 编辑器完成完整的 2D 游戏制作和打包。

---

## 八、必须提供三个示例工程

### Demo A：2D Platformer

包含：

- 角色移动、跳跃。
- 地面与墙壁碰撞。
- 移动平台。
- Sprite Animator。
- Camera Follow。
- Tilemap。
- 简单敌人。
- 触发器。
- UI 和音效。
- 关卡切换。

用于验证横版游戏完整工作流。

### Demo B：Top-Down Pixel RPG

包含：

- 俯视角角色移动。
- Pixel Perfect Camera。
- Y-Sort。
- 多层 Tilemap。
- Rule Tile。
- Sprite Sheet。
- 对象交互。
- 角色逐帧动画。
- 脚本逻辑。
- 场景保存与加载。

用于验证像素风游戏能力。

### Demo C：2D Lighting Showcase

包含：

- Sprite Normal Map。
- Global Light。
- Point / Freeform Light。
- ShadowCaster2D。
- 动态光照。
- 粒子特效。
- Sprite Lit Material。
- Render Graph 集成。
- 性能统计。

用于验证高品质 2D 渲染。

三个 Demo 必须由 Nullus 自身的项目和资源系统加载，可以在 Editor 中运行，并能使用现有构建流程完成发布。

不得依赖未经提交的外部绝对路径资源。

---

## 九、测试与质量要求

每个阶段都要补充 GoogleTest 或现有测试框架支持的自动化测试。

重点覆盖：

**资源系统**

- Sprite 切片正确。
- Sprite 子资源 ID 稳定。
- Atlas 打包和反向映射正确。
- Reimport 后引用不损坏。
- 序列化、反序列化一致。

**渲染系统**

- Sprite Pivot 正确。
- Flip / Rotation / Scale 正确。
- Sorting Layer 正确。
- Sorting Group 正确。
- 透明排序正确。
- Pixel Perfect 不出现错误缩放。
- Atlas UV 不串色。
- Scene Picking 正确。
- 2D / 3D 混合顺序正确。

**物理系统**

- FixedStep 模拟正确。
- Rigidbody / Collider 生命周期正确。
- 触发器事件正确。
- Collision Begin / End 正确。
- Raycast / Overlap 正确。
- Scene 删除时不出现悬空对象。
- Rigidbody Transform 同步正确。
- Physics Layer Matrix 正确。
- 脚本回调不会破坏物理世界状态。

**Tilemap**

- 单 Tile 修改只更新必要 Chunk。
- Chunk 边界正确。
- 自动拼接规则正确。
- Tilemap Collider 更新正确。
- 大地图保存/加载正确。
- Undo/Redo 正确。

**动画与编辑器**

- Sprite 帧轨道播放正确。
- Animator 切换正确。
- 动画资源重新加载正确。
- Play Mode 进入/退出后没有悬空资源。
- Inspector 修改可正常保存。
- Prefab 实例和 Override 正确。

**构建与兼容**

- Editor 编译成功。
- Runtime 编译成功。
- 测试通过。
- 3D 示例无回归。
- 2D 项目可独立打包。
- 支持仓库声明的目标操作系统和渲染后端。

不得通过删除测试、禁用断言或屏蔽失败来宣称验收成功。

---

## 十、性能要求

重点建立可重复的 Benchmark，而不只是肉眼观察 FPS。

至少包括：

1. 10,000 Sprite 渲染测试。
2. 不同材质、Atlas 和排序层下的合批效率测试。
3. 大 Tilemap 滚动与局部编辑测试。
4. 大量 Collider 的物理模拟测试。
5. Sprite Atlas 与独立 Texture 的 Draw Call 对比。
6. 多 2D Light 场景的 GPU 开销测试。
7. Sprite 动画和骨骼变形性能测试。
8. 资源加载、热重载与内存测试。

记录：

- CPU Frame Time。
- GPU Frame Time。
- Draw Calls。
- Sprite Batches。
- Visible Sprites。
- Culling 数量。
- Tilemap Chunks。
- Physics Step Time。
- Active Bodies / Contacts。
- Texture Memory。
- Resource Load Time。

优先正确性、稳定性与架构兼容性，再进行性能优化。

不要为了达到某个 Draw Call 数字破坏 Sprite 透明排序。

如果硬件和 CI 条件无法运行真实 GPU Benchmark，应提供独立可运行的基准测试程序，并明确未执行的测试。

---

## 十一、代码实施规范

必须遵守以下规则：

1. 使用当前仓库已有代码风格和命名规范。
2. 保持 C++20 兼容。
3. 遵循现有 CMake 模块组织。
4. 不要引入不必要的第三方依赖。
5. 依赖版本和许可证必须明确。
6. 不要实现与现有引擎重复的基础系统。
7. 不要进行无关代码重构。
8. 不要将所有 2D 功能集中到单个巨型类。
9. 保持 Runtime / Editor / Platform 职责分离。
10. 对 GPU、Physics、Asset 的创建和销毁实现明确的生命周期管理。
11. 不要在 Editor 组件中保存不可序列化的运行时指针。
12. 不要在 Scene 文件里存储不稳定的底层资源 Handle。
13. 新增组件必须接入现有注册、反射、序列化与 Inspector 系统。
14. 新增资源必须接入 Asset Importer、Database、Cook 和 Build。
15. 新增 API 必须处理空资源、失效 ID、异常生命周期。
16. 运行时资源和编辑器资源不得互相错误依赖。
17. 保持现有 3D 功能兼容。
18. 不覆盖用户未提交的修改。
19. 不执行破坏性 Git 操作。
20. 不以空实现、永久 TODO、模拟返回值冒充功能完成。

当现有架构与建议设计冲突时，优先保证实际仓库架构的统一性，并记录设计偏差和原因。

---

## 十二、任务管理与进度恢复

在仓库现有文档目录下建立 2D 实施文档，路径按当前项目约定选择，例如：

```text
Docs/2D/
  Architecture.md
  FeatureMatrix.md
  ImplementationPlan.md
  AssetFormats.md
  Renderer2D.md
  Physics2D.md
  Tilemap.md
  Animation2D.md
  EditorIntegration.md
  Testing.md
  Progress.md
```

必须维护一张功能跟踪表，至少包含：

- 功能名称。
- 优先级。
- 所属阶段。
- 现有实现。
- 实际修改位置。
- 依赖关系。
- 状态：TODO / IN_PROGRESS / IMPLEMENTED / VERIFIED / BLOCKED。
- 验收测试。
- 已知问题。

`IMPLEMENTED` 不能直接等同于 `VERIFIED`。

只有完成编译、对应功能验证、必要测试和实际集成后才能标记为 `VERIFIED`。

大型任务允许在阶段边界中断，但必须留下：

- 已完成模块。
- 修改文件。
- 测试结果。
- 未完成事项。
- 当前阻塞。
- 下一阶段执行入口。

确保后续 Codex 会话可以继续，而不需要重新设计架构。

---

## 十三、每个阶段的交付格式

每完成一个阶段，必须输出：

### 1. 已完成工作

描述真实完成的功能，不要重复列出没有实现的规划。

### 2. 实际修改文件

列出新增和修改的文件及其用途。

### 3. 架构说明

说明功能如何集成到现有 Nullus 中，特别是组件、资源、渲染、物理、动画、脚本和编辑器的关系。

### 4. 编译及测试

给出实际执行过的构建和测试命令、结果、失败原因。

不允许未经执行就声称测试通过。

### 5. 编辑器验证

说明如何在 Nullus Editor 内操作和验证该功能。

### 6. 已知问题

明确说明哪些能力仍缺失、功能限制和未验证部分。

### 7. 下一阶段

根据实施计划推进到下一未完成模块。

---

## 十四、最终验收

整个任务完成的标志不是所有类都已经创建，而是能够完成以下完整工作流：

1. 启动 Nullus Editor。
2. 创建 2D Project。
3. 导入 Sprite Sheet。
4. 使用 Sprite Editor 切片。
5. 拖入 Sprite 创建角色。
6. 使用 Tile Palette 制作关卡。
7. 配置 SpriteRenderer 和 Sorting Layer。
8. 为角色添加 Rigidbody2D / Collider2D。
9. 使用 Animator 制作角色动画。
10. 通过 C++ / C# / Lua 编写角色控制逻辑。
11. 使用 Input Actions 接收键鼠或手柄输入。
12. 添加 Pixel Perfect Camera。
13. 添加 2D Light、Shadow 和 Particle。
14. 添加游戏 UI 和音效。
15. 通过 Editor Play Mode 运行调试。
16. 保存、关闭并重新打开项目，验证数据完整。
17. 使用 Nullus Build Pipeline 打包游戏。
18. 独立运行打包结果。
19. 重新导入原始资源，确认引用仍正确。
20. 验证原有 3D 工程没有回归。

最终输出完整的架构说明、资源格式说明、脚本 API、编辑器使用说明、示例工程、构建说明及测试结果。

---

## 十五、工作方式

请直接在当前仓库执行任务。

执行顺序：

**第一步：**检查 Git 状态、仓库结构与核心系统。

**第二步：**审计现有 Nullus 代码，判断哪些基础设施可以直接复用。

**第三步：**建立完整 2D 系统技术设计、功能矩阵和实施计划，明确模块依赖。

**第四步：**开始 Phase 1 的真实代码实现，不要只停留在分析和文档阶段。

**第五步：**完成编译、测试、实际编辑器集成和功能验证。

**第六步：**更新进度文档，按计划继续后续阶段。

如果某个设计依赖仓库内尚不存在的功能，应先实现必要的最小通用基础能力，再推进对应 2D 模块。

如果需要选择依赖或实现方式，优先选择与现有仓库兼容、长期可维护、能够跨平台运行的方案。

默认遵循这里已经明确的架构方向，除非遇到真正无法自行判断的重大冲突，否则不要因为普通实现细节而频繁暂停询问。

任务规模较大，必须采用可编译、可测试、可恢复的阶段式增量实施。不能为了表面上完成全部功能而提交大量未经验证的代码。

**现在开始：审计当前仓库，生成详细实施文档，并着手实现 Phase 1。**
