# Audio and VFX Status and Plans

This page covers audio, particles, post-processing, video, and media. None of these has a usable runtime implementation yet, but both the artifact side and the render side already have defined attachment points.

## Current status: planned

### What exists

- The artifact manifest already defines an `Audio` artifact type, so audio artifacts can be registered.
- Dependency records support source file hashes, importer version, and build target, so audio import can reuse the same dependency tracking.
- The UI side has `UILocalizationTable`, which gives later localized audio and text a consistent model.
- The editor already has panel, menu, and command registration, which can host future audio and VFX editors.

### What is missing

- No audio device, mixer, clip playback, or spatial audio
- No particle system or particle render path
- No post-processing stack (bloom, tone mapping, color grading, and so on)
- No video playback, timeline, or cinematics system

One distinction worth keeping clear: the deferred path already handles decals and transparent objects, but those are scene shading, not a visual effects system.

## Goals

| Area | Goal |
| --- | --- |
| Audio assets | Support common compressed formats, imported as runtime artifacts |
| Audio playback | One-shot and looping playback, volume, and pitch |
| Spatial audio | 3D attenuation, directivity, listener position |
| Mixing | Buses, effects, and snapshots |
| Particles | Emitters, modular updates, GPU-driven drawing |
| Post-processing | At minimum tone mapping, color grading, bloom, and depth of field |
| Video | Container playback synchronized with audio tracks |
| Timeline | Sequence cameras, audio, and events inside the editor |

## Dependencies and order

1. Audio needs a **platform audio backend** abstraction built the same way as the RHI: upper layers depend only on the abstraction while the backend handles device and format differences. Choosing the backend is an architecture decision.
2. Audio playback needs listener and source transforms per frame, which the existing component update loop can supply.
3. Particles need a batched instanced draw path that cooperates with existing instancing and draw command caching; see [Rendering Overview](../rendering/overview.md).
4. Post-processing is a render pass and should join both the forward and deferred paths as FrameGraph nodes rather than running its own render loop.
5. Video decoding needs a platform or third-party decoder and must solve resource hand-off with the render thread.
6. A timeline editor needs new asset types above scene serialization, following [Reflection and Serialization](../architecture/reflection-serialization.md).

## Not a commitment

This page describes goals and dependencies, not a schedule. When implementation status changes, the [Feature Status Matrix](../roadmap/index.md) and the source are authoritative.

## Next steps

- [Rendering Overview](../rendering/overview.md) for where post-processing belongs.
- [Feature Status Matrix](../roadmap/index.md) for the cross-domain overview.
