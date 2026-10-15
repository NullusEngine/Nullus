# Scripting Overview

Nullus ships two scripting backends, C# and Lua. They are not two object models but two language front ends over one native runtime: script component types, instance handles, field access, and lifecycle all go through the same ABI and one shared API manifest.

## Architecture

```text
native scene objects
    |
ScriptRuntime (language independent)
    |-- IScriptBackend -> CoreCLR (C#)
    |-- IScriptBackend -> Lua 5.4 (Lua)
    |
ScriptApiDatabase (ScriptApi.json)
    |
generated strongly typed C# bindings / reflective Lua bindings
```

Key constraints:

- There is one language-independent script component type; C# and Lua are not separate component kinds.
- `ScriptApiDatabase` is the only portable contract between native reflection and script bindings.
- Member IDs, public names, and both language adapters are generated from the manifest, so no one maintains a second numbering scheme.
- A native ABI version mismatch makes the runtime refuse to load rather than attempt compatibility.

## Script assets

Dropping a `.cs` or `.lua` file into the project's `Assets/` directory puts it into the asset pipeline:

- Saving `.cs` triggers an incremental build and, on success, a hot reload.
- Saving `.lua` takes effect on reload with no build step.
- Both appear in the Asset Browser with their own category icons.

## Lifecycle callbacks

Both languages use the same callback names:

| Callback | When it runs |
| --- | --- |
| `Awake` | First entering the running state, once |
| `Start` | First entering the running state, after `Awake`, once |
| `OnEnable` | On every transition from disabled to enabled |
| `OnDisable` | On every transition from enabled to disabled |
| `Update` | Every frame |
| `FixedUpdate` | Fixed-step frames |
| `LateUpdate` | Every frame, after `Update` |
| `OnDestroy` | On destruction, once |

On the C# side a callback must be an instance method with no parameters returning `void` and an exact name match; a static method or a same-named method with parameters is not recognized.

Native callback ordering and activation gating are in [Lifecycle and Activation](../scene/lifecycle.md).

## C# versus Lua

| Dimension | C# | Lua |
| --- | --- | --- |
| Runtime | CoreCLR | Lua 5.4 |
| Binding | Generated strongly typed bindings | Reflective, lazy bindings |
| Script shape | A class deriving from `Nullus.Behaviour` | A returned module table |
| Iteration | Incremental build and hot reload on save | Reload on save, no build step |
| Debugging | Breakpoints, attach, mixed mode | Optional LuaPanda integration |
| Integer width | 64-bit | 32-bit (`LUA_USE_C89`) |

Because Lua integers are 32-bit, 64-bit object handles do not cross the script boundary. UI elements are addressed by name, and `UICanvasComponent` resolves names to handles internally.

## C# script shape

```csharp
using Nullus;

public sealed class ExampleBehaviour : Behaviour
{
    private void Awake()
    {
        Debug.Log($"{name} is ready");
    }

    private void Update()
    {
        transform.Rotate(Vector3.up * (30.0f * Time.deltaTime));
    }
}
```

`Behaviour` derives from `Component`, so `gameObject`, `transform`, `tag`, `enabled`, and `isActiveAndEnabled` are directly available.

## Lua script shape

```lua
local Module = {}

function Module:Awake()
    Nullus.Debug.Log("ready")
end

function Module:Update()
    local position = self.transform.localPosition
    position.x = position.x + Nullus.Time.deltaTime
    self.transform.localPosition = position
end

return Module
```

A script returns a table, the lifecycle functions are members of that table, and the table itself is passed as `self`. The engine API is exposed through the global `Nullus` table, for example `Nullus.Time` and `Nullus.Debug`.

## Binding scripts to scenes

A script component attaches to a GameObject, and its serializable fields participate in object graph serialization. The generator writes the C# field list into the script manifest, which the editor uses both to draw editable rows in the Inspector and to persist field values when saving a scene.

Fields survive a hot reload where possible: after a successful build the editor swaps to the new assembly at a frame boundary and rebinds. Existing instances only see the backend replacement wrapped in `OnDisable` and `OnEnable`; the engine never synthesizes `Awake`, `Start`, or `OnDestroy`.

## Scripts and the editor

Files under `Assets/**/Editor/**/*.cs` belong to the editor assembly, are loaded only in the editor process, and may reference `Nullus.Editor` to register custom Inspectors, tool windows, and menu items. Player builds exclude them. See [Editor Script Extensions](../architecture/editor-extensions.md).

## Current boundaries

- No `MonoBehaviour` compatibility type
- No coroutines
- The scripting API does not cover physics, audio, or Renderer/Material
- No asset loading API

Adding one of these means extending the API manifest and the native adapters first, then updating both languages' documentation and templates. The order cannot be reversed. See the [Scripting API Reference](api.md) and the [Feature Status Matrix](../roadmap/index.md).

## Next steps

- [Scripting API Reference](api.md) lists the available types and members.
- [Script Debugging and Hot Reload](debugging.md) covers breakpoints, attach, and troubleshooting.
