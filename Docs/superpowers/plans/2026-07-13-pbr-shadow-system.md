# 缓存式场景阴影系统实现计划

> **面向 AI 代理的工作者：** 必需子技能：使用 superpowers:subagent-driven-development（推荐）或 superpowers:executing-plans 逐任务实现此计划。步骤使用复选框（`- [ ]`）语法来跟踪进度。

**目标：** 为方向光、点光和聚光实现 Forward/Deferred 共用的缓存式阴影系统，在严格视图、draw 和三角形预算内保持完整阴影图发布，并让缺失阴影数据 fail closed。

**架构：** 保留现有每灯 16-word ABI，新增按 scene-light index 对齐的阴影 metadata、matrix 和固定 array-pool 资源。`SceneShadowProvider` 从不可变 scene snapshot 与 `RenderScene` 空间索引形成候选；纯 CPU `ShadowFramePlanner` 根据 `ShadowManager` 提供的 cache snapshot 决定优先级、预算、原子更新组和 suppression；`ShadowManager` 持有 pool/cache/last-good/LRU，`ShadowRenderer` 记录获准视图，FrameGraph 仍是调度权威。所有光照路径调用 `Shadows.hlsl` 的九点 PCF，shadow request 没有有效 metadata 时直接跳过该灯。

**技术栈：** C++20、Nullus RenderScene/FrameGraph/RHI、HLSL/ShaderLab、DX12、GoogleTest、MetaParser。

---

## 文件结构

- 创建：`Runtime/Rendering/Shadows/ShadowTypes.h`，稳定 POD、预算、metadata ABI 和 telemetry。
- 创建：`Runtime/Rendering/Shadows/ShadowFramePlanner.h/.cpp`，无 RHI 依赖的优先级、预算和完整更新组决策。
- 创建：`Runtime/Rendering/Shadows/ShadowManager.h/.cpp`，方向/聚光/点光固定 pool、cache、last-good、LRU 与 generation。
- 创建：`Runtime/Engine/Rendering/SceneShadowProvider.h/.cpp`，不可变 light/caster capture、空间查询和 fingerprint。
- 创建：`Runtime/Engine/Rendering/ShadowRenderer.h/.cpp`，从 prepared shadow frame 记录 depth-only draw commands。
- 创建：`Runtime/Rendering/FrameGraph/SceneRenderGraphBuilderShadows.h/.cpp`，导入 pool 并发射精确 layer 的 shadow pass。
- 创建：`App/Assets/Engine/Shaders/ShadowCaster.hlsl`，内建 depth-only 与 AlphaTest caster。
- 创建：`Tests/Unit/ShadowPlannerTests.cpp`，预算、原子提交、last-good、LRU 和 fail-closed 单元测试。
- 创建：`Tests/Unit/SceneShadowProviderTests.cpp`，组件、capture、空间候选和 invalidation 测试。
- 创建：`Tests/Rendering/DX12ShadowRenderingTests.cpp`，方向/点/聚光及漏光图像夹具。
- 修改：`Runtime/Engine/Components/LightComponent.h/.cpp`，反射属性 `CastShadows`。
- 修改：`Runtime/Engine/Components/MeshRenderer.h/.cpp`，反射属性 `CastShadows`/`ReceiveShadows` 与 render revision。
- 修改：`Runtime/Rendering/Entities/Light.h/.cpp`，运行时 cast flag 与 shadow revision。
- 修改：`Runtime/Rendering/Data/LightingDescriptor.h`，不可变 shadow light identity 对齐信息。
- 修改：`Runtime/Engine/Rendering/SceneLightingProvider.cpp`，捕获 stable `Object::InstanceID` 和 shadow settings。
- 修改：`Runtime/Engine/Rendering/LightGridPrepass.h/.cpp`，上传平行 shadow buffers，保持 16-word record 不变。
- 修改：`Runtime/Engine/Rendering/RenderScene.h/.cpp`，按 shadow frustum/volume 查询 retained primitives 与复用 draw command。
- 修改：`Runtime/Engine/Rendering/BaseSceneRenderer.h/.cpp`，持有并驱动 `SceneShadowProvider`/pool/planner。
- 修改：`Runtime/Rendering/FrameGraph/SceneRenderGraphBuilderForward.cpp`，在 light-grid/scene 前接入 shadow passes。
- 修改：`Runtime/Rendering/FrameGraph/SceneRenderGraphBuilderDeferred.cpp`，在 light-grid/GBuffer 前接入 shadow passes。
- 修改：`Runtime/Rendering/Context/ThreadedRenderingLifecycle.h` 及 `RenderScenePackageBuilder.cpp`，携带不可变 shadow plan。
- 修改：`Runtime/Rendering/Settings/DriverSettings.h`，持有并捕获命名的 `ShadowSettings` balanced profile。
- 修改：`Runtime/Rendering/Resources/ShaderType.cpp`，注册 shadow caster shader 类型/排列。
- 修改：`App/Assets/Engine/Shaders/NullusShaderLibrary/Shadows.hlsl`，metadata 校验、CSM、cube/spot sampling 与 9-tap PCF。
- 修改：`App/Assets/Engine/Shaders/LightGridCommon.hlsli`，Forward/Deferred/Phong 使用真实 visibility。
- 修改：`App/Assets/Engine/Shaders/ShaderLab/StandardPBR.shader`，添加 `ShadowCaster` pass 与 renderer-owned keyword。
- 修改：`Runtime/Rendering/Data/FrameInfo.h`、`Runtime/Rendering/Core/RendererStats.h/.cpp`，阴影 telemetry。
- 修改：`Tests/Unit/LightingDataProviderTests.cpp`、`RenderSceneCacheTests.cpp`、`FrameGraphSceneTargetsTests.cpp`、`RendererStatsTests.cpp`、`AssetMaterialConversionTests.cpp`，集成契约。
- 修改：`Tests/Performance/RenderScenePerformanceTests.cpp`，静态缓存、移动 light/caster 和过预算性能路径。

