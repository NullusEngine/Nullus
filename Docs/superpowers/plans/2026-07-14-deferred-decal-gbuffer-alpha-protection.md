# Deferred Decal GBuffer Alpha Protection Implementation Plan

> **For AI agent workers:** Required sub-skill: use
> `superpowers:subagent-driven-development` to execute this plan one task at a
> time. Track each step with the checkboxes below.

**Goal:** Preserve all deferred GBuffer alpha contracts while keeping decal RGB
blending driven by authored material opacity.

**Architecture:** Add an RGB-only independent blend state for the decal pass,
add a dedicated built-in and ShaderLab `DeferredDecal` shader pass that outputs
only color plus source opacity, and route both immediate and threaded deferred
decal draws through that pass. Keep fallback shader/material caches distinct
from GBuffer caches and reuse the existing material synchronization contract.

**Technical stack:** C++20, HLSL/ShaderLab, Nullus RHI pipeline overrides,
DXC DXIL/SPIR-V compilation, GoogleTest, DX12 blend-state translation.

---

## File Structure

- Create `App/Assets/Engine/Shaders/DeferredDecal.hlsl`: built-in deferred
  decal color-only shader.
- Modify `App/Assets/Engine/Shaders/StandardPBR.hlsl`: consume the shared
  base-color/opacity result in the built-in forward path.
- Modify `App/Assets/Engine/Shaders/NullusShaderLibrary/StandardPBRSurface.hlsl`:
  shared base-color and opacity evaluation.
- Modify `App/Assets/Engine/Shaders/ShaderLab/StandardPBR.shader`: real
  `LightMode=DeferredDecal` pass.
- Modify `Runtime/Rendering/Resources/Loaders/ShaderLoader.cpp`: infer the
  built-in decal light mode.
- Modify `Runtime/Engine/Rendering/DeferredSceneRenderer.h/.cpp`: RGB-only MRT
  state, dedicated shader/cache ownership, pass resolution, immediate and
  threaded routing.
- Modify `Tests/Unit/DX12GraphicsPipelineUtilsTests.cpp`: native RGB write-mask
  translation.
- Modify `Tests/Unit/RendererFrameObjectBindingTests.cpp`: production override,
  fallback/cache, and final recorded-pipeline contracts.
- Modify `Tests/Unit/PBRShadingContractTests.cpp`: shader output, pass parsing,
  dependency, and DXIL/SPIR-V compilation contracts.
- Modify `Tests/Unit/ShaderLabPassAndPipelineTests.cpp`: material light-mode
  resolution for the new pass.

### Task 1: Protect GBuffer Alpha With Per-Target Write Masks

**Files:**

- Modify: `Runtime/Engine/Rendering/DeferredSceneRenderer.cpp:362-384`
- Modify: `Tests/Unit/RendererFrameObjectBindingTests.cpp:5699-5874`
- Modify: `Tests/Unit/DX12GraphicsPipelineUtilsTests.cpp:91-122`

- [ ] **Step 1: Write failing production-override tests**

Update `DeferredDecalOverridesBlendAlbedoOnlyAndDisableStencilWrites` so MRT0
must contain RGB and must not contain Alpha:

```cpp
const auto rgbMask =
    NLS::Render::RHI::RHIColorWriteMask::Red |
    NLS::Render::RHI::RHIColorWriteMask::Green |
    NLS::Render::RHI::RHIColorWriteMask::Blue;

EXPECT_EQ(targets[0].colorWriteMask, rgbMask);
EXPECT_FALSE(NLS::Render::RHI::HasColorWriteMask(
    targets[0].colorWriteMask,
    NLS::Render::RHI::RHIColorWriteMask::Alpha));
EXPECT_EQ(targets[1].colorWriteMask, NLS::Render::RHI::RHIColorWriteMask::None);
EXPECT_EQ(targets[2].colorWriteMask, NLS::Render::RHI::RHIColorWriteMask::None);
```

Refactor `RecordedPipelineOverridesCanUseIndependentDecalGBufferBlendState` to
obtain overrides from
`DeferredSceneRendererTestAccess::BuildDeferredDecalMaterialOverridesForTesting`
instead of rebuilding a test-only state, then assert:

