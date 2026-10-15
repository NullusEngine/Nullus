# Materials and ShaderLab

A material is a shader plus parameters plus render state. ShaderLab is the source format that describes shaders and material properties, and a material is an asset that references a shader.

## ShaderLab structure

```hlsl
Shader "Nullus/StandardPBR"
{
    Properties
    {
        _BaseMap("Base Map", Texture2D) = "white"
        _BaseColor("Base Color", Color) = (1, 1, 1, 1)
        _Metallic("Metallic", Range(0, 1)) = 0
        _Roughness("Roughness", Range(0, 1)) = 1
    }

    SubShader
    {
        Tags
        {
            "RenderPipeline" = "Nullus"
            "RenderType" = "Opaque"
            "Queue" = "Geometry"
        }

        Pass
        {
            Name "Forward"
            Tags { "LightMode" = "Forward" }
            Cull Back
            ZWrite On
            ZTest LessEqual
            Blend Off
        }
    }
}
```

Four top-level pieces:

| Block | Purpose |
| --- | --- |
| `Shader "name"` | Stable shader name and display path |
| `Properties` | Material-exposed properties and defaults |
| `SubShader Tags` | Render pipeline, render type, and render queue |
| `Pass` | Entry points, render state, and HLSL code |

## Property types

| Type | Default syntax | Purpose |
| --- | --- | --- |
| `Float` | `= 1` | Scalar |
| `Range(min, max)` | `= 0.5` | Bounded scalar shown as a slider |
| `Vector` | `= (1, 0, 0, 0)` | Four components |
| `Color` | `= (1, 1, 1, 1)` | Color with a color picker |
| `Texture2D` | `= "white"` | 2D texture |
| `TextureCube` | Cube map name | Cube map |
| `Int` | `= 0` | Integer |

Texture defaults use built-in names (`white`, `black`, `bump`) instead of paths, so a material without an assigned texture still has a defined result.

## Pass render state

| Statement | Values | Notes |
| --- | --- | --- |
| `Cull` | `Back` / `Front` / `Off` | Face culling |
| `ZWrite` | `On` / `Off` | Depth write |
| `ZTest` | `LessEqual` or another comparison | Depth comparison |
| `Blend` | `Off` / `SrcAlpha OneMinusSrcAlpha` and others | Blend mode |

Transparent objects disable depth writes and use alpha blending; opaque objects write depth and use `LessEqual`.

## Shader variants

```hlsl
#pragma vertex VSMain
#pragma fragment PSMain
#pragma shader_feature _ALPHATEST_ON
#pragma multi_compile _ _NORMALMAP
#pragma multi_compile _ MAIN_LIGHT_SHADOWS
```

`shader_feature` is for keywords only compiled when a material actually references them. `multi_compile` is for keywords that need the full permutation. The variant set drives compile cost, and the renderer only accepts compiled combinations.

## Built-in shaders

| Shader | Purpose |
| --- | --- |
| `Nullus/StandardPBR` | Standard PBR opaque material |
| `Nullus/ShaderLab/Transparent` | Alpha-blended transparent material |
| `Nullus/ShaderLab/UnlitColor` | Unlit solid color |
| `Nullus/ShaderLab/UnlitTexture` | Unlit textured |
| `Nullus/ShaderLab/AlphaTest` | Alpha cutout |
| `Nullus/ShaderLab/ForwardDepthOnly` | Depth prepass |

The deferred path uses dedicated HLSL shaders: `DeferredGBuffer`, `DeferredLighting`, `DeferredDecal`, `Skybox`, `HZBBuild`, and `HZBOcclusion`.

## StandardPBR parameters

Properties exposed by `Nullus/StandardPBR`:

- `_BaseMap` / `_BaseColor`: base texture and color
- `_Metallic` / `_Roughness` / `_AmbientOcclusion`: scalar metallic, roughness, and ambient occlusion
- `_MetallicMapChannel` / `_RoughnessMapChannel`: which channel to read from a packed texture
- `_MetallicMap` / `_RoughnessMap` / `_OcclusionMap` / `_NormalMap` / `_OpacityMap` / `_EmissiveMap` / `_SpecularMap`: the individual maps
- `_EmissiveColor` / `_SpecularColor`: emissive and specular tints
- `_NormalScale`: normal strength
- `_Cutoff`: alpha cutoff threshold

Metallic and roughness can come from separate maps or from channels of one packed map, which avoids storing one texture per channel.

## Shared shader library

`NullusShaderLibrary` provides reusable HLSL headers:

