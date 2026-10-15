# Feature Status Matrix

This page summarizes status across all areas and acts as a cross-chapter index. Status values:

| Status | Meaning |
| --- | --- |
| Implemented | There is a working implementation and this handbook documents how to use it |
| Partial | A usable subset or a defined foundation exists, but a key capability is missing |
| Planned | Goals and dependencies are analyzed; nothing is implemented |
| Long-term | The direction is defined but the dependency chain has not been opened up yet |

Status describes **capability**, not validation scope. An "implemented" capability is not necessarily validated on every platform; each chapter states its own platform and evidence boundaries.

## Status overview

| Area | Status | Notes | Details |
| --- | --- | --- | --- |
| Scene and object model | Implemented | GameObjects, components, Transform, hierarchy | [Scenes, GameObjects, and Components](../scene/objects.md) |
| Component lifecycle | Implemented | Awake / Enable / Start / Update / FixedUpdate / LateUpdate / Destroy | [Lifecycle and Activation](../scene/lifecycle.md) |
| Prefab | Partial | Creation/instantiation, Stage saving, and overrides are integrated; Variant/Unpack native workflows lack complete panel entry points | [Prefab Workflow](../prefab/workflow.md) |
| Asset pipeline | Implemented | Asset database, artifact manifests, dependency tracking, thumbnails | [Asset Browser and Import](../assets/pipeline.md) |
| Model import | Implemented | glTF / FBX / OBJ producing meshes, materials, textures, and Prefabs | [Models, Meshes, and Textures](../assets/models-textures.md) |
| Texture mips and compression | Partial | RGBA8/16F and BC1/3/5/7; BC6H, ASTC, and ETC2 are extension points only | Same page |
| Static mesh LOD | Implemented | Built at import, selected at runtime | [LOD and HLOD](../large-scenes/lod-hlod.md) |
| HLOD | Partial | Proxy mechanism exists; compatibility range is limited | Same page |
| Visibility pipeline | Implemented | Spatial index, comparison modes, sparse handle output | [Visibility and Occlusion](../large-scenes/visibility-occlusion.md) |
| HZB occlusion | Partial | Conservatively enabled on DX12 by capability; other backends unvalidated | Same page |
| Resource residency | Partial | Budgets and deduplication exist; streaming world loading does not | [Residency and Budgets](../large-scenes/streaming.md) |
| Render paths | Implemented | Forward and deferred, FrameGraph driven | [Rendering Overview](../rendering/overview.md) |
| Shader workflow | Implemented | HLSL single source, ShaderLab properties and variants | [Materials and ShaderLab](../rendering/materials.md) |
| Graphics backends | Partial | DX12 and Vulkan primary; native Metal; DX11 and OpenGL gated | [Graphics Backends and Large-Scene Features](../rendering/backends.md) |
| Threaded rendering | Implemented | Game / Render / RHI lanes, three RHI thread modes | [Jobs and Threading](../architecture/jobs-threading.md) |
| JobSystem | Implemented | Dependencies, batching, parallel-for, background work and main-thread continuations | Same page |
| Reflection | Implemented | MetaParser generates registration, Inspector, serialization, and script manifest | [Reflection and Serialization](../architecture/reflection-serialization.md) |
| Object graph serialization | Implemented | One format shared by scenes and Prefabs | Same page |
| C# scripting | Implemented | CoreCLR, hot reload, debugger attach | [Scripting Overview](../scripting/overview.md) |
| Lua scripting | Implemented | Lua 5.4, optional LuaPanda debugging | Same page |
| Scripting API coverage | Partial | Objects, components, camera, light, UI, time, input; no physics/audio/asset loading | [Scripting API Reference](../scripting/api.md) |
| UI runtime | Implemented | UIDocument, layout, input routing, data binding, signals, localization | [UI Overview](../ui/overview.md) |
| UI animation | Partial | Opacity tracks only | Same page |
| UI Designer | Implemented | Open and edit UI documents in the editor | [UI Designer and Editor Extensions](../ui/designer.md) |
| Editor script extensions | Partial | Inspectors, tool windows, menu items; no custom property drawers | [Editor Script Extensions](../architecture/editor-extensions.md) |
| Profiling | Implemented | Shared markers, Tracy, Timeline Profiler | [Profiling and Graphics Debugging](../quality/profiling.md) |
| RenderDoc integration | Implemented | In-app capture, runner, capture analysis | Same page |
| Test system | Implemented | Behavior tests, performance tests, reflection smoke | [Debugging and Testing](../quality/debugging.md) |
| Skeletal animation | Partial | Skeleton, skin, and clip artifacts are preserved; no runtime skinning component | [Animation Status and Plans](../animation/roadmap.md) |
| Physics | Planned | No rigid bodies, collisions, queries, or character controller | [Physics Status and Plans](../physics/roadmap.md) |
| Audio | Planned | `Audio` artifact type exists; no playback implementation | [Audio and VFX Status and Plans](../audio-vfx/roadmap.md) |
| Particles and post-processing | Planned | No particle system or post-processing stack | Same page |
| Video and timeline | Planned | No implementation | Same page |
| Gameplay AI foundation | Partial | Native state machine and pushdown automaton, not exposed to scripts | [AI and Networking Status and Plans](../ai-networking/roadmap.md) |
| Navigation and behavior trees | Planned | No implementation | Same page |
| Networking | Planned | No transport, sessions, or replication | Same page |
| Packaging and distribution | Planned | Build actions and artifacts exist; no final container or installer | [Game Runtime and Asset Manifests](../platform-build/runtime.md) |
| Terrain and world partitioning | Long-term | Depends on residency and geometry query infrastructure | [Residency and Budgets](../large-scenes/streaming.md) |
| 2D tooling | Long-term | No sprites, 2D physics, or 2D animation | This page |
| Visual scripting | Long-term | No node graph system | This page |
| Plugin ecosystem | Long-term | Editor script extensions exist; no third-party distribution model | [Editor Script Extensions](../architecture/editor-extensions.md) |
| Mobile and XR | Long-term | Outside the current build and backend targets | [Platform Support](../platform-build/platforms.md) |

## Shared dependencies behind the directions

Most planned work converges on a small set of foundations:

1. **Geometric queries** serve physics, AI navigation, and editor picking.
2. **A runtime skinning component** is the prerequisite for animation, character control, and part of particle work.
3. **Platform backend abstraction** decides whether audio, video, and mobile can integrate consistently.
4. **Residency and budgets** decide whether terrain, world partitioning, and streaming are feasible.
5. **Asset types and serialization** are the common entry point for every new editor-side feature.

That is why implementation order tends to follow the dependency chain rather than how attractive an individual feature is.

## How to read this

- To use an existing capability, read the operational part of the matching chapter.
- To judge whether a feature can ship, check the status here and then the dependency section of its chapter.
- To plan implementation, start from the shared dependencies rather than a single feature.

## Next steps

- [Engine Overview](../start/overview.md) for the capability boundaries.
- [Module Structure](../architecture/modules.md) for how the code is organized.
