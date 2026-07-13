# PBR 几何法线与着色法线正确性实现计划

> **面向 AI 代理的工作者：** 必需子技能：使用 superpowers:subagent-driven-development（推荐）或 superpowers:executing-plans 逐任务实现此计划。步骤使用复选框（`- [ ]`）语法来跟踪进度。

**目标：** Forward 与 Deferred PBR 同时保留几何法线和着色法线，用几何半球约束直接光照，并在不增加 GBuffer 附件的前提下传递几何法线与接收阴影标志。

**架构：** 新建共享 HLSL 法线工具，负责双面朝向、有限值保护、几何半球约束、oct 编解码和 0.10 宽度的 horizon fade。Forward 直接传递两类法线；Deferred 将几何法线 oct 编码到 Albedo/Normal 的 alpha，将接收阴影位写入 Material alpha，decal 管线只允许写 Albedo RGB。对象接收标志通过现有 16 字节对象 push constant 路径传递，并纳入实例分组兼容性。

**技术栈：** C++20、HLSL/ShaderLab、DXC DXIL/SPIR-V、GoogleTest、Nullus RHI/FrameGraph。

---

## 文件结构

- 创建：`App/Assets/Engine/Shaders/NullusShaderLibrary/PBRNormals.hlsl`，共享法线安全处理、oct 编解码和几何 horizon fade。
- 创建：`Tests/Unit/PBRShadingContractTests.cpp`，CPU 参考数学与源码/ABI 契约测试。
- 修改：`App/Assets/Engine/Shaders/CommonTypes.hlsli`，扩展对象 push constant 为索引和标志。
- 修改：`App/Assets/Engine/Shaders/StandardPBR.hlsl`，Forward 内建 PBR 传递两类法线。
- 修改：`App/Assets/Engine/Shaders/DeferredGBuffer.hlsl`，写入 shading normal、oct geometry normal 和 receiver bit。
- 修改：`App/Assets/Engine/Shaders/DeferredLighting.hlsl`，解码几何法线并调用共享 PBR API。
- 修改：`App/Assets/Engine/Shaders/LightGridCommon.hlsli`，统一 Forward/Deferred 的直接光照入口。
- 修改：`App/Assets/Engine/Shaders/ShaderLab/StandardPBR.shader`，ShaderLab Forward/GBuffer 变体遵循相同契约。
- 修改：`Runtime/Rendering/Data/DrawableObjectDescriptor.h`，保存对象渲染标志。
- 修改：`Runtime/Engine/Rendering/RenderScene.cpp`，从 drawable 状态形成对象标志并阻止不兼容实例合并。
- 修改：`Runtime/Engine/Rendering/EngineFrameObjectBindingProvider.cpp`，上传 16 字节对象 push constant。
- 修改：`Runtime/Rendering/Core/FrameObjectBindingProvider.h`，prepared binding snapshot 携带完整对象常量。
- 修改：`Runtime/Rendering/Core/ABaseRenderer.h/.cpp`，prepared draw 提交完整对象常量。
- 修改：`Runtime/Rendering/Core/CompositeRenderer.cpp`，在 immediate/prepared/recorded draw 之间复制完整对象常量。
- 修改：`Runtime/Rendering/Context/ThreadedRenderingLifecycle.h`、`Runtime/Rendering/Context/Driver.cpp`，线程录制命令保存并推送完整对象常量。
- 修改：`Runtime/Engine/Rendering/BaseSceneRenderer.cpp`，场景 prepared draw 不丢失对象标志。
- 修改：`Runtime/Rendering/Resources/IndexedObjectDataShaderSupport.h`，声明新的对象常量大小。
- 修改：`Runtime/Engine/Rendering/DeferredSceneRenderer.cpp`，对 Deferred decal 设置 Albedo RGB-only attachment write mask。
- 修改：`Tests/Unit/RendererFrameObjectBindingTests.cpp`，验证对象标志布局与实例拆分。
- 修改：`Tests/Unit/ThreadedRenderingLifecycleTests.cpp`，验证 recorded draw 的 16 字节常量与 fragment 可见性。
- 修改：`Tests/Unit/RenderSceneCacheTests.cpp`，验证 cast/receive 标志不兼容时实例拆分。
- 修改：`Tests/Unit/DeferredSceneRendererMaterialCacheTests.cpp`，验证 decal 保留三个 alpha 控制通道。
- 修改：`Tests/Unit/MaterialVariantKeyTests.cpp`，验证 attachment write mask 进入 variant key。
- 修改：`Tests/Rendering/AssetMaterialViewportTests.cpp`，加入几何 horizon 与 Forward/Deferred 图像回归夹具。