```cpp
EXPECT_TRUE(desc.blendState.independentBlendEnable);
EXPECT_EQ(desc.blendState.renderTargets[0].colorWriteMask, rgbMask);
EXPECT_TRUE(desc.blendState.renderTargets[0].blendEnable);
EXPECT_EQ(desc.blendState.renderTargets[0].srcColor, RHIBlendFactor::SrcAlpha);
EXPECT_EQ(desc.blendState.renderTargets[0].dstColor, RHIBlendFactor::InvSrcAlpha);
```

- [ ] **Step 2: Verify the tests fail for the old `All` mask**

Run:

```powershell
cmake --build Build --config Debug --target NullusUnitTests -- /m:4
Build\bin\Debug\NullusUnitTests.exe --gtest_filter=RendererFrameObjectBindingTests.DeferredDecalOverridesBlendAlbedoOnlyAndDisableStencilWrites:RendererFrameObjectBindingTests.RecordedPipelineOverridesCanUseIndependentDecalGBufferBlendState
```

Expected: both relevant mask assertions fail because production currently uses
`RHIColorWriteMask::All`.

- [ ] **Step 3: Add a failing DX12 RGB-mask translation assertion**

Extend `BuildsExpandedBlendStateForIndependentRenderTargets` with an RGB-only
target and assert:

```cpp
EXPECT_EQ(
    blendState.RenderTarget[0].RenderTargetWriteMask,
    D3D12_COLOR_WRITE_ENABLE_RED |
        D3D12_COLOR_WRITE_ENABLE_GREEN |
        D3D12_COLOR_WRITE_ENABLE_BLUE);
EXPECT_EQ(
    blendState.RenderTarget[0].RenderTargetWriteMask & D3D12_COLOR_WRITE_ENABLE_ALPHA,
    0u);
```

Run that single test and confirm it fails while the fixture still uses a mask
containing alpha.

- [ ] **Step 4: Implement the RGB-only production mask**

In `BuildDeferredDecalMaterialOverrides`, replace `All` with the explicit
bitwise mask:

```cpp
constexpr auto kDeferredDecalColorWriteMask =
    NLS::Render::RHI::RHIColorWriteMask::Red |
    NLS::Render::RHI::RHIColorWriteMask::Green |
    NLS::Render::RHI::RHIColorWriteMask::Blue;

blendedTarget.blendEnable = true;
blendedTarget.colorWriteMask = kDeferredDecalColorWriteMask;
```

Do not change the blend factors, MRT1/MRT2 suppression, depth state, stencil
state, or frame-graph attachments.

- [ ] **Step 5: Verify Task 1 is green and mutation-sensitive**

Run:

```powershell
Build\bin\Debug\NullusUnitTests.exe --gtest_filter=RendererFrameObjectBindingTests.DeferredDecalOverridesBlendAlbedoOnlyAndDisableStencilWrites:RendererFrameObjectBindingTests.RecordedPipelineOverridesCanUseIndependentDecalGBufferBlendState:DX12GraphicsPipelineUtilsTests.BuildsExpandedBlendStateForIndependentRenderTargets
```

Expected: 3/3 pass. Temporarily restore `All`; the two renderer tests must fail.
Restore RGB and rerun green.

- [ ] **Step 6: Commit Task 1**

```powershell
git add Runtime/Engine/Rendering/DeferredSceneRenderer.cpp Tests/Unit/RendererFrameObjectBindingTests.cpp Tests/Unit/DX12GraphicsPipelineUtilsTests.cpp
git commit -m "fix: protect deferred gbuffer alpha from decals"
```

### Task 2: Add Dedicated Built-In And ShaderLab Decal Passes

**Files:**

