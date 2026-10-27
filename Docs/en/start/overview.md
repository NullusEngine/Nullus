# Engine Overview

Nullus is a C++20 engine for real-time 3D work. The runtime, project launcher, visual editor, asset import, scene serialization, and gameplay scripting share one pipeline: the same runtime that drives the editor viewport also drives the shipped game process.

This handbook is organized as "get a result first, understand the internals second." If you are new to Nullus, start with [Your First Project](../getting-started/first-project.md) and come back here for the capability boundaries.

## What the engine does

| Area | Current capability |
| --- | --- |
| Scene | `Scene` / `SceneManager` / `GameObject` / component model with reflection-driven object graph serialization |
| Editor | Hierarchy, Inspector, Scene View, Game View, Asset Browser, Console, Profiler, Project Settings, UI Designer |
| Assets | Project-level asset database, model and texture import, artifact manifests, thumbnails and async previews |
| Rendering | Explicit multi-backend RHI, FrameGraph-driven forward and deferred paths, PBR materials, ShaderLab |
| Scripting | C# (CoreCLR) and Lua 5.4 backends sharing one scripting API manifest |
| Large scenes | Spatial index, visibility collection, LOD and HLOD, residency budgets, DX12 HZB occlusion |
| Scheduling | Native JobSystem plus TaskGraph with priorities, named lanes, and work stealing |
| Animation | Animator, GPU skinning, graphs, state machines, BlendSpace, and Animation Workbench |
| VFX and queries | CPU/GPU particles, module-stack editor, Sprite/Mesh rendering, and static PhysX scene queries |
| Packaging | Build Profiles, target cooking, `.npak` content, and desktop Player assembly |
| Platforms | Windows / Linux / macOS build targets; Windows DX12 and Linux Vulkan are the validated runtime lines |

A finer breakdown with validation status lives in the [Feature Status Matrix](../roadmap/index.md).

## Current capability limits

Animation, particles, static physics queries, and desktop packaging have implementations, with delivery boundaries documented in their chapters. These areas remain incomplete:

- Dynamic rigid bodies, character controllers, audio, and video playback
- Root Motion, IK, retargeting, complete particle modules/data interfaces, and a general post-processing stack
- Navigation meshes, behavior trees, multiplayer, and replication
- Installers, automatic updates, content delivery services, and mobile delivery
- 2D tooling, gameplay visual scripting, and a plugin marketplace

Material, animation, and particle graphs are specialized authoring tools rather than a gameplay visual scripting system. Each chapter separates implementation, automated tests, device execution, and mouse-driven acceptance.

## Who this is for

- **Game developers** build scenes in the editor, import assets, and write C# or Lua. See [Editor Workspace](../editor/workspace.md) and [Scripting Overview](../scripting/overview.md).
- **Technical artists** author ShaderLab, tune materials, and manage texture and mesh build pipelines. See [Materials and ShaderLab](../rendering/materials.md) and [Models, Meshes, and Textures](../assets/models-textures.md).
- **Engine developers** extend runtime modules, rendering backends, or the editor. See [Module Structure](../architecture/modules.md).
- **Teams migrating from another engine** should read [Core Concepts and Terminology](concepts.md) first; it maps Unity and Unreal concepts onto Nullus.

## Versioning and compatibility

The engine has no stable release number yet. The project version in `CMakeLists.txt` is `1.0.0` and identifies builds and Player/package compatibility; it is not a stable API guarantee. The repository does not promise source API compatibility; read the commit history and the [Feature Status Matrix](../roadmap/index.md) when upgrading.

The scripting API has its own versioning: `ScriptApi.json` is currently schema version `2`. A native scripting ABI mismatch makes the runtime refuse to load rather than attempt compatibility. See the [Scripting API Reference](../scripting/api.md).

## License

Nullus Engine uses a custom source-available license. You may read and modify the source, use it commercially, and ship games made with Nullus Engine; redistributing the engine source or using this project to build a competing engine is not permitted. See `LICENSE` in the repository root for the full terms, and `CONTRIBUTING.md` plus `CLA.md` for the contribution process.

## Next steps

1. [Environment and Build](../getting-started/build.md) turns the source into a running editor.
2. [Your First Project](../getting-started/first-project.md) creates a project, builds a scene, and attaches a first script.
3. [Core Concepts and Terminology](concepts.md) maps what you already know from Unity or Unreal onto Nullus.