### 任务 1：锁定共享法线数学契约

**文件：**
- 创建：`Tests/Unit/PBRShadingContractTests.cpp`
- 创建：`App/Assets/Engine/Shaders/NullusShaderLibrary/PBRNormals.hlsl`

- [ ] **步骤 1：先写 CPU 参考数学失败测试**

测试内实现独立参考 `OctEncode`/`OctDecode`，并添加以下断言；不要读取生产 HLSL 来代替数值验证。

```cpp
TEST(PBRShadingContractTests, OctGeometryNormalRoundTripsBothHemispheres)
{
    for (const auto normal : std::array{
             Normalize(Vector3{1.0f, 2.0f, 3.0f}),
             Normalize(Vector3{-2.0f, 0.5f, -1.0f}),
             Vector3{0.0f, 0.0f, -1.0f}})
    {
        EXPECT_GT(Dot(normal, OctDecode(OctEncode(normal))), 0.999f);
    }
}

TEST(PBRShadingContractTests, GeometryHorizonRejectsWrongHemisphereAndFadesContinuously)
{
    EXPECT_FLOAT_EQ(GeometryFade(-0.01f), 0.0f);
    EXPECT_FLOAT_EQ(GeometryFade(0.0f), 0.0f);
    EXPECT_NEAR(GeometryFade(0.05f), 0.5f, 1.0e-5f);
    EXPECT_FLOAT_EQ(GeometryFade(0.10f), 1.0f);
}
```

- [ ] **步骤 2：运行测试并确认红灯**

运行：

```powershell
cmake --build Build --config Debug --target NullusUnitTests -- /m:4
.\Build\bin\Debug\NullusUnitTests.exe --gtest_filter=PBRShadingContractTests.*
```

预期：编译失败，`PBRNormals.hlsl` 尚不存在或源码契约测试找不到 `NLSConstrainShadingNormalToGeometryHemisphere`。

- [ ] **步骤 3：实现共享 HLSL 最小数学函数**

`PBRNormals.hlsl` 中提供且只提供以下公共入口，所有 normalize 都复用现有 `NLSSafeNormalize`：

```hlsl
float3 NLSOrientGeometryNormal(float3 normalWS, bool isFrontFace);
float3 NLSConstrainShadingNormalToGeometryHemisphere(float3 shadingNormalWS, float3 geometryNormalWS);
float2 NLSOctEncodeNormal(float3 normalWS);
float3 NLSOctDecodeNormal(float2 encoded);
float NLSGeometryHorizonFade(float ndotDirection)
{
    return ndotDirection <= 0.0f ? 0.0f : smoothstep(0.0f, 0.10f, ndotDirection);
}
```

半球约束使用 `shadingNormalWS -= min(0, dot(Ns, Ng)) * Ng` 后安全归一化；NaN、Inf 和近零输入回退到已定向的 `geometryNormalWS`。

- [ ] **步骤 4：增加源码符号与公式契约并确认绿灯**

在同一测试文件读取 `PBRNormals.hlsl`，断言五个公共入口、`smoothstep(0.0f, 0.10f` 和有限值回退存在。重新运行上述过滤器，预期 `PASS`。

