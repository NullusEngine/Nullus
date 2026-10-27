# Audio and VFX Status and Plans

Particles have a CPU/GPU runtime and module-stack editor. Audio, post-processing, video, and timeline remain planned. These domains have different implementation states.

## Particles: partial

Particle Editor combines System Overview, module stacks, dynamic inputs, parameters, curves/gradients, timeline, diagnostics, and whole-system preview. Scene components consume compiled systems, Sprite/Mesh rendering integrates with the scene pipeline, and importing/cooking retains dependencies.

Start with [Particle Authoring Workflow](particles.md). Full Niagara compatibility, complete data interfaces, all renderers, and all-platform device coverage are not promised. Legacy graph files are not migrated automatically.

## Audio: planned

The artifact manifest defines `Audio` and provides source/importer dependency tracking, but there is no usable audio device, mixer, playback component, or spatial audio. This requires a platform audio abstraction, decoding, resource lifetimes, and scene listener/source updates.

## Post-processing and media: planned

Decals, transparent objects, and particles in the render pipeline do not form a complete post-processing stack. There is no general bloom, color-grading, depth-of-field, video playback, or camera/audio timeline workflow.

Post-processing should integrate through FrameGraph. Video also needs decoder-to-render-thread resource transfer; timeline needs persistent assets, editor sequencing, and event synchronization.

## Validation scope

Particle source compilation, focused CPU/GPU tests, mouse-driven editing, scene playback, and standalone output need separate acceptance. Unimplemented media features do not have invented menus or procedures.

- [Rendering Overview](../rendering/overview.md) covers drawing and FrameGraph.
- [Feature Status Matrix](../roadmap/index.md) summarizes cross-domain status.