- Create: `App/Assets/Engine/Shaders/DeferredDecal.hlsl`
- Modify: `App/Assets/Engine/Shaders/StandardPBR.hlsl`
- Modify: `App/Assets/Engine/Shaders/NullusShaderLibrary/StandardPBRSurface.hlsl`
- Modify: `App/Assets/Engine/Shaders/ShaderLab/StandardPBR.shader:25-332`
- Modify: `Runtime/Rendering/Resources/Loaders/ShaderLoader.cpp:79-89`
- Modify: `Tests/Unit/PBRShadingContractTests.cpp`
- Modify: `Tests/Unit/ShaderLabPassAndPipelineTests.cpp:290-350`

- [ ] **Step 1: Write failing shared-surface and pass contract tests**

Add a contract for a parameterized shared helper:

```hlsl
float4 NLSEvaluateStandardPbrBaseColorAndOpacity(
    float4 baseSample,
    float4 baseColor,
    float opacity)
{
    return float4(
        baseSample.rgb * baseColor.rgb,
        saturate(baseColor.a * baseSample.a * opacity));
}
```

The helper is the SSoT for every Standard PBR path that evaluates both base
color and opacity. The contract test must require these consumers:

- built-in `StandardPBR.hlsl`;
- built-in `DeferredDecal.hlsl`;
- ShaderLab `Forward`;
- ShaderLab `GBuffer` alpha-test/base-color evaluation;
- ShaderLab `DeferredDecal`.

`DepthOnly` remains outside this helper because its established contract does
not bind or sample the opacity map. The C++ test must parse
`StandardPBR.shader`, find exactly one pass with `LightMode=DeferredDecal`, and
verify its pixel entry emits only `SV_Target0` through this helper. Read
`DeferredDecal.hlsl` and require the same helper.
Require neither decal path to contain `u_GBuffer`, `NLSOctEncodeNormal`, or
`NLSPackStandardPbrGBuffer`.

- [ ] **Step 2: Verify the shader contract test fails**

Run:

```powershell
cmake --build Build --config Debug --target NullusUnitTests -- /m:4
Build\bin\Debug\NullusUnitTests.exe --gtest_filter=PBRShadingContractTests.DeferredDecal*
```

Expected: fail because the helper, built-in shader, and ShaderLab pass do not
exist.

- [ ] **Step 3: Add failing built-in light-mode and ShaderLab resolution tests**

Extend loader/pass tests to require:

```cpp
auto* builtIn = ShaderLoader::CreateBuiltInHlsl(
    "App/Assets/Engine/Shaders/DeferredDecal.hlsl");
ASSERT_NE(builtIn, nullptr);
Material builtInMaterial(builtIn);
EXPECT_EQ(
    builtInMaterial.ResolveShaderForLightMode("DeferredDecal"),
    builtIn);
```

For ShaderLab, parse a material containing `Forward`, `GBuffer`, and
`DeferredDecal`, then assert:

```cpp
EXPECT_NE(material.ResolveShaderForLightMode("DeferredDecal"), nullptr);
EXPECT_NE(
    material.ResolveShaderForLightMode("DeferredDecal"),
    material.ResolveShaderForLightMode("GBuffer"));
```

Run the focused tests and confirm failure is due to the missing new light mode.

- [ ] **Step 4: Implement the shared evaluation helper and built-in shader**

Add `NLSEvaluateStandardPbrBaseColorAndOpacity` to
`StandardPBRSurface.hlsl`. Reuse it from built-in `StandardPBR.hlsl` and the
ShaderLab `Forward` and `GBuffer` passes wherever base color and opacity are
evaluated, so the formula has one source of truth. Do not add an opacity
texture read to built-in `DeferredGBuffer.hlsl`, which currently has no alpha
test and does not consume surface alpha.

Create `DeferredDecal.hlsl` with the existing built-in object/frame/material
bindings, an instanced vertex transform, and a pixel shader equivalent to:

```hlsl
float4 PSMain(VSOutput input) : SV_Target0
{
    const float4 baseSample =
        u_AlbedoMap.Sample(u_LinearWrapSampler, input.TexCoord);
    const float opacity =
        u_OpacityMap.Sample(u_LinearWrapSampler, input.TexCoord).r;
    return NLSEvaluateStandardPbrBaseColorAndOpacity(baseSample, u_Albedo, opacity);
}
```