### 任务 1：增加可序列化的 cast/receive 属性

**文件：**
- 修改：`Runtime/Engine/Components/LightComponent.h/.cpp`
- 修改：`Runtime/Engine/Components/MeshRenderer.h/.cpp`
- 修改：`Runtime/Rendering/Entities/Light.h/.cpp`
- 修改：`Tests/Unit/SceneShadowProviderTests.cpp`

- [ ] **步骤 1：写默认值、copy、revision 和反射失败测试**

```cpp
TEST(SceneShadowProviderTests, ShadowFlagsDefaultTrueAndCopyWithComponents)
{
    LightComponent light;
    MeshRenderer renderer;
    EXPECT_TRUE(light.GetCastShadows());
    EXPECT_TRUE(renderer.GetCastShadows());
    EXPECT_TRUE(renderer.GetReceiveShadows());
    MeshRenderer copy(renderer);
    EXPECT_TRUE(copy.GetCastShadows());
    EXPECT_TRUE(copy.GetReceiveShadows());
}

TEST(SceneShadowProviderTests, ShadowFlagMutationAdvancesRenderRevision)
{
    MeshRenderer renderer;
    const auto before = renderer.GetRenderRevision();
    renderer.SetCastShadows(false);
    EXPECT_GT(renderer.GetRenderRevision(), before);
}
```

同时用反射数据库断言三个 PROPERTY 可发现、可写、序列化 round-trip 后保留 false。

- [ ] **步骤 2：构建确认红灯**

```powershell
cmake --build Build --config Debug --target NullusUnitTests ReflectionTest -- /m:4
```

预期：编译失败，三个 getter/setter 尚不存在。

- [ ] **步骤 3：实现组件 API，不编辑 Gen**

在头文件给 getter/setter 成对添加同名 `PROPERTY(castShadows)` / `PROPERTY(receiveShadows)` 元数据；字段默认值均为 `true`。`MeshRenderer` setter 仅在值变化时调用 `MarkRenderStateChanged()`；copy ctor/assignment 复制两个持久 flag。`LightComponent` 将值写入 `Render::Entities::Light::castShadows` 并递增 `shadowRevision`。

```cpp
PROPERTY(castShadows)
FUNCTION() bool GetCastShadows() const;
PROPERTY(castShadows)
FUNCTION() void SetCastShadows(bool enabled);

PROPERTY(receiveShadows)
FUNCTION() bool GetReceiveShadows() const;
PROPERTY(receiveShadows)
FUNCTION() void SetReceiveShadows(bool enabled);
```

- [ ] **步骤 4：走正常 MetaParser 构建并验证**

```powershell
cmake --build Build --config Debug --target NullusUnitTests ReflectionTest -- /m:4
.\Build\bin\Debug\NullusUnitTests.exe --gtest_filter=SceneShadowProviderTests.*ShadowFlag*
.\Build\bin\Debug\ReflectionTest.exe
```

预期：全部 `PASS`；生成文件由 MetaParser 构建步骤更新，不手工修改。

- [ ] **步骤 5：提交组件属性**

```powershell
git add Runtime/Engine/Components/LightComponent.h Runtime/Engine/Components/LightComponent.cpp Runtime/Engine/Components/MeshRenderer.h Runtime/Engine/Components/MeshRenderer.cpp Runtime/Rendering/Entities/Light.h Runtime/Rendering/Entities/Light.cpp Tests/Unit/SceneShadowProviderTests.cpp
git add Runtime/Engine/Gen/Components/LightComponent.generated.cpp Runtime/Engine/Gen/Components/LightComponent.generated.h Runtime/Engine/Gen/Components/MeshRenderer.generated.cpp Runtime/Engine/Gen/Components/MeshRenderer.generated.h
git commit -m "feat: add shadow casting and receiving flags"
```

提交前只添加 MetaParser 实际改动的生成文件；若构建没有改变它们，不执行第二条 `git add`。

### 任务 2：定义阴影 ABI 且保持 light record 不变

**文件：**
- 创建：`Runtime/Rendering/Shadows/ShadowTypes.h`
- 修改：`Runtime/Rendering/Data/LightingDescriptor.h`
- 修改：`Runtime/Engine/Rendering/SceneLightingProvider.cpp`
- 修改：`Runtime/Engine/Rendering/LightGridPrepass.h/.cpp`
- 修改：`Runtime/Rendering/Settings/DriverSettings.h`
- 修改：`Tests/Unit/LightingDataProviderTests.cpp`

- [ ] **步骤 1：写 16-word 与索引对齐失败测试**

```cpp
EXPECT_EQ(LightGridPrepass::kPackedLightWordStride, 16u);
EXPECT_EQ(frame.forwardLocalLightData.size(), frame.capturedLights.size() * 16u);
ASSERT_EQ(frame.shadowMetadata.size(), frame.capturedLights.size());
EXPECT_EQ(frame.capturedLights[2].lightId, lightObject.GetInstanceID());
EXPECT_EQ(frame.shadowMetadata[2].viewCount, 6u);
```

- [ ] **步骤 2：运行测试确认缺少 metadata**

