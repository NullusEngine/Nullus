# Double-Sided PBR Lighting Design

## Problem

Nullus can disable back-face culling through ShaderLab `Cull Off` or the native
material culling state, but the PBR shaders do not distinguish front-facing and
back-facing fragments. A visible back face therefore reuses the front-face
tangent frame. Direct light from the front can illuminate the back face, and a
normal map makes the error appear as bright seams or light leaking through a
thin wall.

## Decision

Treat every surviving back-facing fragment as the back side of a two-sided
surface. Use the rasterizer-provided `SV_IsFrontFace` semantic in the pixel
shader and orient the tangent frame before applying a tangent-space normal map.

For a back-facing fragment:

- negate the geometric normal;
- keep the tangent direction;
- negate the bitangent to preserve tangent-frame handedness;
- decode and apply the normal map using that oriented frame.

Front-facing fragments remain byte-for-byte equivalent in their normal setup.
When back-face culling is enabled, no back-facing fragment reaches this logic.

## Shared Helper

Add one helper to the shared shader normal utilities rather than duplicating the
orientation rule in each material shader. It accepts an `NLSTangentFrame` and
the `SV_IsFrontFace` value and returns the oriented frame. The helper must use
finite, normalized frame data already produced by `NLSBuildSafeTangentFrame`.

## Shader Paths

Apply the same rule at each normal-producing PBR path:

1. `StandardPBR.hlsl`: receive `SV_IsFrontFace` in `PSMain`, orient the input
   tangent frame, then apply `u_NormalMap`.
2. `ShaderLab/StandardPBR.shader`: receive `SV_IsFrontFace` in the Forward pixel
   entry, orient the frame, then apply `_NormalMap`. Existing ShaderLab keywords
   and pass state remain unchanged.
3. `DeferredGBuffer.hlsl`: orient the frame before normal mapping and encode the
   resulting world-space normal into the GBuffer. Deferred lighting then consumes
   an already face-correct normal and requires no front-face reconstruction.

The Cook-Torrance evaluator continues to operate on the final shading normal.
It must not infer face orientation from `NdotV`, because that value changes with
normal-map detail and camera angle.

## Cache Behavior

Increment the asset-browser thumbnail renderer version once so cached previews
created with the old one-sided normal convention are regenerated. Do not delete
the thumbnail library or alter the GPU-only prefab preview path.

## Tests And Validation

- Add a failing shader contract test covering `SV_IsFrontFace` and the shared
  tangent-frame orientation helper in all three PBR paths.
- Verify the front-face branch is identity and the back-face branch flips normal
  and bitangent but not tangent.
- Verify ShaderLab keyword declarations remain present.
- Compile `StandardPBR.hlsl` and `DeferredGBuffer.hlsl` with DXC.
- Run the focused PBR, ShaderLab, thumbnail-cache, and DX12 GPU preview tests.
- Build the Debug Editor.

RenderDoc is intentionally excluded from this change at the user's request.

## Acceptance Criteria

- A `Cull Off` plane viewed from the back uses an outward-facing back normal.
- A light on the front does not directly illuminate the back face.
- A light on the back illuminates the back face normally.
- Normal-map relief remains coherent on both sides without bright seam leakage.
- Front-face rendering and material keywords do not regress.
- Forward and deferred paths follow the same face-orientation convention.

## Non-Goals

- Shadow-map occlusion through thick or closed geometry.
- Separate Flip, Mirror, or Keep two-sided-normal material modes.
- Changes to culling defaults or the material serialization schema.