Use the actual established built-in parameter names from `CommonTypes.hlsli`.
Do not add GBuffer texture bindings or normal/material outputs.

- [ ] **Step 5: Implement the ShaderLab pass and loader inference**

Add a pass with:

```shaderlab
Name "DeferredDecal"
Tags { "LightMode" = "DeferredDecal" }
Cull Back
ZWrite Off
ZTest LessEqual
Blend SrcAlpha OneMinusSrcAlpha
```

Its pixel shader samples `_BaseMap` and `_OpacityMap`, calls the shared helper,
supports `_ALPHATEST_ON`, and returns one `SV_Target0`. Keep material bindings
consistent with existing ShaderLab properties.

In `InferBuiltInHlslLightMode`, map `DeferredDecal.hlsl` to
`"DeferredDecal"` using the same normalized-path convention as GBuffer.

- [ ] **Step 6: Add DXIL/SPIR-V compile variants without skips**

Extend the native DXC test to compile vertex and pixel stages for
`DeferredDecal.hlsl`, and ShaderLab pixel variants:

```cpp
Default
AlphaTest
```

Run both `ShaderTargetPlatform::DXIL` and `SPIRV`. Assert non-empty bytecode,
the expected artifact extension, and dependencies on both
`StandardPBRSurface.hlsl` and the decal source.

- [ ] **Step 7: Verify Task 2 suites**

Run:

```powershell
Build\bin\Debug\NullusUnitTests.exe --gtest_filter=PBRShadingContractTests.*:ShaderLabPassAndPipelineTests.*:ShaderLabMaterialTests.*
```

Expected: all pass, no shader compile skip. Temporarily change the decal helper
alpha to packed normal or omit opacity; the decal behavior/contract test must
fail. Restore and rerun green.

- [ ] **Step 8: Commit Task 2**

```powershell
git add App/Assets/Engine/Shaders/DeferredDecal.hlsl App/Assets/Engine/Shaders/StandardPBR.hlsl App/Assets/Engine/Shaders/NullusShaderLibrary/StandardPBRSurface.hlsl App/Assets/Engine/Shaders/ShaderLab/StandardPBR.shader Runtime/Rendering/Resources/Loaders/ShaderLoader.cpp Tests/Unit/PBRShadingContractTests.cpp Tests/Unit/ShaderLabPassAndPipelineTests.cpp
git commit -m "feat: add deferred decal color pass"
```

### Task 3: Route Immediate And Threaded Decals Through The New Pass

**Files:**

- Modify: `Runtime/Engine/Rendering/DeferredSceneRenderer.h:148-210`
- Modify: `Runtime/Engine/Rendering/DeferredSceneRenderer.cpp:1060-1130`
- Modify: `Runtime/Engine/Rendering/DeferredSceneRenderer.cpp:580-600`
- Modify: `Runtime/Engine/Rendering/DeferredSceneRenderer.cpp:1539-1553`
- Modify: `Runtime/Engine/Rendering/DeferredSceneRenderer.cpp:1625-1645`
- Modify: `Runtime/Engine/Rendering/DeferredSceneRenderer.cpp:2492-2660`
- Modify: `Runtime/Engine/Rendering/DeferredSceneRenderer.cpp:2728-2825`
- Modify: `Tests/Unit/RendererFrameObjectBindingTests.cpp`
- Modify: `Tests/Unit/FrameGraphSceneTargetsTests.cpp:2482-2610`

- [ ] **Step 1: Write failing resolution and cache-isolation tests**

Add test access only for production functions needed by these assertions. Cover:

```cpp
EXPECT_EQ(
    &DeferredSceneRendererTestAccess::ResolveDeferredDecalDrawableMaterialForTesting(
        renderer,
        shaderLabMaterialWithDeferredDecal),
    &shaderLabMaterialWithDeferredDecal);

auto& fallback =
    DeferredSceneRendererTestAccess::ResolveDeferredDecalDrawableMaterialForTesting(
        renderer,
        materialWithoutDeferredDecal);
EXPECT_EQ(fallback.GetShader(), deferredDecalShader);
EXPECT_NE(&fallback, &resolvedGBufferFallback);
```