```powershell
.\Build\bin\Debug\NullusUnitTests.exe --gtest_filter=LightingDataProviderTests.*Shadow*:LightingDataProviderTests.*PackedLight*
```

预期：编译失败，`ShadowLightMetadata` 尚未定义。

- [ ] **步骤 3：定义固定布局 POD**

`ShadowTypes.h` 定义：

```cpp
enum class ShadowMapKind : uint32_t { None, DirectionalCascade, Spot, PointFaces };
enum ShadowMetadataFlags : uint32_t {
    ShadowRequested = 1u << 0u,
    ShadowPublished = 1u << 1u,
    ShadowSuppressed = 1u << 2u
};
struct alignas(16) ShadowLightMetadata {
    uint32_t flags = 0u;
    uint32_t mapKind = 0u;
    uint32_t poolSlot = 0u;
    uint32_t firstMatrixIndex = 0u;
    uint32_t viewCount = 0u;
    uint32_t readyState = 0u;
    uint32_t padding0 = 0u;
    uint32_t padding1 = 0u;
    float nearPlane = 0.0f;
    float farPlane = 0.0f;
    float receiverBiasTexels = 0.0f;
    float normalOffsetTexels = 0.0f;
    float cascadeSplits[4]{};
};
static_assert(sizeof(ShadowLightMetadata) == 64u);
```

同时把 `LightGridPrepass.cpp` 匿名命名空间里的 `kLightWordStride` 提升为 `LightGridPrepass::kPackedLightWordStride = 16u`，packing/reserve 和测试只引用该 C++ SSoT；HLSL 的 `NLS_LIGHT_WORD_STRIDE` 由源码契约测试锁定为同值。CPU capture 另存 `Object::InstanceID lightId`、light index、fingerprints 和 setting；GPU metadata 不塞进 16-word light buffer。`ShadowFrameData` 保存 pool inverse resolutions、有效 light/matrix counts 和 active RHI depth convention。

- [ ] **步骤 4：定义并测试 balanced `ShadowSettings`**

在 `DriverSettings.h` 的现有 renderer settings ownership 中加入 `ShadowSettings`，精确默认值为：4 cascades，2048/1024/512 resolutions，1/8/4 light capacities，12 views，2048 draws，2,000,000 triangles，split lambda 0.70，transition 0.10，max distance 200，bias 1.25/1.75/0.50。测试构造默认 `DriverSettings` 后逐字段断言；frame capture 按值复制它，不建立独立 editor-thread 设置通道。

- [ ] **步骤 5：捕获 immutable light identity**

`SceneLightingProvider` 从 light component 所属 `GameObject::GetInstanceID()` 捕获稳定 ID、cast flag 和 revision；`PrepareRenderScenePackage` 只复制 snapshot 值，不在 render thread 解引用 component。

- [ ] **步骤 6：上传平行 buffers**

`LightGridPrepass::FrameData` 新增 `shadowMetadata`、`shadowMatrices`，并固定 pass-space-1 bindings：metadata=`t3`、matrices=`t4`、directional array=`t5`、spot array=`t6`、point-face array=`t7`、comparison sampler=`s0`、`ShadowFrameData`=`b1`；继续断言 `kLightWordStride == 16u`。

- [ ] **步骤 7：构建并运行数据契约测试**

```powershell
cmake --build Build --config Debug --target NullusUnitTests -- /m:4
.\Build\bin\Debug\NullusUnitTests.exe --gtest_filter=LightingDataProviderTests.*
```

预期：`PASS`，现有 light packing 的逐 word 预期不变。

- [ ] **步骤 8：提交阴影数据 ABI**

```powershell
git add Runtime/Rendering/Shadows/ShadowTypes.h Runtime/Rendering/Data/LightingDescriptor.h Runtime/Engine/Rendering/SceneLightingProvider.cpp Runtime/Engine/Rendering/LightGridPrepass.h Runtime/Engine/Rendering/LightGridPrepass.cpp Runtime/Rendering/Settings/DriverSettings.h Tests/Unit/LightingDataProviderTests.cpp
git commit -m "feat: add parallel shadow light metadata"
```

### 任务 3：用纯 CPU planner 锁定预算与原子发布

**文件：**
- 创建：`Runtime/Rendering/Shadows/ShadowFramePlanner.h/.cpp`
- 创建：`Tests/Unit/ShadowPlannerTests.cpp`

- [ ] **步骤 1：写默认预算和完整组失败测试**

```cpp
const ShadowBudget budget{};
EXPECT_EQ(budget.maxViews, 12u);
EXPECT_EQ(budget.maxDraws, 2048u);
EXPECT_EQ(budget.maxTriangles, 2'000'000u);

ShadowUpdateGroup point{.viewCount=6u, .drawCount=2048u, .triangleCount=1'999'998u};
auto plan = planner.Plan({point}, budget);
EXPECT_EQ(plan.scheduledGroups.size(), 1u);
EXPECT_EQ(plan.scheduledViews, 6u);
```

再添加超出任一剩余预算时整组 delayed、点光 6 面不可拆、新方向光 4 cascade 不可拆的测试。

- [ ] **步骤 2：运行确认红灯**

运行 `ShadowPlannerTests.*Budget*`，预期编译失败。

- [ ] **步骤 3：实现稳定优先级和三预算 admission**

候选排序键必须按设计顺序显式比较：已占 slot 的 incomplete new、CSM near-to-far、visible locals（projected influence、intensity、distance、stable ID）、maintenance。`CanAdmit` 同时检查三个无符号饱和预算，禁止局部截断 caster list。

