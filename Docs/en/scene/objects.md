# Scenes, GameObjects, and Components

A scene is the unit of load, save, and play; a GameObject is an entity inside it; components are the behavior units attached to a GameObject. Those three layers decide where nearly every editor action lands.

## Scene

`Scene` is responsible for:

- Holding the GameObject list and maintaining parent/child hierarchy.
- Registering a GameObject's components on add and unregistering them on remove.
- Driving the runtime callbacks `OnAwake`, `OnStart`, and `OnUpdate`.
- Running `OnDisable` and `OnDestroy` during shutdown.
- Collecting destroyed objects.

Two creation paths exist: `CreateGameObject` performs the normal creation flow and broadcasts the creation event, while `CreateEditorTransientGameObject` creates objects such as gizmos and helper meshes that should exist only in the editor and must not be saved into the scene. Those objects carry an editor-transient flag.

`SceneManager` handles switching between scenes; see [Lifecycle and Activation](lifecycle.md).

## GameObject

Every GameObject has at least:

| Member | Notes |
| --- | --- |
| `name` | Display name, not unique |
| `tag` | Category tag whose values come from project settings |
| `active` | Self activation state |
| `components` | Component list in insertion order |

Creating a GameObject adds a `TransformComponent` automatically, and only one can exist: requesting another Transform returns the existing instance instead of creating a new one.

Scripting surface:

| Member | Notes |
| --- | --- |
| `GetName()` / `SetName(string)` | Read and write the name |
| `GetTag()` / `SetTag(string)` | Read and write the tag |
| `GetActive()` / `IsSelfActive()` | Self activation state |
| `SetActive(bool)` | Toggle activation |
| `GetTransform()` | Retrieve the Transform component |
| `CreatePrimitive(string)` | Create a primitive child in the owning scene; the scene owns the result |

`CreatePrimitive` returns a scene-owned object that scripts must not free manually.

## Components

Built-in components:

| Component | Purpose | Key fields |
| --- | --- | --- |
| `TransformComponent` | Hierarchy transform | Local and world position, rotation, scale |
| `CameraComponent` | Image output | `Fov`, `Near`, `Far`, `Size`, projection mode, clear color |
| `LightComponent` | Light source | Type, color, intensity, `Range`, `OuterCutoff` |
| `MeshFilter` | Mesh handle | Mesh asset reference |
| `MeshRenderer` | Mesh drawing | Material reference and render toggle |
| `SkyBoxComponent` | Skybox | Skybox asset |
| `UICanvasComponent` | Screen or world-space UI | Render mode, sorting, DPI, document reference |

Shared component semantics:

- **Enabled state**: A component has its own `enabled` flag and `IsActiveAndEnabled()`. It only takes effect when the component is enabled and its host GameObject is active in the hierarchy.
- **Ordering**: Update callbacks run in component insertion order, adjustable with `MoveComponent`.
- **Destruction**: `DestroyFromOwner()` triggers `OnDisable` when the object is still active and enabled, then `OnDestroy`, exactly once.
- **Serialization extension**: Components whose state cannot be expressed with reflected fields can override `SerializeObjectGraphProperties` / `DeserializeObjectGraphProperties` and own their payload while the engine owns the graph structure.

## Adding and removing components

In the editor, add components through the Inspector's component selector and reorder them in the list.

From scripts, C# uses the generic API while Lua reaches the same members through reflection. The set of addable components is defined by reflection registration, so unregistered types never appear in the selector.

## Components and rendering

A component whose change affects rendering propagates the change through `MarkRenderStateChanged()`, which keeps components from depending on scene-layer headers. The render thread only receives incremental commands; it never reads component objects directly.

For the persistent render-side data and the per-frame payload, see [Rendering Overview](../rendering/overview.md).

## Next steps

- [Lifecycle and Activation](lifecycle.md) explains callback ordering.
- [Prefab Workflow](../prefab/workflow.md) turns an object hierarchy into a reusable asset.