- [ ] **步骤 5：提交共享法线数学**

```powershell
git add App/Assets/Engine/Shaders/NullusShaderLibrary/PBRNormals.hlsl Tests/Unit/PBRShadingContractTests.cpp
git commit -m "feat: add shared PBR normal math"
```

### 任务 2：把接收阴影标志加入对象 ABI

**文件：**
- 修改：`Runtime/Rendering/Data/DrawableObjectDescriptor.h`
- 修改：`Runtime/Engine/Rendering/RenderScene.cpp`
- 修改：`Runtime/Engine/Rendering/EngineFrameObjectBindingProvider.cpp`
- 修改：`Runtime/Rendering/Core/FrameObjectBindingProvider.h`
- 修改：`Runtime/Rendering/Core/ABaseRenderer.h/.cpp`
- 修改：`Runtime/Rendering/Core/CompositeRenderer.cpp`
- 修改：`Runtime/Rendering/Context/ThreadedRenderingLifecycle.h`
- 修改：`Runtime/Rendering/Context/Driver.cpp`
- 修改：`Runtime/Engine/Rendering/BaseSceneRenderer.cpp`
- 修改：`Runtime/Engine/Rendering/RenderScene.cpp`
- 修改：`Runtime/Rendering/Resources/IndexedObjectDataShaderSupport.h`
- 修改：`App/Assets/Engine/Shaders/CommonTypes.hlsli`
- 修改：`App/Assets/Engine/Shaders/NullusShaderLibrary/Instancing.hlsl`
- 修改：`Tests/Unit/RendererFrameObjectBindingTests.cpp`
- 修改：`Tests/Unit/ThreadedRenderingLifecycleTests.cpp`
- 修改：`Tests/Unit/RenderSceneCacheTests.cpp`

- [ ] **步骤 1：写失败的 ABI 与实例兼容性测试**

增加测试，要求对象常量恰好四个 `uint32_t`（index、flags、两个显式 padding），并且 cast/receive 任一位不同的 draw 不得合并为一个 instanced draw。再分别覆盖 immediate、prepared 和 threaded recorded draw，确保三条路径推送相同 16 字节且 stage mask 同时包含 vertex/fragment：

```cpp
static_assert(sizeof(ObjectDrawConstants) == 16u);
EXPECT_EQ(constants.objectIndex, 7u);
EXPECT_EQ(constants.objectFlags & kDrawableObjectFlagReceiveShadows,
          kDrawableObjectFlagReceiveShadows);
EXPECT_EQ(groupedDraws.size(), 2u);
EXPECT_EQ(capturedPush.size, sizeof(ObjectDrawConstants));
EXPECT_TRUE(HasShaderStage(capturedPush.stageMask, ShaderStageMask::Vertex));
EXPECT_TRUE(HasShaderStage(capturedPush.stageMask, ShaderStageMask::Fragment));
EXPECT_EQ(recordedDraw.objectConstants.objectFlags, constants.objectFlags);
```

- [ ] **步骤 2：运行过滤测试确认失败**

```powershell
.\Build\bin\Debug\NullusUnitTests.exe --gtest_filter=RendererFrameObjectBindingTests.*ReceiveShadow*:RenderSceneCacheTests.*ReceiveShadow*
```

预期：编译失败，`ObjectDrawConstants` 和 `kDrawableObjectFlagReceiveShadows` 尚未定义。

- [ ] **步骤 3：添加对象标志和 push constant 结构**

在 `DrawableObjectDescriptor.h` 定义稳定 ABI：

```cpp
inline constexpr uint32_t kDrawableObjectFlagReceiveShadows = 1u << 0u;
inline constexpr uint32_t kDrawableObjectFlagCastShadows = 1u << 1u;
struct ObjectDrawConstants
{
    uint32_t objectIndex = DrawableObjectDescriptor::kInvalidObjectIndex;
    uint32_t objectFlags = kDrawableObjectFlagReceiveShadows | kDrawableObjectFlagCastShadows;
    uint32_t padding0 = 0u;
    uint32_t padding1 = 0u;
};
static_assert(sizeof(ObjectDrawConstants) == 16u);
```