- [ ] **步骤 4：写 last-good/initial suppression 失败测试**

```cpp
EXPECT_TRUE(plan.newUnpublishedLights[0].suppressDirectLight);
EXPECT_EQ(plan.delayedDirtyLights[0].publishedGeneration, lastGoodGeneration);
EXPECT_EQ(plan.failedUpdates[0].publishedGeneration, lastGoodGeneration);
```

- [ ] **步骤 5：实现 staged commit 状态机**

planner 以不可变 cache snapshot 模拟 `Unallocated -> AllocatedUnpublished -> Staging -> Published` 转换并输出 decisions，自身不持有 RHI handle 或跨帧可变状态。只有完整组才能产生 publish candidate；失败或延迟 decision 指向 last-good。首次方向光必须四 cascade 原子发布，已有完整 CSM 可按 snapped projection/caster fingerprint 只更新 dirty cascades，near 优先，far cascade 延迟不使其余 last-good cascades 失效。首次未发布、容量 suppressed、metadata invalid 均设置 `suppressDirectLight=true`。

- [ ] **步骤 6：写 LRU 与 in-flight 安全测试并实现**

同优先级低价值 slot 按 `lastUsedFrame` 淘汰，但 `retireAfterFrame > completedFrame` 的 slot 不可重用。验证 point/directional group 的所有 layer 同属一个 owner generation。

- [ ] **步骤 7：运行 planner 全套测试**

```powershell
cmake --build Build --config Debug --target NullusUnitTests -- /m:4
.\Build\bin\Debug\NullusUnitTests.exe --gtest_filter=ShadowPlannerTests.*
```

预期：`PASS`，边界值恰好等于 12/2048/2,000,000 可调度，多 1 必须 delayed。

- [ ] **步骤 8：提交 planner**

```powershell
git add Runtime/Rendering/Shadows/ShadowFramePlanner.h Runtime/Rendering/Shadows/ShadowFramePlanner.cpp Tests/Unit/ShadowPlannerTests.cpp
git commit -m "feat: plan atomic shadow updates within budgets"
```

### 任务 4：从 retained RenderScene 查询完整 caster 集

**文件：**
- 创建：`Runtime/Engine/Rendering/SceneShadowProvider.h/.cpp`
- 修改：`Runtime/Engine/Rendering/RenderScene.h/.cpp`
- 修改：`Tests/Unit/SceneShadowProviderTests.cpp`
- 修改：`Tests/Unit/RenderSceneCacheTests.cpp`

- [ ] **步骤 1：写 off-screen caster 与资格失败测试**

构造主相机不可见但在 point light volume 内的墙，断言 shadow query 返回它；再验证 inactive、`CastShadows=false`、Transparent、Decal 被排除，而 Opaque 和 `_ALPHATEST_ON` 被接受。

```cpp
EXPECT_THAT(result.casters, Contains(occluderHandle));
EXPECT_THAT(result.casters, Not(Contains(transparentHandle)));
EXPECT_THAT(result.casters, Not(Contains(decalHandle)));
```

- [ ] **步骤 2：运行确认当前只能拿主相机 visible list**

运行 `SceneShadowProviderTests.*Caster*`，预期失败。

- [ ] **步骤 3：增加只读 shadow volume query**

复用 `RenderScene` static spatial index 与 dirty dynamic overlay，提供 `QueryShadowCasters(const ShadowVolume&, ShadowCasterScratch&) const`。返回 retained primitive handles；不得每 light 全场扫描，不得在 inner loop 单独 heap allocate。drawable capture 把 `MeshRenderer::CastShadows/ReceiveShadows` 写入稳定 flags；dynamic instancing key 同时包含两位，任一不同都拆分 draw。

- [ ] **步骤 4：复用 draw command 与代价估算**

每 caster 记录 `drawCount`、`indexCount / 3 * instanceCount`、mesh/material/shader/resource/LOD/HLOD/render revisions。`Cull Off` 只改变 raster cull，不重复两次 draw；AlphaTest 保存 base/opacity/cutoff bindings。

- [ ] **步骤 5：写 fingerprint invalidation 矩阵测试**

逐项改变 light transform/type/range/cone/settings、caster transform/mesh/LOD/HLOD/active/flag/bounds、material surface/cull/AlphaTest/cutoff/texture/shader/resource generation，断言只影响相交 local lights。相同输入第二帧必须 cache hit 且零 scheduled view。

- [ ] **步骤 6：运行空间与缓存测试**

```powershell
.\Build\bin\Debug\NullusUnitTests.exe --gtest_filter=SceneShadowProviderTests.*:RenderSceneCacheTests.*Shadow*
```

预期：`PASS`；telemetry 中 `fullScanCandidateCount` 不因 shadow lights 增加。

- [ ] **步骤 7：提交 caster collection**

```powershell
git add Runtime/Engine/Rendering/SceneShadowProvider.h Runtime/Engine/Rendering/SceneShadowProvider.cpp Runtime/Engine/Rendering/RenderScene.h Runtime/Engine/Rendering/RenderScene.cpp Tests/Unit/SceneShadowProviderTests.cpp Tests/Unit/RenderSceneCacheTests.cpp
git commit -m "feat: query cached shadow casters from render scene"
```

### 任务 5：实现稳定 CSM 与 local-light 矩阵

**文件：**
- 修改：`Runtime/Engine/Rendering/SceneShadowProvider.cpp`
- 修改：`Runtime/Rendering/Shadows/ShadowTypes.h`
- 修改：`Tests/Unit/SceneShadowProviderTests.cpp`

