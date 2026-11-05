# Sprite rendering

SpriteRendererComponent submits immutable render data through the existing scene pipeline. GPU resources are device-owned/shared, never one vertex buffer per Sprite component. Quad instances and shared dynamic geometry are the two submission paths.

Per view: filter layers/visibility -> frustum cull -> stable transparent sort -> merge only adjacent compatible records -> record existing RHI commands. The batch key includes material/variant, texture/atlas page, blend/depth/mask state and geometry mode. Statistics include submitted, visible, culled sprites, batches/draws and split reasons.

Sort uses sorting-layer rank, order, sorting-group boundaries, camera/custom-axis depth and a stable object tie breaker. Mixed scenes need the same transparent submission boundary for meshes, sprites and particles. Default Sprite alpha draws test opaque scene depth and do not write it. Picking uses matching geometry and alpha coverage.

Pixel Perfect is a Camera/output feature. Follow and physics interpolation stay continuous; snapping affects render snapshots only. Upscale the logical target before screen-space UI, and test crop/letterbox/integer scaling on resize.

Sprite Lit adds exact shader/material interfaces through current ShaderArtifact/ShaderLab/graph infrastructure. Normal UVs, atlas rotation and flip handedness must agree. Light/shadow resources are scheduled through the shared graph, with configurable budgets and explicit diagnostics.

Phase 1's default material follows the existing generated 3D fallback pattern: resolve the stable SpriteUnlit Shader asset from the runtime manifest, load its compiled pass artifact, create a white transparent Material, and retain its resource lease in the immutable Sprite proxy. It does not ask the native material loader to read an authoring .mat file. Editor startup imports the built-in SpriteUnlit Shader; packaging retains it as an implicit Sprite dependency.

Direct procedural Sprite commands must pass through Driver's shared recorded-draw path. Both instanced and CPU-expanded geometry have explicit vertex/instance counts and no mesh or vertex buffer; the recorder must preserve those counts without pushing mesh object constants. GPU tests use this shared recorder and assert half-alpha blending and invisible-alpha picking in addition to geometry/depth coverage.