`DrawableObjectDescriptor` 新增默认 cast/receive 两位；`RenderScene` 的 draw/instance key 包含 `objectFlags`。本计划阶段还没有组件字段时，所有普通 drawable 默认同时投射并接收阴影。

- [ ] **步骤 4：上传 16 字节常量并同步 HLSL**

两个 HLSL 声明统一为：

```hlsl
cbuffer ObjectIndexConstants : register(b1, space3)
{
    uint u_ObjectIndex;
    uint u_ObjectFlags;
    uint u_ObjectPadding0;
    uint u_ObjectPadding1;
};
static const uint NLS_OBJECT_FLAG_RECEIVE_SHADOWS = 1u;
```

`EngineFrameObjectBindingProvider` 用 `ObjectDrawConstants` 调用 `PushConstants`，并让该 range 对 vertex/fragment stages 可见。将 provider 当前的 `m_currentDrawObjectIndex` 提升为完整常量；`PreparedBindingSets`、`ABaseRenderer::PreparedRecordedDraw` 和 `RecordedDrawCommandInput` 都保存 `ObjectDrawConstants`，`CompositeRenderer`、`BaseSceneRenderer`、`ABaseRenderer::SubmitPreparedDraw` 与 `Driver` 逐层复制并推送同一 16 字节值。禁止在线程命令中重新从 mutable drawable 查询 flags。更新所有布局断言和 `BackendSupportsIndexedObjectDataPushConstants` 相关大小判断。

- [ ] **步骤 5：构建并运行对象绑定测试**

```powershell
cmake --build Build --config Debug --target NullusUnitTests -- /m:4
.\Build\bin\Debug\NullusUnitTests.exe --gtest_filter=RendererFrameObjectBindingTests.*:ThreadedRenderingLifecycleTests.*ObjectConstant*:RenderSceneCacheTests.*ReceiveShadow*
```

预期：`PASS`，旧的 object-index 测试仍通过，新增测试观察到 16 字节 push constant。

- [ ] **步骤 6：提交对象 ABI**

```powershell
git add Runtime/Rendering/Data/DrawableObjectDescriptor.h Runtime/Engine/Rendering/RenderScene.cpp Runtime/Engine/Rendering/EngineFrameObjectBindingProvider.cpp Runtime/Rendering/Core/FrameObjectBindingProvider.h Runtime/Rendering/Core/ABaseRenderer.h Runtime/Rendering/Core/ABaseRenderer.cpp Runtime/Rendering/Core/CompositeRenderer.cpp Runtime/Rendering/Context/ThreadedRenderingLifecycle.h Runtime/Rendering/Context/Driver.cpp Runtime/Engine/Rendering/BaseSceneRenderer.cpp Runtime/Rendering/Resources/IndexedObjectDataShaderSupport.h App/Assets/Engine/Shaders/CommonTypes.hlsli App/Assets/Engine/Shaders/NullusShaderLibrary/Instancing.hlsl Tests/Unit/RendererFrameObjectBindingTests.cpp Tests/Unit/ThreadedRenderingLifecycleTests.cpp Tests/Unit/RenderSceneCacheTests.cpp
git commit -m "feat: carry shadow receiver object flags"
```

### 任务 3：统一 Forward PBR 的两法线直接光照

**文件：**
- 修改：`App/Assets/Engine/Shaders/LightGridCommon.hlsli`
- 修改：`App/Assets/Engine/Shaders/StandardPBR.hlsl`
- 修改：`App/Assets/Engine/Shaders/ShaderLab/StandardPBR.shader`
- 修改：`Tests/Unit/PBRShadingContractTests.cpp`

