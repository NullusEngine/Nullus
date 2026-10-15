# Animation Status and Plans

This page states where animation and character support stands, what already exists, where the gaps are, and what constrains the implementation order. It is organized as status, evidence, and dependency, and it commits to no version or date.

## Current status: partial

### What exists

- Import preserves skeleton, skin, animation clip, and morph target data, registering them in the artifact manifest as `Skeleton`, `Skin`, `AnimationClip`, and `MorphTarget`.
- Those artifacts participate in dependency tracking and versioning, and regenerate when the source file or importer changes.
- The component update loop already separates regular, fixed-step, and late updates (`OnUpdate`, `OnFixedUpdate`, `OnLateUpdate`).

### What is missing

- No runtime skinning component: imported skeleton and skin data cannot drive a mesh at runtime yet.
- No animation clip player, state machine, blending, or transitions.
- No animation editor and no animation curve assets.
- No character controller.

When `ModelPrefabBuilder` encounters a model with skins or animations, it records the diagnostic code `runtime-skinning-component-missing`: imported skeleton, skin, and animation data is preserved as artifacts, but static generated Prefabs cannot play it until runtime skinning components exist. That diagnostic is the authoritative statement of the current state.

The UI side has a separate `UIAnimationPlayer` that only provides opacity tracks for interface animation. It is not a character animation system.

## Goals

| Capability | Goal |
| --- | --- |
| Runtime skinning | Drive mesh vertices from a bone hierarchy, supporting single and multiple roots |
| Animation clips | Sample keyframes, loop modes, playback rate |
| Animation state machine | States, transition conditions, blend durations |
| Blending | Single-layer blending and masked blending |
| Morph targets | Weight-driven deformation |
| Animation editor | Inspect and adjust clips and state machines in the editor |
| Character control | Work with physics or with an independent kinematic controller |

## Dependencies and order

1. **A runtime skinning component** is the prerequisite for everything else. Clip playback and state machines need a target that can consume bone data.
2. Skinning requires mesh artifacts to carry bone weights and bone indices, plus a render path that updates vertices from bone transforms. That depends on render-side instance data and constant updates and falls under [Rendering Overview](../rendering/overview.md).
3. Animation clips and state machines need new asset types and serialization support, following the field and reference rules in [Reflection and Serialization](../architecture/reflection-serialization.md).
4. Character control needs a kinematic or dynamic solver. A kinematic version can land before any physics system exists; a dynamic version depends on [Physics Status and Plans](../physics/roadmap.md).
5. Editor-side work adds editors to the existing Inspector and panel model, following the registration conventions in [Editor Script Extensions](../architecture/editor-extensions.md).

## Not a commitment

This page describes goals and dependencies, not a schedule. When implementation status changes, the [Feature Status Matrix](../roadmap/index.md) and the source are authoritative.

## Next steps

- [Models, Meshes, and Textures](../assets/models-textures.md) for where skeleton and skin data is stored.
- [Feature Status Matrix](../roadmap/index.md) for the cross-domain overview.
