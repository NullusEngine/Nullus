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

`self.gameObject`, `self.transform`, and component members use the same object graph as C#. The Lua global table `Nullus` exposes `Find`, `FindWithTag`, `Instantiate`, `Destroy`, `GetComponent`, `AddComponent`, `Time`, `Input`, and `SceneManager`. Component queries accept `"Transform"`, `"Camera"`, `"Light"`, or a script type name registered in the manifest.

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

The build-generated `Library/ScriptApi/ScriptApi.json` is the v2 API manifest; module source manifests live in the corresponding `*/Gen/ScriptApi.json` files. The generator derives member IDs, public names, and the C#/Lua adapters from these manifests. ABI v1 is rejected by the runtime; C# and Lua must not maintain separate member-number tables.

v1 does not include physics, audio, coroutines, Renderer/Material, or resource-loading APIs, and it provides no MonoBehaviour compatibility type. Add a manifest entry and native adapter before expanding both language bindings, documentation, and templates.