- [ ] **步骤 1：写失败的共享调用契约**

断言 `NLSEvaluateCookTorranceDirect` 的参数顺序为 geometry、shading、view、light，并要求两个 Forward shader 都调用同一 API：

```cpp
EXPECT_NE(common.find("float3 geometryNormalWS,\n    float3 shadingNormalWS"), std::string::npos);
EXPECT_NE(standard.find("geometryNormalWS,\n        shadingNormalWS"), std::string::npos);
EXPECT_NE(shaderLab.find("geometryNormalWS,\n                    shadingNormalWS"), std::string::npos);
```

- [ ] **步骤 2：运行测试确认失败**

运行过滤器 `PBRShadingContractTests.*Forward*`，预期失败：当前 API 只有一个 `normalWS`。

- [ ] **步骤 3：重构直接 BRDF API**

保持 GGX 的 D/G/F 和 roughness variance filter 使用 `shadingNormalWS`；先计算未 saturate 的几何点积：

```hlsl
const float geometryNdotL = dot(geometryNormalWS, lightDir);
const float geometryNdotV = dot(geometryNormalWS, viewDir);
if (geometryNdotL <= 0.0f || geometryNdotV <= 0.0f)
    return 0.0f.xxx;
const float geometryFade = NLSGeometryHorizonFade(geometryNdotL) *
                           NLSGeometryHorizonFade(geometryNdotV);
return brdf * radiance * shadingNdotL * geometryFade * visibility;
```

ambient/IBL 保留在 direct loop 外，仅乘 AO，不乘 geometry fade 或 shadow visibility。

- [ ] **步骤 4：Forward shader 构造两类法线**

内建和 ShaderLab PBR 都先用 `SV_IsFrontFace` 得到 `geometryNormalWS`，再从已定向 tangent frame 得到 `shadingNormalWS`，最后调用 `NLSConstrainShadingNormalToGeometryHemisphere`。无 normal map 时两者相等。

- [ ] **步骤 5：编译并运行契约测试**

```powershell
cmake --build Build --config Debug --target NullusUnitTests -- /m:4
.\Build\bin\Debug\NullusUnitTests.exe --gtest_filter=PBRShadingContractTests.*
```

预期：`PASS`；DXC 可用时 `StandardPBR.hlsl` 和 ShaderLab `_NORMALMAP`/`Cull Off` 变体编译成功。

- [ ] **步骤 6：提交 Forward 正确性**

```powershell
git add App/Assets/Engine/Shaders/LightGridCommon.hlsli App/Assets/Engine/Shaders/StandardPBR.hlsl App/Assets/Engine/Shaders/ShaderLab/StandardPBR.shader Tests/Unit/PBRShadingContractTests.cpp
git commit -m "fix: gate PBR direct light by geometry normals"
```

### 任务 4：实现 Deferred GBuffer 双法线契约

**文件：**
- 修改：`App/Assets/Engine/Shaders/DeferredGBuffer.hlsl`
- 修改：`App/Assets/Engine/Shaders/DeferredLighting.hlsl`
- 修改：`App/Assets/Engine/Shaders/ShaderLab/StandardPBR.shader`
- 修改：`Tests/Unit/PBRShadingContractTests.cpp`

- [ ] **步骤 1：写失败的 GBuffer 通道测试**

要求精确布局：

```cpp
EXPECT_NE(gbuffer.find("output.Albedo = float4(albedo, geometryNormalOct.x)"), std::string::npos);
EXPECT_NE(gbuffer.find("output.Normal = float4(shadingNormalWS * 0.5f + 0.5f, geometryNormalOct.y)"), std::string::npos);
EXPECT_NE(gbuffer.find("output.Material = float4(metallic, roughness, ao, receiveShadows)"), std::string::npos);
EXPECT_NE(lighting.find("NLSOctDecodeNormal(float2(albedo.a, encodedShadingNormal.a))"), std::string::npos);
```