- [ ] **步骤 1：写矩阵与 cascade 失败测试**

四个方向 cascade 使用 practical split，断言 split 严格递增、最后覆盖 camera far plane；小于一个 shadow texel 的 camera 平移不改变 snapped light-space origin。spot 使用外锥角，point 生成固定顺序 `+X,-X,+Y,-Y,+Z,-Z` 六矩阵。

- [ ] **步骤 2：实现固定分辨率常量**

```cpp
inline constexpr uint32_t kDirectionalCascadeCount = 4u;
inline constexpr uint32_t kDirectionalShadowResolution = 2048u;
inline constexpr uint32_t kSpotShadowResolution = 1024u;
inline constexpr uint32_t kPointShadowResolution = 512u;
```

split 从 camera near 到 `min(cameraFar, 200.0f)`，practical split lambda 固定为 0.70。方向矩阵按 cascade ortho extent / 2048 对 light-space XY 做 texel snapping；保存 split near/far 与 10% overlap blend band，square extent 只有跨过量化边界时才变化。

- [ ] **步骤 3：实现 bias 数据**

每 view metadata 保存 depth bias、slope-scaled bias 和 receiver normal offset；balanced profile 分别为 1.25 texels、1.75 slope multiplier、0.50 texel world-normal offset。receiver offset 使用 oriented geometry normal，不使用 normal-map 法线。

- [ ] **步骤 4：运行矩阵测试并提交**

```powershell
.\Build\bin\Debug\NullusUnitTests.exe --gtest_filter=SceneShadowProviderTests.*Matrix*:SceneShadowProviderTests.*Cascade*
git add Runtime/Engine/Rendering/SceneShadowProvider.cpp Runtime/Rendering/Shadows/ShadowTypes.h Tests/Unit/SceneShadowProviderTests.cpp
git commit -m "feat: build stable shadow view matrices"
```

预期：所有矩阵有限且可逆，snapping 与 split blend 测试 `PASS`。

### 任务 6：有界预热固定深度 array pool 与 staging layers

**文件：**
- 创建：`Runtime/Rendering/Shadows/ShadowManager.h/.cpp`
- 修改：`Tests/Unit/ShadowPlannerTests.cpp`
- 修改：`Tests/Unit/DX12TextureViewUtilsTests.cpp`
- 修改：`Tests/Unit/DX12SamplerUtilsTests.cpp`

- [ ] **步骤 1：写资源描述失败测试**

断言三类 pool 使用 `Depth32F | DepthStencilAttachment | Sampled`：directional active=4 layers（1 light）、spot active=8 layers（8 lights）、point active=24 layers（4 lights）；另有可复用 staging directional=4、spot=1、point=6 layers。比较 sampler 为 `LESS_EQUAL`，active 120 MiB、staging 最大 74 MiB、总峰值 194 MiB。

- [ ] **步骤 2：实现 lazy pool generation**

`ShadowManager` 通过现有 renderer resource preparation path 分三步预热：frame N directional，frame N+1 spot，frame N+2 point 与 staging，每帧最多一步。测试连续调用 preparation 三次时每次只增加一个 ready step，普通 scene/thumbnail 请求不得同步创建整池；对应 pool 未 ready 时 shadow-request light suppressed。manager 记录 pool `generation`，并在 completion 后原子提交 planner decision、last-good metadata/matrices 和 LRU；device loss 或 capability failure 递增 generation 并使所有 slot invalid。资源创建失败每 generation 只记录一次 diagnostic；capacity/resolution 可降低，但 zero capacity 是显式 unsupported。

- [ ] **步骤 3：实现 exact-layer view**

active 与 staging 均使用 `Texture2DArray`。为每个 scheduled view 创建 `RHITextureViewDesc`，`baseArrayLayer=staging.firstLayer+faceOrCascade`、`layerCount=1`、`mipCount=1`。测试 DX12 DSV/SRV descriptor 不覆盖相邻 cached layer；只有最高优先级 directional light 获得默认 CSM active slot。

- [ ] **步骤 4：运行 RHI 测试**

```powershell
.\Build\bin\Debug\NullusUnitTests.exe --gtest_filter=ShadowPlannerTests.*Pool*:DX12TextureViewUtilsTests.*Shadow*:DX12SamplerUtilsTests.*Comparison*
```

预期：`PASS`。

- [ ] **步骤 5：提交 pool**

```powershell
git add Runtime/Rendering/Shadows/ShadowManager.h Runtime/Rendering/Shadows/ShadowManager.cpp Tests/Unit/ShadowPlannerTests.cpp Tests/Unit/DX12TextureViewUtilsTests.cpp Tests/Unit/DX12SamplerUtilsTests.cpp
git commit -m "feat: allocate cached shadow map pools"
```

### 任务 7：加入 ShadowCaster shader 与材质选择

**文件：**
- 创建：`App/Assets/Engine/Shaders/ShadowCaster.hlsl`
- 修改：`App/Assets/Engine/Shaders/ShaderLab/StandardPBR.shader`
- 修改：`Runtime/Rendering/Resources/ShaderType.cpp`
- 修改：`Runtime/Rendering/Resources/Material.h/.cpp`
- 修改：`Tests/Unit/AssetMaterialConversionTests.cpp`

- [ ] **步骤 1：写 pass 选择失败测试**

要求 built-in Standard PBR 和 ShaderLab Standard PBR 存在 `LightMode = ShadowCaster`；custom opaque 无 pass 时选择内建 depth fallback；custom AlphaTest 无兼容 pass 时选择 conservative opaque fallback 并产生一次 diagnostic。

