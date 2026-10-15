# Core Concepts and Terminology

This page covers the Nullus object model and maps it onto the terms Unity and Unreal developers already use. The mapping is about "where do I find the equivalent", not about one-to-one identity.

## Object model

| Concept | Description | Lifetime |
| --- | --- | --- |
| `Scene` | Container of GameObjects; the unit of load, save, and play | Owned by `SceneManager` |
| `GameObject` | An entity in the scene with a name, tag, activation state, and components | Owned by the scene |
| `Component` | A behavior or data unit on a GameObject; every GameObject carries a `TransformComponent` | Owned by the GameObject |
| `Object` | Engine object base class providing instance IDs and handle lookup | See [Object identity](#object-identity-and-handles) |
| `Asset` | Importable content on disk with a GUID, a type, and sub-assets | Managed by the asset database |

Components are not registered through a hand-written table. MetaParser generates metadata that drives type registration, Inspector drawing, serialized fields, and the scripting API manifest from one declaration.

## Object identity and handles

Serialized references use object identifiers rather than raw pointers:

- In-scene references use a non-zero `fileID`, an empty `guid`, and `type = NonAssetType`.
- Asset references use a non-zero `fileID`, a valid asset `guid`, and the asset type; `filePath` is only a hint.

Scripts use handles rather than pointers. On the C# side that is `NativeObjectHandle`. On the Lua side UI elements are addressed by name, because the repository compiles Lua with `LUA_USE_C89`, which limits integers to 32 bits; 64-bit handles do not cross that boundary.

Field-level rules live in [Reflection and Serialization](../architecture/reflection-serialization.md).

## Frames and threads

Nullus uses a command-style threaded rendering model with three logical lanes:

- The **Game Thread** produces commands and never touches render resources directly.
- The **Render Thread** consumes commands and builds the frame.
- The **RHI lane** translates commands into native command buffers and submits them in order.

Test the current lane with `IsInGameThread()`, `IsInRenderingThread()`, and `IsInRhiThread()`. The project setting `rhi_thread_mode` accepts `none`, `dedicated_thread`, and `tasks`. The full model is in [Jobs and Threading](../architecture/jobs-threading.md).

## Mapping from Unity

| Goal | Unity | Nullus |
| --- | --- | --- |
| Scene entity | `GameObject` | `GameObject` |
| Data unit | `Component` / `MonoBehaviour` | `Component` / script component |
| Content | `Asset` | `Asset` with GUID and sub-assets |
| Asset browser | Project window | Asset Browser |
| Reusable object | Prefab | Prefab with Stage and Variant support |
| Material | Shader + Material | ShaderLab + `.mat` material |
| UI | uGUI / UI Toolkit | UIDocument + `UICanvasComponent` |
| Scripting | C# | C# and Lua |
| Serialization | YAML | Reflection-driven object graph JSON |

`Object`, `Component`, `Behaviour`, `GameObject`, `Transform`, `Camera`, and `Light` keep Unity-like names on the scripting side, and the lifecycle callbacks are also `Awake`, `OnEnable`, `Start`, `Update`, `OnDisable`, and `OnDestroy`. There is no `MonoBehaviour` compatibility type on the C# side.

## Mapping from Unreal

| Goal | Unreal | Nullus |
| --- | --- | --- |
| Scene entity | `Actor` | `GameObject` |
| Function unit | `Component` | `Component` |
| Content browser | Content Browser | Asset Browser |
| Level | Level | Scene |
| Blueprint class | Blueprint | None; use C#/Lua script components |
| Material | Material + Material Instance | ShaderLab + `.mat` material |
| Reflection | UHT codegen | MetaParser codegen |
| Scheduling | TaskGraph | TaskGraph and JobSystem |
| Render lanes | Game / Render / RHI | Game / Render / RHI |

Nullus deliberately aligns its render threading split with Unreal, but the implementation is independent. Structural resemblance is not evidence that an Unreal API or editor behavior exists here.

## Glossary

| Term | Meaning |
| --- | --- |
| MetaParser | Tool that generates reflection metadata, registration code, and the scripting API manifest from C++ declarations |
| Scripting API manifest | `ScriptApi.json`; the shared source of member IDs, signatures, and adapters for C# and Lua |
| Artifact | Runtime data produced by import, carrying platform and encoder metadata |
| Manifest | An artifact manifest describing which runtime artifacts one asset produced |
| Prefab Stage | A mode that opens a single Prefab for editing |
| Prefab Variant | A derived Prefab based on another Prefab |
| FrameGraph | A graph describing render pass dependencies and resource state transitions |
| RHI | The rendering hardware interface layer that hides DX12 / Vulkan / Metal differences |

## Next steps

- [Editor Workspace](../editor/workspace.md) maps these concepts onto the actual interface.
- [Scripting Overview](../scripting/overview.md) makes these objects move.
- [Module Structure](../architecture/modules.md) shows where each concept lives in code.