- [ ] **步骤 2：运行测试确认红灯**

运行 `PBRShadingContractTests.*GBuffer*`，预期失败：三个 alpha 当前都是 `surfaceAlpha`。

- [ ] **步骤 3：写入新布局**

Opaque/AlphaTest 在 clip 后写：

```hlsl
const float2 geometryNormalOct = NLSOctEncodeNormal(geometryNormalWS);
const float receiveShadows = (u_ObjectFlags & NLS_OBJECT_FLAG_RECEIVE_SHADOWS) != 0u ? 1.0f : 0.0f;
output.Albedo = float4(albedo, geometryNormalOct.x);
output.Normal = float4(shadingNormalWS * 0.5f + 0.5f, geometryNormalOct.y);
output.Material = float4(metallic, roughness, ao, receiveShadows);
```

- [ ] **步骤 4：Deferred 解码并调用共享直接光 API**

采样三张纹理各一次；从 `albedo.a/normal.a` 解码 geometry normal，从 `normal.rgb` 解码 shading normal，`material.a >= 0.5f` 得到 receiver flag。调用参数顺序与 Forward 完全一致。

- [ ] **步骤 5：运行契约与 shader 编译测试**

```powershell
.\Build\bin\Debug\NullusUnitTests.exe --gtest_filter=PBRShadingContractTests.*:ShaderCompilerTests.*Deferred*
```

预期：`PASS`；DXIL 与 SPIR-V 两目标均成功，SPIR-V 输出通过测试内 `spirv-val` 检查或在工具缺失时明确 `GTEST_SKIP`。

- [ ] **步骤 6：提交 GBuffer 契约**

```powershell
git add App/Assets/Engine/Shaders/DeferredGBuffer.hlsl App/Assets/Engine/Shaders/DeferredLighting.hlsl App/Assets/Engine/Shaders/ShaderLab/StandardPBR.shader Tests/Unit/PBRShadingContractTests.cpp
git commit -m "fix: preserve geometry normals in deferred GBuffer"
```

### 任务 5：确保 Deferred decal 保留控制 alpha

**文件：**
- 修改：`Runtime/Engine/Rendering/DeferredSceneRenderer.cpp`
- 修改：`Tests/Unit/DeferredSceneRendererMaterialCacheTests.cpp`
- 修改：`Tests/Unit/DX12GraphicsPipelineUtilsTests.cpp`
- 修改：`Tests/Unit/MaterialVariantKeyTests.cpp`

- [ ] **步骤 1：写失败的 decal write-mask 测试**

构造 Deferred decal material variant，断言 Albedo 只写 RGB，Normal/Material 完全禁写：

```cpp
const auto rgb = RHIColorWriteMask::Red | RHIColorWriteMask::Green | RHIColorWriteMask::Blue;
EXPECT_EQ(pipeline.blendState.renderTargets[0].colorWriteMask, rgb);
EXPECT_EQ(pipeline.blendState.renderTargets[1].colorWriteMask, RHIColorWriteMask::None);
EXPECT_EQ(pipeline.blendState.renderTargets[2].colorWriteMask, RHIColorWriteMask::None);
```

- [ ] **步骤 2：运行测试确认失败**

运行 `DeferredSceneRendererMaterialCacheTests.*Decal*Alpha*`，预期失败：默认 write mask 是 `All`。

- [ ] **步骤 3：在 Deferred decal variant 设置独立附件覆盖**

用现有 `MaterialPipelineStateOverrides::SetRenderTargetBlendStates` 为 Albedo 设置 `Red | Green | Blue`，为 Normal/Material 设置 `None`。只修改 Deferred decal variant，不改变 Forward decal 或普通 GBuffer material。

- [ ] **步骤 4：验证 DX12 映射和 material cache key**

