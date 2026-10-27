# Animation Status and Plans

Animation now includes runtime playback, GPU skinning, and Animation Workbench in addition to imported artifacts. Its status is **partial**: authoring and playback exist, while advanced character features and full product acceptance remain incomplete.

## Implemented path

- glTF/GLB and FBX skeletons, skins, multiple animation Takes, and native subresources.
- Animator and Skinned Mesh Renderer, CPU pose evaluation, four-influence GPU skinning, and dynamic bounds.
- Clips, graphs, state machines, BlendSpace, BoneMask, curves, and Notify events.
- Workbench previews, Undo/Redo, Working / Saved / Applied states, and retention of the previous program after failure.
- Cooking, frozen dependencies, standalone Player playback, and reflected/scriptable Animator controls.

Start with [Animation Authoring Workflow](workflow.md) and [Animation Validation](validation.md). UI opacity animation is a separate system.

## Limits

The first implementation uses serial evaluation, floating-point TRS, and four influences per vertex. Dynamic bounds use CPU reference deformation. Character budgets and platform acceptance need separate results.

Root Motion, Montage/Slot, Marker Sync, IK, Sockets, retargeting, Morph Targets, parallel evaluation, animation LOD, and Compute Skinning remain incomplete. Character movement also needs the queries or simulation described in [Physics Status and Plans](../physics/roadmap.md); an animation graph does not provide a character controller.

## Acceptance scope

Import/compiler tests, GPU readback, package self-tests, packaged images, and mouse-driven interaction are separate checks. Results from another path do not establish Metal execution, full Vulkan deferred rendering, skinned Picking/Selection/DepthOnly, or Autodesk SDK support.

- [Models, Meshes, and Textures](../assets/models-textures.md) covers import foundations.
- [Feature Status Matrix](../roadmap/index.md) summarizes cross-domain status.
