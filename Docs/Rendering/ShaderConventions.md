# Shader Conventions

## Scope

This document defines the project-wide shader authoring rules for the rendering migration toward a single-source shader pipeline targeting OpenGL, Vulkan, and DX12.

The target state is:

- HLSL is the only maintained shader source language.
- Backend-specific shader binaries are generated from HLSL.
- Reflection data is generated from compiled shader artifacts, not handwritten.
- Material, pass, and frame bindings follow a stable register/space convention across all backends.

This document is normative for new rendering work. Legacy GLSL files may remain temporarily during migration, but they are not the target authoring model.

## Source Of Truth

- The only source language for shader authoring is HLSL.
- Shader entry files must live in a shader source tree that is backend-agnostic.
- Generated backend outputs are build artifacts, not source-controlled handwritten equivalents.
- No feature may land as "HLSL + manually maintained GLSL mirror".

## File Roles

Recommended logical split:

- `*.hlsl`: concrete shader entry points and pass-local logic.
- `*.hlsli`: shared includes, structs, constants, helper functions, BRDF/math utilities.

Recommended content split:

- One file owns the entry points for one pipeline stage combination or one render pass.
- Reusable math, material sampling, lighting helpers, and binding declarations go into includes.
- Backend translation products must not be edited by hand.

## Binding Model

The binding model must be explicit, stable, and reflection-friendly.

### Register Spaces

Use fixed descriptor spaces by semantic layer:

- `space0`: `Frame`
- `space1`: `Pass`
- `space2`: `Material`
- `space3`: `Object`
- `space4+`: reserved for specialized systems such as bindless tables, ray tracing, or platform experiments

Do not repurpose a lower space for convenience. If a binding belongs to per-material data, it stays in `space2` across every backend.

### Layer Rules

`Frame`

- Camera matrices
- camera position
- frame time
- viewport size
- global lighting environment
- global scene textures that are truly frame-global

`Pass`

- render-pass-specific constants
- pass-local textures and buffers
- clustered/tiled lighting buffers
- shadow pass data
- post-process parameters scoped to one pass instance

`Material`

- material scalar/vector constants
- material textures
- material samplers
- feature toggles that vary per material instance

`Object`

- object transform
- skinning palette / object-local animation data
- object ID / selection ID
- per-draw overrides

### Register Allocation

Use explicit HLSL registers for every externally bound resource.

Recommended convention:

- constant buffers: `b#`
- textures: `t#`
- structured/read-only buffers: `t#` or `StructuredBuffer`
- RW buffers/textures: `u#`
- samplers: `s#`

Example:

```hlsl
cbuffer FrameConstants : register(b0, space0)
{
    float4x4 g_View;
    float4x4 g_Proj;
    float3 g_CameraWorldPos;
    float g_Time;
};

cbuffer PassConstants : register(b0, space1)
{
    uint3 g_ClusterDims;
    float g_NearPlane;
};

cbuffer MaterialConstants : register(b0, space2)
{
    float4 g_BaseColor;
    float g_Metallic;
    float g_Roughness;
    float g_AO;
};

Texture2D g_BaseColorTex : register(t0, space2);
Texture2D g_NormalTex : register(t1, space2);
SamplerState g_LinearWrap : register(s0, space2);

cbuffer ObjectConstants : register(b0, space3)
{
    float4x4 g_Model;
};
```

Rules:

- Within one space, binding indices must be dense and deterministic.
- `b0` in one space has no relationship to `b0` in another space.
- Binding order must be derived from shader declarations or generated metadata, never from ad hoc call order in renderer code.

## Naming Conventions

### General

- Types: `PascalCase`
- functions: `PascalCase`
- local variables: `camelCase`
- global shader resources and constant-buffer members: `g_` prefix
- material parameters exposed to tools: prefer semantic names, not backend names

### Examples

- `FrameConstants`
- `MaterialConstants`
- `GetNormalWS`
- `EvaluateCookTorrance`
- `g_BaseColor`
- `g_Roughness`
- `g_ClusterLightIndices`

### Stage Entry Points

Use explicit stage names:

- `VSMain`
- `PSMain`
- `CSMain`

If multiple variants exist in one file, suffix by purpose:

- `VSDepthOnly`
- `PSDeferredLighting`
- `CSClusterBuild`

## Constant Buffer Rules

- Constant buffers must be grouped by update frequency: frame, pass, material, object.
- Do not mix per-frame and per-object fields in one cbuffer.
- Layout must be 16-byte aligned and stable under reflection.
- Avoid backend-specific packing assumptions in handwritten CPU code.
- CPU upload structs must be generated from reflection metadata or validated against it.

## Sampler Rules

- Samplers must be declared explicitly.
- Do not assume implicit sampler state from texture objects.
- Default sampler naming:
  - `g_PointClamp`
  - `g_PointWrap`
  - `g_LinearClamp`
  - `g_LinearWrap`
  - `g_AnisoWrap`

If the project later adopts immutable/static samplers, the naming and binding must remain stable in reflection output.

## Include Rules

- Shared includes must not declare backend-specific macros like `#ifdef OPENGL`.
- Shared includes may define math helpers, BRDF helpers, packing helpers, and shared parameter structs.
- One include should own one conceptual area where possible:
  - `CommonMath.hlsli`
  - `PBR.hlsli`
  - `MaterialBindings.hlsli`
  - `FrameBindings.hlsli`

## Cross-Backend Rules

- HLSL must compile through the selected compiler path for all supported backends:
  - DX12: DXIL
  - Vulkan: SPIR-V
  - OpenGL: translated path from HLSL-compiled or IR-based intermediate
- Backend-specific capabilities must be hidden behind shared abstraction macros only if the abstraction is project-owned and documented.
- Backend workarounds must live in compiler/backend translation code, not spread across shader source.

## Reflection Rules

- Reflection is generated from compiled shader artifacts and entry points.
- Material editors and runtime binding code consume reflection output, not hardcoded uniform names.
- Reflection output must include:
  - resource name
  - resource type
  - stage visibility
  - register index
  - register space
  - array size
  - cbuffer member layout

## Prohibited

The following are not allowed in new shader code:

- handwritten GLSL as a parallel maintained source
- OpenGL-only binding assumptions such as implicit texture unit conventions
- runtime string-based uniform discovery as the primary binding mechanism
- mixing frame/pass/material/object data in one catch-all constants block
- backend-specific resource naming such as `u_` for one backend and `g_` for another
- `#ifdef VULKAN` / `#ifdef OPENGL` logic inside gameplay-facing shader logic unless explicitly approved and documented
- hidden register allocation by source include order

## Migration Notes

During migration from legacy GLSL:

- Existing GLSL shaders may remain temporarily as compatibility assets.
- New features must be authored in HLSL first.
- When a pass is migrated, its runtime binding path must migrate with it; do not leave the pass on string-driven OpenGL uniform submission if reflection-backed bindings already exist.
- If one pass cannot support all backends yet, document the backend matrix explicitly in the backlog and gate the feature accordingly.

## Definition Of Done For A Shader Pass

A shader pass is considered fully migrated only when all of the following are true:

- the maintained source is HLSL only
- backend binaries for enabled targets are generated in build or asset pipeline
- reflection metadata is emitted and consumed by runtime binding code
- the pass runs through the project RHI path
- no renderer code for that pass depends on handwritten OpenGL uniform locations or GL-specific resource types