扩展 DX12 测试，断言 `D3D12_RENDER_TARGET_BLEND_DESC::RenderTargetWriteMask` 不含 `D3D12_COLOR_WRITE_ENABLE_ALPHA`；同时验证不同 mask 产生不同 material variant key。

- [ ] **步骤 5：构建并运行测试**

```powershell
cmake --build Build --config Debug --target NullusUnitTests -- /m:4
.\Build\bin\Debug\NullusUnitTests.exe --gtest_filter=DeferredSceneRendererMaterialCacheTests.*Decal*:DX12GraphicsPipelineUtilsTests.*WriteMask*
```

预期：`PASS`。

- [ ] **步骤 6：提交 decal 通道保护**

```powershell
git add Runtime/Engine/Rendering/DeferredSceneRenderer.cpp Tests/Unit/DeferredSceneRendererMaterialCacheTests.cpp Tests/Unit/DX12GraphicsPipelineUtilsTests.cpp Tests/Unit/MaterialVariantKeyTests.cpp
git commit -m "fix: preserve deferred control channels through decals"
```

### 任务 6：增加 GPU 图像证据并完成阶段验证

**文件：**
- 修改：`Tests/Rendering/AssetMaterialViewportTests.cpp`
- 修改：`Tests/Unit/PBRShadingContractTests.cpp`

- [ ] **步骤 1：写失败的 horizon GPU 夹具**

渲染单面墙和强倾斜 normal map；分别测试正面、背面、Forward、Deferred。在线性色彩缓冲上断言：背向灯的 patch 不超过 ambient + 0.05；球面非轮廓区域的相邻亮度 99 分位不超过 0.12。

```cpp
EXPECT_LE(BackFacingPatchMax(frame), ambientLuminance + 0.05f);
EXPECT_LE(AdjacentLuminanceDeltaPercentile(frame, 0.99f, interiorMask), 0.12f);
```

- [ ] **步骤 2：写 Forward/Deferred 一致性失败测试**

同一场景两个 renderer 输出在线性空间比较：

```cpp
EXPECT_LE(ComputeRmse(forward, deferred, interiorMask), 0.02f);
EXPECT_LE(ComputeMaxDifference(forward, deferred, interiorMask), 0.08f);
```

- [ ] **步骤 3：运行 GPU 过滤测试确认先失败后通过**

```powershell
.\Build\bin\Debug\NullusUnitTests.exe --gtest_filter=AssetMaterialViewportTests.*GeometryNormal*:AssetMaterialViewportTests.*ForwardDeferredPBR*
```

预期：实现前至少一个阈值失败；完成任务 1-5 后全部 `PASS`。记录运行后端必须为 DX12；若无 DX12 adapter，测试应 `GTEST_SKIP` 且不能当作通过证据。

- [ ] **步骤 4：运行阶段回归**

```powershell
.\Build\bin\Debug\NullusUnitTests.exe --gtest_filter=PBRShadingContractTests.*:RendererFrameObjectBindingTests.*:DeferredSceneRendererMaterialCacheTests.*:AssetMaterialViewportTests.*PBR*
```

预期：`PASS`，无 NaN/Inf、无 isolated white-pixel 回归。

- [ ] **步骤 5：提交 GPU 证据**

```powershell
git add Tests/Rendering/AssetMaterialViewportTests.cpp Tests/Unit/PBRShadingContractTests.cpp
git commit -m "test: cover PBR geometry-normal horizons"
```

## 阶段完成标准

- Forward 与 Deferred 的直接光照都通过同一个 `NLSEvaluateCookTorranceDirect`。
- 几何 `NdotL/NdotV <= 0` 时直接光严格为零，`0..0.10` 连续淡入。
- normal map 只控制 shading normal，不能把错误半球重新照亮。
- GBuffer 三个 alpha 通道分别是 oct X、oct Y、receiver bit，decal 只写 Albedo RGB。
- 对象常量 ABI、实例拆分、DXIL/SPIR-V 编译和 DX12 图像阈值均有测试证据。