Resolve twice with an unchanged source and require a cache hit/reused fallback;
change a source texture/property revision and require synchronization without
creating an unbounded new entry.

Add lifecycle and failure-path tests that require:

- deferred pipeline readiness is false when all other built-ins are ready but
  `m_deferredDecalShader` is absent;
- missing decal shader causes both immediate and threaded decal preparation to
  fail closed instead of resolving `GBuffer` or publishing a partial draw;
- the frame-resolution cache and its hit/miss counters reset at the frame
  boundary while the persistent fallback entry remains reusable;
- decal fallback materials/caches are released before the owned decal shader
  is destroyed. Make the ordering mutation-sensitive through existing test
  access or a narrow lifetime probe, not through a second production path.

- [ ] **Step 2: Verify resolution tests fail against GBuffer routing**

Run the new `RendererFrameObjectBindingTests.*DeferredDecal*Resolution*` filter.
Expected: fail because only `ResolveGBufferDrawableMaterial` and the GBuffer
fallback exist.

- [ ] **Step 3: Implement dedicated shader ownership and cache identity**

Load and own `m_deferredDecalShader` beside `m_gBufferShader`:

```cpp
m_deferredDecalShader = ShaderLoader::CreateBuiltInHlsl(
    ResolveEngineShaderPath("DeferredDecal.hlsl"),
    projectAssetsRoot);
```

Destroy `m_deferredDecalShader` through `ShaderLoader::Destroy` beside the
other built-in shaders, clear dependent fallback materials before destruction,
and include it in the renderer pipeline-resource readiness predicate. Decal
drawing must fail closed when the dedicated fallback shader is unavailable.

Add dedicated decal fallback and frame-resolution cache state. Reuse the
existing material synchronization fields and stamp rules, but do not share a
cache entry or key namespace with GBuffer. The resolver contract is:

```cpp
Material& ResolveDeferredDecalDrawableMaterial(Material& source)
{
    if (source.ResolveShaderForLightMode("DeferredDecal") != nullptr)
        return source;
    return ResolveFrameDeferredDecalMaterial(source);
}
```

Fallback materials use `m_deferredDecalShader`, retain source textures and
properties through the established synchronization path, and are blendable.
The expected hot-path complexity is average O(1) lookup per resolved source
material, with no shader compilation or fallback allocation after cache
warm-up. The frame cache is bounded by decal source materials observed in the
current frame and is cleared at the existing frame boundary.

- [ ] **Step 4: Route both draw paths to `DeferredDecal`**

In threaded capture, replace the decal-only GBuffer resolution and light-mode
argument:

```cpp
decalDrawable.material =
    &ResolveDeferredDecalDrawableMaterial(*drawable.material);
CaptureThreadedPreparedDraw(
    decalDrawable,
    decalOverrides,
    GetDeferredDecalDepthCompare(),
    "DeferredDecal",
    preparedDraw);
```

In immediate `DrawDecals`, resolve the same material and call:

```cpp
DrawEntity(
    decalDrawable,
    decalOverrides,
    GetDeferredDecalDepthCompare(),
    "DeferredDecal");
```

Do not change opaque `GBuffer` or transparent `Forward` routing.

- [ ] **Step 5: Add integration assertions for both paths and pass state**

Extend renderer/frame-graph tests so a captured threaded decal draw and an
immediate decal callback both resolve a pipeline whose shader/pass identity is
`DeferredDecal`. Assert the final recorded pipeline still has:

```cpp
MRT0: blend enabled, RGB mask
MRT1: blend disabled, None mask
MRT2: blend disabled, None mask
depth write: false
stencil write mask: 0
```

Keep the existing frame-graph assertions that the decal pass is between GBuffer
and lighting, loads/stores all three color attachments without clear, and uses
read-only depth.

- [ ] **Step 6: Verify Task 3 focused tests and mutations**

Run:

```powershell
Build\bin\Debug\NullusUnitTests.exe --gtest_filter=RendererFrameObjectBindingTests.*DeferredDecal*:FrameGraphSceneTargetsTests.DeferredPreparedExecutionSlicesDecalBeforeLightingAndTransparentAfterLighting:ShaderLabPassAndPipelineTests.*
```

Expected: all pass. Mutate either immediate or threaded light mode back to
`GBuffer`; the corresponding integration assertion must fail. Restore and
rerun green.

- [ ] **Step 7: Run the full Task 5 regression set**

```powershell
cmake --build Build --config Debug --target NullusUnitTests -- /m:4
Build\bin\Debug\NullusUnitTests.exe --gtest_filter=PBRShadingContractTests.*
Build\bin\Debug\NullusUnitTests.exe --gtest_filter=AssetMaterialConversionTests.Pbr*:AssetMaterialConversionTests.BuiltInPbr*:AssetMaterialConversionTests.SafeTangentFrameFallbacksKeepMappedNormalsFinite
Build\bin\Debug\NullusUnitTests.exe --gtest_filter=RendererFrameObjectBindingTests.*DeferredDecal*:FrameGraphSceneTargetsTests.Deferred*:DX12GraphicsPipelineUtilsTests.*
Build\bin\Debug\NullusUnitTests.exe --gtest_filter=ShaderLabHotReloadTests.*:ShaderLabPassAndPipelineTests.*:ShaderLabMaterialTests.*
git diff --check 5f0965b3..HEAD
git status --short
```

Expected: every selected test passes, DXIL/SPIR-V compile tests report no skip,
`diff --check` is empty, and no generated file is changed.

- [ ] **Step 8: Commit Task 3**

```powershell
git add Runtime/Engine/Rendering/DeferredSceneRenderer.h Runtime/Engine/Rendering/DeferredSceneRenderer.cpp Tests/Unit/RendererFrameObjectBindingTests.cpp Tests/Unit/FrameGraphSceneTargetsTests.cpp
git commit -m "fix: route deferred decals through color pass"
```

## Final Review Gates

After each task:

1. dispatch a fresh specification reviewer;
2. resolve all Critical and Important findings with the same implementer;
3. dispatch a fresh code-quality reviewer only after specification review
   passes;
4. rerun focused verification after every fix.

After all three tasks, dispatch a final cross-task reviewer and run the complete
Task 5 regression set from Task 3 Step 7. Record the evidence precisely as
DX12 blend-state/pipeline translation plus DXIL compilation; these checks are
not a real GPU runtime validation. SPIR-V compilation is likewise not Vulkan
runtime validation. Do not claim runtime validation unless a separate,
repeatable DX12 Editor/Game smoke run is actually executed and its command,
scene/project, backend-selection log, and shader/pipeline error log are
recorded. Do not use RenderDoc for this task.

## Industry Alignment And Evidence Limits

- The `benchmarks/rendering_layout.md` entry **RHI In-Render-Pass Parallel Draw
  Recording** applies to the immediate/threaded ordering, authoritative pass
  attachments, backend gate, and lifetime/synchronization contracts. Task 3
  preserves one decal pass between GBuffer and lighting and verifies both
  recorded and immediate paths select the same pass state.
- Unreal Engine 4.27's
  `Engine/Source/Runtime/Renderer/Private/DeferredDecalRendering.cpp` is the
  concrete industry reference for selecting a dedicated deferred decal shader
  stage and render-target state rather than reusing an opaque GBuffer material
  pass. Nullus deliberately implements only color/opacity in this task, not
  Unreal's broader DBuffer/material-property decal modes.
- Microsoft D3D12's `D3D12_RENDER_TARGET_BLEND_DESC::RenderTargetWriteMask`
  and independent render-target blend descriptors are the API-level reference
  for retaining source alpha as a blend factor while suppressing alpha writes.
  The DX12 unit test proves descriptor translation only; it is not GPU runtime
  evidence.
- The benchmark registry has no direct deferred-decal alpha-contract entry.
  Therefore the RHI benchmark must not be presented as proof of RGB-only decal
  semantics, and a registry addition should be considered when this subsystem
  gains normal/material decals or cross-backend runtime coverage.
