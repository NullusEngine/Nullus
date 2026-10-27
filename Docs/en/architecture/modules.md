# Module Structure

Nullus is organized in three layers: runtime libraries, project programs, and tools. Runtime libraries never depend on project programs, and project programs share one set of runtime libraries.

## Repository layout

| Directory | Content |
| --- | --- |
| `Runtime/` | Engine runtime modules |
| `Project/` | Executables: Launcher, Editor, Game |
| `Managed/` | C# assemblies and code generation tools |
| `Tools/` | Build, generation, debugging, and performance tools |
| `App/` | Output directory plus engine-deployed assets |
| `Tests/` | Unit and performance tests |
| `Docs/` | Source files for this handbook |
| `App/Assets/Templates/` | Project templates; the Launcher copies their `content/` when creating a project |

## Runtime modules

| Module | Responsibility |
| --- | --- |
| `Base` | Reflection, object model, events, JobSystem, TaskGraph, threading, time, logging, and profiling markers |
| `Core` | Asset identity and metadata, asset database, artifact writing and parsing, filesystem, resource management, and serialization foundations |
| `Engine` | Scene system, components, engine-side assets, render bridge, engine settings, UI runtime, and AI |
| `Math` | Vectors, quaternions, matrices, and geometric types |
| `Platform` | Windows, input, cursors, dialogs, and process invocation |
| `Rendering` | RHI, FrameGraph, shader compilation and ShaderLab, render resources, and the editor UI bridge |
| `Scripting` | Language-independent script runtime plus the C# and Lua backends |
| `UI` | Editor interface framework and Dear ImGui extensions |
| `Animation` / `AnimationCompiler` | Poses, clips, graph evaluation, animation source import, and compilation |
| `Physics` | PhysX CPU scene query worlds and static collision geometry |
| `VFX` / `VFXCompiler` | CPU/GPU particle runtime, module-stack compilation, and backend programs |
| `MaterialCompiler` | Material/ShaderGraph compilation, dependency snapshots, and cooking |
| `Cook` | Target asset building shared by the Editor and command line |
| `Build` | Build Profiles, dependencies, packaging, platform adapters, and output validation |

Dependencies point one way: `Base` does not depend on `Engine`, and `Engine` does not depend on `Editor`. `Project/*` may depend on `Runtime/*`, never the reverse.

## Project programs

| Program | Responsibility |
| --- | --- |
| `Launcher` | Project list, creation wizard, template management, and project settings editing |
| `Editor` | Editor shell, panels, commands and undo, asset workflows, and Play mode |
| `Game` | Standalone runtime: project resolution, asset manifest loading, render loop, and script driving |

The Editor and Game share engine code, which is why the editor viewport and the runtime image come from the same render pipeline.

## C# assemblies

| Assembly | Purpose |
| --- | --- |
| `Nullus.Managed` | Runtime C# API: `Object`, `Component`, `Behaviour`, `GameObject`, `Transform`, `Camera`, `Light`, `Time`, `Input`, `SceneManager`, `Debug`, and math types |
| `Nullus.EngineScripts` | Engine-provided script samples |
| `Nullus.GameScripts` | Project scripts that enter player builds |
| `Nullus.GameScriptsEditor` | Project editor scripts loaded only in the editor process |
| `Nullus.Editor` | Editor scripting API: `ComponentEditor`, `EditorWindow`, `EditorGUILayout`, `SerializedObject` |
| `Nullus.ScriptGenerator` | Generates C# bindings from the API manifest |
| `Nullus.ScriptAnalysis` | Analyzes project scripts and produces behavior manifests and field descriptions |

## Tools

| Tool | Purpose |
| --- | --- |
| `Tools/MetaParser` | Scans C++ declarations and generates reflection registration plus the scripting API manifest |
| `Tools/DXC` | Shader compiler |
| `Tools/ReflectionTest` | Lightweight reflection pipeline smoke tool |
| `Tools/Performance` | Asset prewarm and performance helper scripts |
| `Tools/RenderDoc` | RenderDoc launch, self-check, and analysis scripts |
| `Tools/Tracy` | Profiler deployment directory |
| `Tools/Debug` | Visual Studio and VS Code extensions |
| `Tools/StackDump` | Crash stack dump tool |
| `Tools/BuildCleanEnvironment` | Cleans duplicated `Path`/`PATH` in build subprocesses |
| `Tools/Docs` | Handbook validation and wiki sync tooling |
| `Tools/NullusBuild` | Headless Player packaging, inspection, and validation |
| `Tools/Material` / `Tools/VFX` | Material/particle compilation and helper tools |
| `Tools/Icons` | Icon manifest, SVG sources, deterministic generation, and validation |

## Generated code

MetaParser produces code at build time under:

- `Runtime/<Module>/Gen/`
- `Project/<Target>/Gen/`

These directories are build output and must not be edited by hand. Change the source declarations and rebuild.

## Directories and artifacts

| Path | Committed | Content |
| --- | --- | --- |
| `Runtime/`, `Project/`, `Managed/`, `Tools/`, `Docs/` | Yes | Source and documentation |
| `Runtime/*/Gen/`, `Project/*/Gen/` | No | Generated reflection and binding code |
| `build/` | No | CMake build trees |
| `App/Win64_*`, `App/Linux_*` | No | Deployed executables and runtime libraries |
| `Library/` (inside a project) | No | Import results, caches, and IDE workspace |

## Next steps

- [Reflection and Serialization](reflection-serialization.md) covers the two data paths driven by generated code.
- [Jobs and Threading](jobs-threading.md) covers the concurrency structure of the runtime modules.
