# Deferred Decal GBuffer Alpha Protection Design

## Problem

The deferred GBuffer stores persistent per-pixel data in all three alpha
channels:

- `Albedo.a`: packed geometry-normal octahedral X;
- `Normal.a`: packed geometry-normal octahedral Y;
- `Material.a`: receive-shadows flag.

Deferred decals currently reuse the `GBuffer` shader pass. The decal pipeline
blends render target 0 with an `All` color write mask and suppresses render
targets 1 and 2. This corrupts `Albedo.a`. It also makes the decal RGB blend
factor depend on packed geometry-normal X because the reused GBuffer pass no
longer writes material opacity to `SV_Target0.a`.

## Decision

Use a dedicated `DeferredDecal` color pass and an RGB-only write mask for
GBuffer render target 0.

The decal shader outputs `float4(albedo, surfaceAlpha)`. Source alpha remains
available to the fixed-function `SrcAlpha` / `InvSrcAlpha` RGB blend, while the
RGB-only write mask prevents that alpha from reaching `Albedo.a`. Render
targets 1 and 2 remain bound, loaded, stored, and fully write-suppressed.

This preserves all three GBuffer alpha contracts without sampling or copying
the existing GBuffer in the decal shader.

## Shader Paths

Add a built-in `DeferredDecal.hlsl` shader with vertex and pixel entry points.
It samples the same base-color and opacity inputs as the standard PBR material
surface and emits only `SV_Target0`.

Add a real ShaderLab pass to `StandardPBR.shader`:

- `Name "DeferredDecal"`;
- `LightMode = "DeferredDecal"`;
- the existing alpha-test and material keyword conventions remain available;
- the pixel output is `float4(albedo, surfaceAlpha)`;
- it does not output normal or material MRT values.

Shared surface helpers may be extended to centralize base-color and opacity
evaluation when doing so removes real duplication. Pass state, resource
bindings, and entry points remain local to each pass.

## Renderer Integration

Deferred decals must resolve `LightMode=DeferredDecal`, not `LightMode=GBuffer`.
If a ShaderLab material lacks that pass, the renderer uses the built-in
`DeferredDecal.hlsl` fallback material. Opaque geometry continues to resolve
and draw `GBuffer` unchanged.

Both renderer paths use the same resolution and override rules:

1. immediate/frame-graph decal drawing;
2. threaded prepared-draw capture.

The decal pipeline state is:

- MRT0: blending enabled, existing color blend factors and operation retained,
  write mask `Red | Green | Blue`;
- MRT1: blending disabled, write mask `None`;
- MRT2: blending disabled, write mask `None`;
- independent blend enabled by the three per-target states;
- depth test enabled, depth write disabled;
- stencil test and stencil writes disabled.

The decal render pass loads and stores all three GBuffer color attachments,
does not clear them, and treats depth as read-only.

## Cache And Binding Behavior

The built-in decal fallback has its own shader/material cache identity. It must
not alias the GBuffer fallback cache entry. ShaderLab pass resolution and
material binding continue through the existing light-mode and pipeline-variant
systems; no parallel binding path is introduced.

Per-target blend states, including the RGB-only mask, remain part of the
pipeline and material variant keys.

## Tests

Follow TDD and prove each regression test fails for the intended reason before
changing production code.

Required coverage:

- renderer override test expects MRT0 RGB-only and MRT1/MRT2 `None`; mutating
  MRT0 back to `All` must fail;
- recorded-pipeline test verifies independent blending and the final three
  write masks using the production decal override helper;
- DX12 blend conversion verifies an RGB mask maps to RED, GREEN, and BLUE but
  not ALPHA;
- ShaderLab pass parsing and material resolution select `DeferredDecal` when it
  exists and use the built-in decal fallback when it does not;
- immediate and threaded deferred paths both resolve the decal pass rather than
  `GBuffer`;
- DXIL and SPIR-V compile the built-in and ShaderLab decal variants without
  skips;
- shader behavior or contract tests prove source alpha is material opacity,
  not packed geometry-normal data;
- existing GBuffer packing, deferred lighting, frame-graph ordering, PBR,
  ShaderLab, and material binding tests remain green.

## Validation

Build `NullusUnitTests` in Debug and run focused renderer, frame-graph, DX12
blend-state, PBR, ShaderLab pass, ShaderLab material, and material binding
tests. Record the backend for runtime evidence.

RenderDoc is excluded at the user's request. DXIL and SPIR-V compilation proves
shader acceptance only. Current runtime evidence is DX12-specific and must not
be extrapolated to Vulkan, Linux, or macOS.

## Acceptance Criteria

- Deferred decals never modify `Albedo.a`, `Normal.a`, or `Material.a`.
- Deferred decal RGB uses authored base-color and opacity with the existing
  source-alpha blend equation.
- Built-in fallback and ShaderLab materials follow the same decal convention.
- Immediate and threaded deferred rendering resolve the same decal pass and
  pipeline overrides.
- Opaque GBuffer layout and DeferredLighting decoding remain unchanged.
- The decal shader does not sample the GBuffer.
- No generated files are hand-edited.

## Non-Goals

- Decal normal, metallic, roughness, or ambient-occlusion modification;
- shadow-map sampling or receiver visibility implementation;
- changing forward decals;
- adding or restoring a Vulkan runtime backend;
- changing GBuffer formats or increasing GBuffer bandwidth.
