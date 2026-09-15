# Nullus scripting API v1

Nullus uses familiar component-based runtime semantics, but its scripting runtime does not use Mono. The public script base class is `Nullus.Behaviour`; C# and Lua share the same native scripting ABI v2.

## C# scripts

Derive directly from `Behaviour`. Script authors do not write a handle constructor. Lifecycle methods use the message-method model: they are not virtual members on the base class, so they require neither `new` nor `override`. Declare an instance method with no parameters and a `void` return type. It may be `private`; the runtime caches the delegate before invoking it.

```csharp
using Nullus;

public sealed class Rotator : Behaviour
{
    private void Awake() { }

    private void Update()
    {
        transform.Rotate(Vector3.up * (90.0f * Time.deltaTime));
    }

    private void OnDestroy() { }
}
```

Use ordinary serializable fields for script state and `[SerializeField]` when an explicit field marker is needed. Do not declare a constructor taking `NativeObjectHandle`, inherit an old managed namespace, or use the old `dt` lifecycle signature.

## Lua scripts

A Lua file must return a module table. Lifecycle callbacks also take no arguments; read time from `Nullus.Time`:

```lua
local Module = {}

function Module:Awake()
    self.angle = 0.0
end

function Module:Update()
    self.angle = (self.angle + Nullus.Time.deltaTime * 90.0) % 360.0
end

return Module
```

`self.gameObject`, `self.transform`, and component members use the same object graph as C#. The Lua global table `Nullus` exposes `Find`, `FindWithTag`, `Instantiate`, `Destroy`, `GetComponent`, `AddComponent`, `Time`, `Input`, and `SceneManager`. Component queries accept `"Transform"`, `"Camera"`, `"Light"`, `"UICanvas"`, or a script type name registered in the manifest.

For a ready-made self-check UI, point a UI Canvas at `TestProject/Assets/ScriptTestPanel.ui` and put `TestProject/Assets/UiScriptTest.lua` on the same GameObject: the script updates the counter text, progress bar, and toggle by name. `ScriptRuntimeTests.LuaUiScriptDrivesTheProjectUiCanvasDocument` runs that same document and script.

The Editor's Create C# Script and Create Lua Script actions read their templates from `Assets/Editor/ScriptTemplates`. The C# template replaces `#SCRIPTNAME#` with the type name derived from the file name; a missing template fails the creation action and records a diagnostic.

## Types and common members

| Type | Main members |
| --- | --- |
| `Object` | `name`, `GetInstanceID()`, `Destroy(Object, delay)`, `Instantiate(GameObject)`, equality |
| `Component` | `gameObject`, `transform`, `tag`, `CompareTag`, `GetComponent<T>()` |
| `Behaviour` | `enabled`, `isActiveAndEnabled`, and the lifecycle callbacks |
| `GameObject` | `new GameObject(name, tag)`, `activeSelf`, `activeInHierarchy`, `tag`, `layer`, `transform`, `SetActive`, `AddComponent<T>()`, `GetComponent<T>()`, `Find`, `FindWithTag`, `CreatePrimitive` |
| `Transform` | `position`, `rotation`, `localPosition`, `localRotation`, `localScale`, `parent`, `root`, `forward` / `right` / `up`, `Translate`, `Rotate`, `LookAt`, and space conversion |
| `Camera` | `fieldOfView`, `orthographic`, `orthographicSize`, `nearClipPlane`, `farClipPlane`, `clearColor` |
| `Light` | `color`, `intensity`, `range`, `spotAngle`, `type` |
| `UICanvas` | `SetText(name, text)`, `GetText(name)`, `SetValue(name, value)`, `GetValue(name)`, `SetChecked(name, checked)`, `GetChecked(name)`, `Focus(name)`, `GetDocumentGeneration()` |

UI elements are addressed by **name**. The native element handle is a generation-tagged 64-bit value while this repository compiles Lua with `LUA_USE_C89`, so Lua integers are only 32 bits wide and the handle never crosses the script boundary; the UI Canvas members resolve name to handle internally.

`Vector2`, `Vector3`, `Vector4`, `Quaternion`, `Color`, `Ray`, and `Mathf` provide lowercase fields, constants, operators, and common math functions. Angles are in degrees; `Quaternion` uses `(x, y, z, w)` components.

## Time, input, and diagnostics

`Time.deltaTime` is scaled by `timeScale`; `unscaledDeltaTime` is not. The defaults are `fixedDeltaTime = 0.02` and `maximumDeltaTime = 0.1`. Fixed updates run before ordinary updates. `time`, `unscaledTime`, `frameCount`, and `fixedFrameCount` are available for deterministic logic.

`Input` provides `GetKey`, `GetKeyDown`, `GetKeyUp`, `GetMouseButton`, `GetMouseButtonDown`, `GetMouseButtonUp`, `mousePosition`, and `mouseScrollDelta`. Keyboard arguments use `KeyCode`.