- [ ] **步骤 2：实现 caster variants**

`ShadowCaster.hlsl` 顶点只输出 clip position/UV；Opaque pixel shader 无颜色输出，AlphaTest 变体采样 base/opacity 并执行与 visible pass 相同的 `_Cutoff`。排列覆盖 `_ALPHATEST_ON` 与 renderer-owned `MAIN_LIGHT_SHADOWS`。

- [ ] **步骤 3：继承 cull 与 keywords**

shadow material variant 复制 source `GetShaderLabKeywords()`、texture/sampler/property bindings 和 front/back culling。`Cull Off` 生成 `cullEnabled=false`，确保两面投射；普通 Transparent/Decal 不产生 caster draw。

- [ ] **步骤 4：运行 pass/编译测试**

```powershell
.\Build\bin\Debug\NullusUnitTests.exe --gtest_filter=AssetMaterialConversionTests.*ShadowCaster*:ShaderLabPassAndPipelineTests.*ShadowCaster*:ShaderCompilerTests.*Shadow*
```

预期：DXIL/SPIR-V 编译成功，SPIR-V 通过 `spirv-val`；工具缺失只能 `GTEST_SKIP` 并记录。

- [ ] **步骤 5：提交 caster shader**

```powershell
git add App/Assets/Engine/Shaders/ShadowCaster.hlsl App/Assets/Engine/Shaders/ShaderLab/StandardPBR.shader Runtime/Rendering/Resources/ShaderType.cpp Runtime/Rendering/Resources/Material.h Runtime/Rendering/Resources/Material.cpp Tests/Unit/AssetMaterialConversionTests.cpp
git commit -m "feat: add two-sided alpha-tested shadow casters"
```

### 任务 8：把 shadow passes 接入 FrameGraph

**文件：**
- 创建：`Runtime/Rendering/FrameGraph/SceneRenderGraphBuilderShadows.h/.cpp`
- 创建：`Runtime/Engine/Rendering/ShadowRenderer.h/.cpp`
- 修改：`Runtime/Rendering/FrameGraph/SceneRenderGraphBuilderForward.cpp`
- 修改：`Runtime/Rendering/FrameGraph/SceneRenderGraphBuilderDeferred.cpp`
- 修改：`Runtime/Rendering/Context/ThreadedRenderingLifecycle.h`
- 修改：`Runtime/Rendering/Context/RenderScenePackageBuilder.cpp`
- 修改：`Tests/Unit/FrameGraphSceneTargetsTests.cpp`

- [ ] **步骤 1：写 pass 顺序和 layer dependency 失败测试**

Forward 预期 `ShadowDepth -> LightGrid -> Opaque`；Deferred 预期 `ShadowDepth -> LightGrid -> GBuffer -> Decal -> DeferredLighting`。每个 shadow writer 只声明一个 array layer，sampling pass 依赖 published layers。

- [ ] **步骤 2：运行测试确认 shadow pass 缺失**

运行 `FrameGraphSceneTargetsTests.*Shadow*`，预期失败。

- [ ] **步骤 3：捕获 immutable execution plan**

`RenderScenePackage` 保存 `shared_ptr<const PreparedShadowFrame>`，其中只有 RHI handles、prepared draw commands、matrices、layer ranges、completion tokens；render thread 不访问 mutable component、scene 或 planner。

- [ ] **步骤 4：发射已调度视图**

`AddPreparedShadowPasses` 为 planner admitted views 建 staging depth pass；pool textures 声明所需 `CopySrc/CopyDst` usage，setup 声明 exact staging subresource depth write，execute 调用 `ShadowRenderer::RecordPreparedView` 记录完整 caster list。`ShadowRenderer` 只消费 prepared draw commands/caster bindings，不做 scene query、budget 或 cache mutation。完整组所有 views 成功后增加 copy passes，把 staging layers 复制到 active layers，再发布匹配 matrices/metadata。spot 更新串行复用唯一 staging layer；零 scheduled view 时只 import 已发布 pools，不发射 shadow draw/copy。

- [ ] **步骤 5：添加发布完成回执**

同一 frame 内，每个更新组必须形成 `staging depth writes -> staging-to-active copies -> lighting samples` 的精确 subresource 依赖；该 frame 上传的 matrices/metadata 必须指向复制后的同一 generation，不能用旧矩阵采样已覆盖的 active layer。多个 spot/point 更新通过 `depth -> copy -> depth -> copy` 串行复用 staging layers。GPU completion token 由 `ShadowManager` 观察后才提交 CPU durable staged/last-good generation 与 LRU 状态；纯策略 `ShadowFramePlanner` 不接收回执、不持有跨帧状态，且没有路径等待 fence。

- [ ] **步骤 6：运行 FrameGraph 测试**

```powershell
.\Build\bin\Debug\NullusUnitTests.exe --gtest_filter=FrameGraphSceneTargetsTests.*Shadow*:ThreadedRenderingLifecycleTests.*Shadow*
```

预期：`PASS`，没有 unrelated layer transition 或 render-thread mutable dereference。

- [ ] **步骤 7：提交 FrameGraph 接入**

