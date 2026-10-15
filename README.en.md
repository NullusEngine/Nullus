# Nullus

<p align="center">
    <img src="NullusLogo.png" width="320" alt="Nullus Engine logo">
</p>

<p align="center">
    <strong>A native C++20 engine for real-time 3D creation</strong><br>
    Scenes, assets, scripts, the Editor, runtime, and multi-backend rendering share one engine mainline.
</p>

<p align="center">
    <a href="./README.md">中文</a> | <a href="./README.en.md">English</a>
</p>

![License Source Available](https://img.shields.io/badge/License-Source%20Available-blue.svg)
![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C)
![CMake >=3.18](https://img.shields.io/badge/CMake-%E2%89%A53.18-064F8C)
![.NET 8](https://img.shields.io/badge/.NET-8.0-512BD4)
![Build Windows](https://github.com/NullusEngine/Nullus/workflows/Build%20Windows/badge.svg)
![Build Linux](https://github.com/NullusEngine/Nullus/workflows/Build%20Linux/badge.svg)
![Build macOS](https://github.com/NullusEngine/Nullus/workflows/Build%20MacOS/badge.svg)

![Nullus Editor: Sponza scene, hierarchy, inspector, transform gizmo, and Asset Browser](Docs/Screenshots/editor.png)

## Built For Real-Time Creation

Nullus is an evolving C++ 3D engine. It brings a native runtime, project launcher, visual Editor, asset importing, scene serialization, and game scripting into one workflow: build and run scenes directly in the Editor while retaining clear engine-module boundaries and an explicit RHI architecture.

### A Modern, Extensible Rendering Mainline

- **Explicit multi-backend RHI**: DX12 on Windows and Vulkan 1.1 on Linux/WSL are the currently validated runtime mainlines. A native Metal backend and macOS build target are included as well.
- **FrameGraph scene rendering**: Forward and deferred paths cover GBuffer, clustered lighting, decals, skybox, transparent objects, and Editor-view output, with graph-managed resource dependencies and state transitions.
- **One HLSL workflow**: HLSL is the single maintained shader source. DXIL, SPIR-V, and reflection data are generated from compiled artifacts, while material, pass, and object binding spaces stay consistent.
- **Large-scene rendering tools**: Visibility collection, cached draw commands, instanced draw merging, static-mesh LOD construction and runtime selection are in place. DX12 HZB occlusion is enabled conservatively according to device capabilities.

### A UE-Aligned Multi-Threaded Rendering Mainline

Rendering follows a command-based thread model: the Game Thread only produces commands, the Render Thread consumes them and builds the frame, and RHI work funnels through one submit entry point per queue type.

- **Named execution lanes**: `RenderingThreadSystem` owns the Game, Render, and RHI logical lanes and exposes `IsInGameThread` / `IsInRenderingThread` / `IsInRhiThread`. The Render and RHI lanes attach to the TaskGraph at startup, and in `Tasks` mode the RHI lane is represented by a TLS execution context rather than a fixed OS thread.
- **Render Command pipeline**: The main pipe is a strictly FIFO serial queue. Scene-proxy add/update/remove, frame rendering, resize, and resource release all enter the Render Thread through `EnqueueRenderCommand`. The async pipe only carries independent resource preparation, and its dependency event must be merged before a consumer runs.
- **Three-stage RHI executor**: Dispatch serially accepts work on the Render Thread and assigns the global submission sequence; Translate builds native command buffers in parallel on workers; Submit performs queue submission, cross-queue semaphores, readback, and Present strictly in sequence order.
- **Selectable RHI lane modes**: The `rhi_thread_mode` project setting offers `none`, `dedicated_thread`, and `tasks`. DX12 and Vulkan default to `tasks`, while Metal stays on `dedicated_thread` until thread ownership and concurrent submission are validated on device. Backends declare support through the `supportsRhiTasksMode` capability; forcing an unsupported combination fails startup with an explicit reason instead of silently falling back.
- **Frame resources and GPU fences**: A `FrameResourceRing` slot only answers "is the GPU still using these resources?", and a GPU fence decides when it becomes reusable. CPU-side frame latency is managed by `FrameLatencyController` together with the Render / Rhi / Swapchain sync depths of `RenderCommandFence`.
- **Incremental scenes and a per-frame payload**: The Game Thread no longer copies the full scene every frame. `SceneRenderStore` is the Render-Thread-owned persistent scene data, updated incrementally by commands; each frame only carries a `RenderFramePacket` (camera, views, output targets, and per-frame parameters).
- **TaskGraph scheduling**: Each worker keeps priority-partitioned local queues, external submissions land in a shared injection queue, and idle workers steal work. `TaskCompletion::Wait()` assists with runnable tasks on workers to avoid nested-wait deadlock, and `RequestCancel` propagates through the dependency graph and wakes every waiter.

### A Visual Material And ShaderLab Workflow

- **Built-in StandardPBR baseline**: The engine ships a `StandardPBR` ShaderLab asset. Model import connects materials, textures, and sub-assets to the same PBR content path.
- **Shader-defined material parameters**: ShaderLab properties support Float, Int, Range, Vector, Color, and Texture2D / TextureCube values. Materials also manage keyword sets and render-queue overrides, keeping parameters, variants, and render state on one resource object.
- **Edit and preview in the Editor**: Selecting a material asset in the Inspector exposes its render state, shader, and uniform parameters, with the parameter list generated from the material's current uniform data. Edits apply immediately and join the undo stack, while save and reload stay explicit asset actions. The Asset Browser supplies GPU thumbnails and a Preview action that opens the material in the Asset View panel.
- **Asset-managed, validated reloads**: Materials are managed as asynchronously loadable and prewarmable resource artifacts. ShaderLab reloads validate requested variants first, then update dependent materials and invalidate affected pipeline generations on success.

### From Source Files To Usable Assets

- **Project-level Asset Browser**: Browsing, search, drag and drop, thumbnails, previews, object-reference picking, and a resource catalog are part of the Editor workflow.
- **Model and material import**: glTF / FBX scenes and models can produce meshes, materials, textures, and sub-assets. Static meshes support configurable LOD import, reduction, and packaging.
- **PBR content pipeline**: Standard PBR materials, texture color-space handling, mip and compression builds, material assets, and ShaderLab asset importing are connected to the asset database.
- **Prefab and scene assets**: Create Prefab Variants from scene objects and persist scenes, object graphs, and component fields through reflection-driven serialization.

### One Object Model, Two Scripting Languages

- **C# and Lua side by side**: CoreCLR and Lua backends use one native scripting ABI and API manifest to access `GameObject`, `Transform`, `Component`, `Camera`, `Light`, input, and time.
- **Familiar component lifecycle**: `Awake`, `OnEnable`, `Start`, `Update`, `OnDisable`, and `OnDestroy` keep gameplay code close to scene objects.
- **Iterate inside the Editor**: Saving a C# script queues an incremental Asset Watcher build. A successful assembly hot-reloads at a frame boundary while preserving fields; a failed build keeps the last valid assembly alive.
- **Debugging-oriented tooling**: Generate VS Code / Visual Studio configuration, attach to the running Editor, and use C# breakpoints or a mixed-debug workflow.

### Keep The Creative Loop In The Editor

- **Project launcher**: Create, open, and manage projects before entering the matching Editor.
- **Visual scene editing**: Hierarchy, Scene View, Game View, Inspector, component picker, play controls, transform Gizmos, and hit-proxy picking support the day-to-day loop.
- **Assets stay visible**: Models, materials, textures, Prefabs, and scripts have categories, icons, and asynchronous previews in the Asset Browser.
- **Material debugging stays beside the scene**: Resource selection, property editing, and sphere preview happen in the Editor without leaving the active project context.
- **Performance is observable**: A shared profiling facade can feed Tracy or the Editor Timeline Profiler, with common CPU/GPU marker entry points for Render and RHI threads.

### Native Engine Foundations

- A component-based `Scene / SceneManager / GameObject / Component` system.
- MetaParser code-generated reflection for type registration, the Inspector, object serialization, and the scripting API manifest.
- A native JobSystem with dependencies, batching, parallel-for scheduling, and background work; rendering scheduling is handled by the priority-aware, named-lane TaskGraph with work stealing.
- Runtime modules for `Base`, `Core`, `Engine`, `Math`, `Platform`, `Rendering`, `Scripting`, and `UI`; Editor and Game share the same runtime mainline.

## Project Launcher

<p align="center">
    <img src="Docs/Screenshots/launcher.png" width="760" alt="Nullus Hub project launcher">
</p>

Create or open a project in the Launcher, then enter the Editor workspace shown above.

## Quick Start

### Requirements

- Git
- CMake 3.18 or newer
- A C++20-capable compiler
- Python 3.8 or newer for the dependency preparation script
- .NET SDK 8.0.408: CMake installs the pinned SDK inside the repository when needed and does not modify the system `PATH`

### Get The Code And Prepare Dependencies

```bash
git clone https://github.com/NullusEngine/Nullus.git
cd Nullus
git submodule update --init --recursive
```

On Windows:

```powershell
.\SetupDependencies.bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug --target Editor
```

On Linux:

```bash
./SetupDependencies.sh
./build_linux.sh debug
```

On macOS:

```bash
./SetupDependencies.sh
./build_macos.sh debug
```

Run `Editor` after the build to create a project and scene. `Game` runs project content through the same runtime mainline.

If a host environment reports `MSB6001` because both `Path` and `PATH` exist, use the repository wrapper. It cleans only the build child process:

```powershell
.\Tools\BuildCleanEnvironment.cmd cmake --build build --config Debug --target Editor
```

### Graphics Backend Notes

- Windows selects `DX12` by default.
- Linux / WSL selects `Vulkan` by default. In WSL, run `./build_wsl_vulkan.sh --install-deps` once for script prerequisites, then run `./build_wsl_vulkan.sh`.
- macOS selects `Metal` by default.
- DX11 and OpenGL are explicitly gated: startup reports them as unsupported and exits cleanly; do not target them for project delivery.

## Documentation

The full handbook lives in `Docs/` and is published bilingually to the [Wiki](https://github.com/NullusEngine/Nullus/wiki). The wiki is generated from `Docs/`; do not edit wiki pages directly.

- [Getting to Know Nullus](Docs/en/start/overview.md)
- [Environment and Build](Docs/en/getting-started/build.md)
- [Your First Project](Docs/en/getting-started/first-project.md)
- [Using the Editor](Docs/en/editor/workspace.md)
- [Scenes and Objects](Docs/en/scene/objects.md)
- [Assets and Content Pipeline](Docs/en/assets/pipeline.md)
- [Rendering, Materials, and Lighting](Docs/en/rendering/overview.md)
- [Large Scenes and Resource Management](Docs/en/large-scenes/visibility-occlusion.md)
- [Gameplay and Scripting](Docs/en/scripting/overview.md)
- [User Interface](Docs/en/ui/overview.md)
- [Platforms, Builds, and Releases](Docs/en/platform-build/platforms.md)
- [Extending the Engine](Docs/en/architecture/modules.md)
- [Debugging, Performance, and Quality](Docs/en/quality/debugging.md)
- [Reference](Docs/en/reference/layout-cli.md)
- [Roadmap](Docs/en/roadmap/index.md)

## Cross-Platform And CI

The project includes build scripts for Windows, Linux, and macOS. GitHub Actions covers builds on all three platforms and continues with reflection- and unit-test-related targets. See the platform scripts and the documentation above for configuration and dependency details.

## Contributing

Issues, suggestions, and pull requests are welcome. Read [CONTRIBUTING.md](./CONTRIBUTING.md) and [CLA.md](./CLA.md) before contributing.

## License

Nullus Engine uses a custom Source-Available License. You may view and modify the source code, use it commercially, and publish games made with Nullus Engine; you may not distribute the engine source code or use this project to create a competing engine. See [LICENSE](./LICENSE).