C# provides `Debug.Log`, `Debug.LogWarning`, `Debug.LogError`, `Debug.LogException`, and `Debug.Assert`. Lua runtime errors are reported through script diagnostics and the Editor Console; use Lua's `error` function when a script needs to report a location-aware failure.

## Lifecycle and object semantics

- A component is instantiated and bound before it enters the `Awake`, `OnEnable`, and `Start` sequence.
- Changing `enabled` or the GameObject active state invokes `OnEnable` / `OnDisable` as appropriate; `isActiveAndEnabled` reflects both states.
- `Destroy` accepts a delay in seconds. Destruction is applied at a safe frame boundary, so destroying from a callback does not invalidate the current iteration.
- `Instantiate(GameObject)` clones the object hierarchy, Transform state, script components, and serialized fields.
- `SceneManager.LoadScene(path)` queues a single-active-scene synchronous switch after the current script frame. `GetActiveScene()` returns `name`, `path`, `buildIndex`, and `isLoaded`.
- Accessing an invalid or destroyed object raises a missing-reference error. Do not cache and reuse destroyed objects.

## Manifest and boundaries

## Authoring editor UI from scripts

Project files under `Assets/**/Editor/**/*.cs` compile into a separate
`GameScriptsEditor.dll` that only the editor loads, so editor-only code never
reaches a player build; every other `.cs` file still goes into
`GameScripts.dll`. Editor scripts may reference `Nullus.Editor` and extend the
editor declaratively.

A custom component inspector:

```csharp
using Nullus;
using Nullus.Editor;

namespace Nullus.GameScriptsEditor;

[CustomEditor(typeof(UICanvasComponent))]
[CanEditMultipleObjects]
public sealed class UiCanvasInspector : ComponentEditor
{
    public override void OnInspectorGUI()
    {
        EditorGUILayout.HelpBox("Drawn by a script", MessageType.Info);
        EditorGUILayout.PropertyField(serializedObject.FindProperty("document"));

        if (EditorGUILayout.Button("Reset pixels per unit"))
        {
            var pixelsPerUnit = serializedObject.FindProperty("pixelsPerUnit");
            if (pixelsPerUnit is not null)
            {
                pixelsPerUnit.floatValue = 100.0f;
                ApplyModifiedProperties();
            }
        }
    }
}
```

A custom tool window:

```csharp
using Nullus.Editor;

namespace Nullus.GameScriptsEditor;

public sealed class AssetAuditWindow : EditorWindow
{
    [EditorMenuItem("Window/Asset Audit")]
    public static void ShowWindow()
    {
        var window = GetWindow<AssetAuditWindow>() ?? new AssetAuditWindow();
        window.Open();
    }

    public override void OnGUI()
    {
        EditorGUILayout.Label("Hello from a project editor script");
    }
}
```

Rules and semantics:

- The inspected type is either a reflected engine component that exposes a
  `NativeTypeName` constant (the generator emits one such C# type per native
  component, so `typeof(UICanvasComponent)` is always available) or a project
  script component (a `Nullus.Behaviour` subclass, for example
  `[CustomEditor(typeof(RotatingCube))]`).
- Every script component shares one native `ScriptComponent` reflection type, so
  a scripted editor for a Behaviour matches on the script's own type name (the
  C# full name). One script's editor never applies to another script component.
  `editorForChildClasses` does not apply between script components yet.
- Scripts never write component objects directly. Every read and write goes
  through `serializedObject` / `SerializedProperty` and commits with
  `ApplyModifiedProperties()`, sharing Undo, Prefab overrides, and
  multi-object semantics with the built-in inspectors.
- `EditorGUILayout` exposes `Label`, `Space`, `Toggle`, `IntField`,
  `FloatField`, `Slider`, `TextField`, `Button`, `HelpBox`, `PropertyField`,
  plus `BeginHorizontal/EndHorizontal` and `BeginVertical/EndVertical`.
- Scripted editors take priority over built-in ones, so a script replaces the
  built-in editor for the same target. Duplicate
  `(target type, inheritance rule, priority)` registrations fail loudly.
- Menu entries may only extend the `Window` and `Tools` top-level menus.
- Exceptions thrown by a script are caught: that Inspector falls back to the
  generic drawing path with an error box, and an editor that keeps failing is
  retired for the session without affecting other panels.
- Recompiling scripts rescans the registry and rebuilds open editors and
  windows.

v1 does not expose scripted `CustomPropertyDrawer` registration or arbitrary
ImGui access; the widget set is the controlled API listed above.

## Manifest and boundaries

The build-generated `Library/ScriptApi/ScriptApi.json` is the v2 API manifest; module source manifests live in the corresponding `*/Gen/ScriptApi.json` files. The generator derives member IDs, public names, and the C#/Lua adapters from these manifests. ABI v1 is rejected by the runtime; C# and Lua must not maintain separate member-number tables.

v1 does not include physics, audio, coroutines, Renderer/Material, or resource-loading APIs, and it provides no MonoBehaviour compatibility type. Add a manifest entry and native adapter before expanding both language bindings, documentation, and templates.