| Header | Content |
| --- | --- |
| `Core.hlsl` | Base types and common functions |
| `Common.hlsl` | Shared utilities |
| `BRDF.hlsl` | Lighting model |
| `Lighting.hlsl` | Light evaluation |
| `PBRNormals.hlsl` | Normal handling |
| `StandardPBRSurface.hlsl` | StandardPBR surface data |
| `Shadows.hlsl` | Shadow sampling |
| `SpaceTransforms.hlsl` | Space conversions |
| `Instancing.hlsl` | Instancing data |
| `Texture.hlsl` | Texture sampling wrappers |
| `Fog.hlsl` | Fog |

New shaders should reuse these headers rather than copying implementations.

## Material workflow in the editor

- Selecting a material asset in the Inspector edits render state, shader, and uniform parameters.
- Parameter rows come from the material's current uniform data, so no hand-written entries can drift from the shader.
- Changes take effect immediately and enter the undo stack.
- Saving and reloading are explicit resource actions.
- The Asset Browser provides GPU thumbnails, and a sphere preview opens in the Asset View.

## ShaderLab reload

Reloading a shader first validates the requested variants. On success it updates the associated materials and invalidates the affected pipeline generations. A validation failure keeps the previous state so the scene never ends up half updated.

## Shader conventions

### File roles

| Extension | Content |
| --- | --- |
| `*.hlsl` | Concrete shader entry points and pass-local logic |
| `*.hlsli` | Shared includes, structs, constants, and BRDF/math utilities |

One file owns the entry points for one pipeline stage combination or one render pass. Reusable math, material sampling, lighting helpers, and binding declarations belong in includes. Backend translation products must never be edited by hand.

### Binding spaces

| Space | Content |
| --- | --- |
| `set0` | Frame / Scene |
| `set1` | Material |
| `set2` | Object |
| `set3` | Pass / Rare |

Do not repurpose a lower space for convenience. Data that belongs to a material stays in the Material space across every backend.

### Register allocation

Write an explicit HLSL register for every externally bound resource:

| Resource | Convention |
| --- | --- |
| Constant buffers | `b#` |
| Textures | `t#` |
| Structured/read-only buffers | `t#` or `StructuredBuffer` |
| Read-write buffers and textures | `u#` |
| Samplers | `s#` |

```hlsl
cbuffer MaterialConstants : register(b0, space1)
{
    float4 g_BaseColor;
    float g_Metallic;
    float g_Roughness;
};

Texture2D g_BaseColorTex : register(t0, space1);
SamplerState g_LinearWrap : register(s0, space1);

cbuffer ObjectConstants : register(b0, space2)
{
    float4x4 g_Model;
};
```

Rules:

- Within one space, binding indices must be dense and deterministic.
- `b0` in one space has no relationship to `b0` in another.
- Binding order must come from shader declarations or generated metadata, never from ad hoc call order in renderer code.

### Naming

| Object | Convention | Examples |
| --- | --- | --- |
| Types | PascalCase | `FrameConstants`, `MaterialConstants` |
| Functions | PascalCase | `GetNormalWS`, `EvaluateCookTorrance` |
| Local variables | camelCase | `baseColor` |
| Global resources and constant buffer members | `g_` prefix | `g_BaseColor`, `g_ClusterLightIndices` |

Use explicit stage entry point names: `VSMain`, `PSMain`, `CSMain`. When one file has multiple variants, suffix by purpose, for example `VSDepthOnly`, `PSDeferredLighting`, or `CSClusterBuild`.

Material parameters exposed to tools should prefer semantic names over backend names.

### Constant buffer rules

- Group constant buffers by update frequency: frame, pass, material, object.
- Never mix per-frame and per-object fields in one buffer.
- Layout must be 16-byte aligned and stable under reflection.
- Do not assume backend-specific packing in handwritten CPU code.
- CPU upload structs must be generated from reflection metadata or validated against it.

### Sampler rules

Declare samplers explicitly and never assume implicit sampler state from a texture object. Default names: `g_PointClamp`, `g_PointWrap`, `g_LinearClamp`, `g_LinearWrap`, `g_AnisoWrap`.

### Include rules

Shared includes must not declare backend-specific macros such as `#ifdef OPENGL`; they may provide math, BRDF, packing helpers, and shared parameter structs. One include should own one conceptual area, for example `CommonMath.hlsli`, `PBR.hlsli`, or `MaterialBindings.hlsli`.

### Prohibited

- Maintaining one HLSL source alongside a hand-written GLSL mirror for the same feature
- Editing backend translation products directly
- Writing backend-specific branches inside shared includes
- Replacing reflection-generated binding information with hand-maintained numbering

## Next steps

- [Rendering Overview](overview.md) places materials in the frame.
- [Models, Meshes, and Textures](../assets/models-textures.md) covers texture import and compression.