```powershell
git add Runtime/Rendering/FrameGraph/SceneRenderGraphBuilderShadows.h Runtime/Rendering/FrameGraph/SceneRenderGraphBuilderShadows.cpp Runtime/Engine/Rendering/ShadowRenderer.h Runtime/Engine/Rendering/ShadowRenderer.cpp Runtime/Rendering/FrameGraph/SceneRenderGraphBuilderForward.cpp Runtime/Rendering/FrameGraph/SceneRenderGraphBuilderDeferred.cpp Runtime/Rendering/Context/ThreadedRenderingLifecycle.h Runtime/Rendering/Context/RenderScenePackageBuilder.cpp Tests/Unit/FrameGraphSceneTargetsTests.cpp Tests/Unit/ThreadedRenderingLifecycleTests.cpp
git commit -m "feat: schedule cached shadow passes in frame graph"
```

### 任务 9：实现共享采样、PCF 与 fail-closed direct lighting

**文件：**
- 修改：`App/Assets/Engine/Shaders/NullusShaderLibrary/Shadows.hlsl`
- 修改：`App/Assets/Engine/Shaders/LightGridCommon.hlsli`
- 修改：`App/Assets/Engine/Shaders/StandardPBR.hlsl`
- 修改：`App/Assets/Engine/Shaders/DeferredLighting.hlsl`
- 修改：`App/Assets/Engine/Shaders/ShaderLab/StandardPBR.shader`
- 修改：`Runtime/Engine/Rendering/LightGridPrepass.cpp`
- 修改：`Tests/Unit/PBRShadingContractTests.cpp`

- [ ] **步骤 1：写 shader fail-closed 失败契约**

要求 `NLSEvaluateShadowVisibility(lightIndex, worldPosition, geometryNormalWS)` 为唯一 visibility 入口。定义 `NLSShadowVisibilityResult { float visibility; uint suppressDirectLight; }`；测试 metadata requested 但 `Published` 缺失、matrix 非有限、layer 越界、capacity suppressed 时 `suppressDirectLight != 0`，direct loop 必须 `continue`，不能当作 visibility 1。Forward opaque、Forward transparent、Deferred lighting 和 Phong 都必须覆盖；`ReceiveShadows=false` 在每条 receiver 路径精确返回 visibility 1 且不抑制非阴影 direct light。

- [ ] **步骤 2：实现 9-tap PCF**

方向/spot 使用固定 3x3 comparison taps；point 用 light-to-fragment 方向构造稳定 tangent basis，九个角向 offset 都重新投影到 canonical face/UV/layer，允许跨 cube-face 边界而不产生 seam。所有 UV/depth/layer/matrix 检查在采样前完成；无 shadow request 返回 visibility 1，receiver flag false 返回 1。投影/Y 轴和 compare convention 只通过 active RHI backend helpers 提供，不在调用点分叉。

```hlsl
NLSShadowVisibilityResult shadow = NLSEvaluateShadowVisibility(...);
if (shadow.suppressDirectLight)
    continue;
direct += NLSEvaluateCookTorranceDirect(..., shadow.visibility);
```

- [ ] **步骤 3：实现 CSM 选择与 blend**

按正 view-space distance 选 cascade，在 10% overlap band 同时采样相邻 cascade，并用 `saturate((depth - blendStart) / blendWidth)` 做线性混合；out-of-range 或 incomplete initial CSM 抑制方向 direct light。

- [ ] **步骤 4：renderer-owned keyword 与 variant failure**

由 renderer 为支持阴影的 pass 启用 `MAIN_LIGHT_SHADOWS`，不依赖 source material opt-in。variant 创建失败时 native light metadata 将所有 `CastShadows` light 标为 suppressed，非 shadow light 保持正常。

- [ ] **步骤 5：运行 shader 与集成测试**

```powershell
.\Build\bin\Debug\NullusUnitTests.exe --gtest_filter=PBRShadingContractTests.*Shadow*:LightingDataProviderTests.*Shadow*:AssetMaterialConversionTests.*MAIN_LIGHT_SHADOWS*
```

预期：`PASS`；Forward opaque/transparent、Deferred、Phong 都消费同一 metadata/visibility 函数。

- [ ] **步骤 6：提交采样路径**

```powershell
git add App/Assets/Engine/Shaders/NullusShaderLibrary/Shadows.hlsl App/Assets/Engine/Shaders/LightGridCommon.hlsli App/Assets/Engine/Shaders/StandardPBR.hlsl App/Assets/Engine/Shaders/DeferredLighting.hlsl App/Assets/Engine/Shaders/ShaderLab/StandardPBR.shader Runtime/Engine/Rendering/LightGridPrepass.cpp Tests/Unit/PBRShadingContractTests.cpp Tests/Unit/LightingDataProviderTests.cpp Tests/Unit/AssetMaterialConversionTests.cpp
git commit -m "feat: apply fail-closed shadow visibility in PBR"
```

### 任务 10：接入 renderer 生命周期、统计与故障恢复

**文件：**
- 修改：`Runtime/Engine/Rendering/BaseSceneRenderer.h/.cpp`
- 修改：`Runtime/Rendering/Data/FrameInfo.h`
- 修改：`Runtime/Rendering/Core/RendererStats.h/.cpp`
- 修改：`Project/Editor/Panels/FrameInfo.cpp`
- 修改：`Tests/Unit/RendererStatsTests.cpp`
- 修改：`Tests/Unit/ShadowPlannerTests.cpp`

- [ ] **步骤 1：写 telemetry 聚合/重置失败测试**

验证 requested/scheduled/completed/delayed views，estimated/submitted draws/triangles，hits/invalidations/evictions/swaps，pool use/suppression/stale frames、queried candidates、accepted casters、touched lights、reused commands，以及 planning CPU/GPU ns 在 frame 汇总后正确、下一帧清零。

- [ ] **步骤 2：实现 `ShadowTelemetry`**

