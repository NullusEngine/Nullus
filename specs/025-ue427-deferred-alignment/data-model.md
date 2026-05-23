# Data Model: UE4.27 Deferred Lighting Alignment

## Deferred Scene Frame

Represents one frame rendered by the deferred path.

Fields:
- `gbufferAlbedo`: material base color output.
- `gbufferNormal`: encoded world normal output.
- `gbufferMaterial`: metallic, roughness, ambient occlusion output.
- `gbufferDepth`: depth used for world-position reconstruction.
- `sceneColor`: final lit output target.
- `sceneLights`: packed active light list for the frame.
- `overlayPasses`: optional editor/debug passes appended after lighting.

Validation:
- Lighting may run only when required GBuffer resources are present.
- Overlay passes must execute after deferred lighting.

## Scene Light

Represents one active LightComponent captured for rendering.

Fields:
- `type`: point, directional, spot, ambient box, or ambient sphere.
- `position`: world-space origin for local lights.
- `forward`: world-space direction for directional/spot lights.
- `color`: RGB light color.
- `intensity`: scalar contribution.
- `effectRange`: local light range.
- `attenuation`: constant, linear, quadratic terms.
- `outerCutoff`: spot cone outer cutoff.

Validation:
- Ambient Box and Ambient Sphere contribute globally in phase-one deferred lighting.
- Directional lights contribute globally.
- Point/spot lights contribute only when the shaded point is within range.

## RenderDoc Stage Marker

Represents a stable debug label attached to a render/compute event.

Fields:
- `name`: readable stage name.
- `kind`: graphics render pass or compute dispatch.
- `order`: relative placement in the frame.

Validation:
- Deferred GBuffer precedes deferred lighting.
- Light-grid compute support precedes lighting when enabled.
- Editor overlay/debug stages follow deferred lighting.