`FrameInfo` 新增一个聚合 POD；`RendererStats::RecordShadowTelemetry` 对 counter 求和、对 pool usage gauge 取最新值、对 times 求和。面板只在 render diagnostics 开启时展示详细行。

- [ ] **步骤 3：实现 estimator error**

完成记录时计算 submitted-estimated draws/triangles，任何 undercount 增加 diagnostic counter；submitted 绝不得超过 admitted budget，否则测试失败并拒绝发布该 staged group。

- [ ] **步骤 4：实现 device/pool generation 恢复**

device loss 清空 GPU handles、递增 pool generation、保留 CPU cache fingerprint 但取消 published validity。恢复为 lazy recreate；所有 shadow-request lights 在新图完整发布前 suppressed。禁止同步 fence wait 和 same-frame readback。

- [ ] **步骤 5：运行 stats/failure 测试**

```powershell
.\Build\bin\Debug\NullusUnitTests.exe --gtest_filter=RendererStatsTests.*Shadow*:ShadowPlannerTests.*Failure*:ShadowPlannerTests.*Device*
```

预期：`PASS`。

- [ ] **步骤 6：提交生命周期与 telemetry**

```powershell
git add Runtime/Engine/Rendering/BaseSceneRenderer.h Runtime/Engine/Rendering/BaseSceneRenderer.cpp Runtime/Rendering/Data/FrameInfo.h Runtime/Rendering/Core/RendererStats.h Runtime/Rendering/Core/RendererStats.cpp Project/Editor/Panels/FrameInfo.cpp Tests/Unit/RendererStatsTests.cpp Tests/Unit/ShadowPlannerTests.cpp
git commit -m "feat: report and recover cached shadow state"
```

### 任务 11：建立 DX12 漏光与性能证据

**文件：**
- 创建：`Tests/Rendering/DX12ShadowRenderingTests.cpp`
- 修改：`Tests/Performance/RenderScenePerformanceTests.cpp`

- [ ] **步骤 1：写 DX12 图像失败夹具**

分别覆盖 directional、spot、point、closed building、single-sided wall、`Cull Off` 双面墙、AlphaTest fence、receiver acne 和 detached-shadow bias。在线性 readback 上断言：

```cpp
EXPECT_LE(OccludedPatchMax(frame), ambient + 0.02f);
EXPECT_LE(BackfaceBrickGapMax(frame), ambient + 0.05f);
EXPECT_EQ(CountFireflies(frame, 0.98f, 0.75f, silhouetteMask), 0u);
```

- [ ] **步骤 2：先运行并记录预期失败**

```powershell
.\Build\bin\Debug\NullusUnitTests.exe --gtest_filter=DX12ShadowRenderingTests.*
```

预期：在任务 1-10 完成前遮挡阈值失败；无 DX12 adapter 时 `GTEST_SKIP`，不得用截图代替。

- [ ] **步骤 3：增加性能路径**

`RenderScenePerformanceTests` 运行 warmup 后的静态场景、moving local light、moving caster、over-budget scene，记录 p50/p95/p99。证据记录 OS/build、CPU、GPU、driver、backend、fixture 的 light/caster/triangle 数、确定性 camera path，以及改动前后 planning/candidate/draw-record/GPU 各阶段可比较 baseline delta。静态缓存期必须 zero shadow draws；过预算提交不超过 12 views/2048 draws/2,000,000 triangles。

- [ ] **步骤 4：构建 performance target 并运行过滤器**

```powershell
cmake -S . -B Build -DNLS_BUILD_TESTS=ON -DNLS_BUILD_PERFORMANCE_TESTS=ON
cmake --build Build --config Debug --target NullusPerformanceTests -- /m:4
.\Build\bin\Debug\NullusPerformanceTests.exe --gtest_filter=RenderScenePerformanceTests.*Shadow*
```

预期（记录实际 DX12 adapter）：planning CPU p95 <= 0.50 ms；admitted shadow GPU p95 <= 2.50 ms；cached static GPU p95 <= 0.10 ms；over-budget editor-frame p99 increase <= 4.0 ms。

- [ ] **步骤 5：运行 DX12 Debug Layer 与阶段回归**

以 Debug Editor、DX12 Debug Layer 运行上述固定场景；确认无 resource-state、descriptor、NaN depth 或 device-removal 警告。遵守用户要求，不使用 RenderDoc。

- [ ] **步骤 6：提交验证夹具**

```powershell
git add Tests/Rendering/DX12ShadowRenderingTests.cpp Tests/Performance/RenderScenePerformanceTests.cpp
git commit -m "test: verify cached shadows and frame budgets"
```

## 阶段完成标准

- 每灯原有 16-word record 逐 word 不变，shadow metadata/matrices 按 scene light index 对齐。
- 新/缺失/容量 suppressed 的 shadow-request light 不产生无阴影 direct light；last-good 延迟更新仍可用。
- 每帧硬限制为 12 views、2048 draws、2,000,000 triangles，point 初始 6 面和 directional 初始 4 cascade 原子发布。
- Opaque/AlphaTest 与 `Cull Off` caster 正确；Transparent/Decal 不投射。
- Forward opaque/transparent、Deferred、Phong 使用同一 visibility 逻辑；ReceiveShadows=false 精确返回 1。
- 静态 cache hit 为零 shadow draw，资源/组件/材质/LOD/device invalidation 有测试。
- DX12 图像阈值、Debug Layer、性能阈值和 DXIL/SPIR-V 验证都有记录；未验证 backend 不被宣称正确。
