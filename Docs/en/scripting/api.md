# Scripting API Reference

This page describes the public scripting surface. The shared native API metadata is `Library/ScriptApi/ScriptApi.json` at schema version `2`; C# projects it into managed types, while Lua installs its own runtime table and userdata adapters. Metadata availability does not imply identical language APIs.

## Language scope and exact signatures

The member tables below primarily describe C#. Signatures and nullability come from [GeneratedBindings.cs](../../../Managed/Nullus.Managed/GeneratedBindings.cs); component, scene, input, and time helpers also live in [Behaviour.cs](../../../Managed/Nullus.Managed/Behaviour.cs). Selected complete C# signatures are:

| Signature | Semantics |
| --- | --- |
| `public static void Destroy(Object? target, float delay = 0.0f)` | Null or already destroyed targets are ignored; the managed wrapper is invalidated immediately |
| `public static GameObject Instantiate(GameObject original)` | Original must be non-null and alive; clones its hierarchy |
| `public T? AddComponent<T>() where T : Component` | Instance method on GameObject; check the nullable result |
| `public T? GetComponent<T>() where T : Component` | Instance method on GameObject; absence returns null |
| `public static GameObject? Find(string objectName)` | Missing name returns null |
| `public void Translate(Vector3 translation, Space relativeTo)` | The one-argument overload uses `Space.Self` |
| `public void Rotate(Vector3 eulerAngles, Space relativeTo)` | Degrees; the one-argument overload uses `Space.Self` |
| `public Vector3 InverseTransformPoint(Vector3 point)` | Inverts position, rotation, and scale; zero-scale components resolve to zero |

Lua's actual adapter is [LuaScriptBackend.cpp](../../../Runtime/Scripting/LuaScriptBackend.cpp). Use `Nullus.Find(name)`, `object:GetComponent(typeName)`, and `object:CreatePrimitive("Cube")`; Lua has no C# generic syntax or `new GameObject(...)` constructor. Lua vectors are tables with `x/y/z` fields. Read, modify, and assign back a Transform property to change the native value. Managed convenience methods such as `Transform.Rotate` are not automatically installed on Lua userdata.

For UI, C# uses the exported element handle members listed below. Lua addresses an element by name through `canvas:SetText(name, text)`, `GetText(name)`, `SetValue(name, value)`, `GetValue(name)`, `SetChecked(name, checked)`, `GetChecked(name)`, and `Focus(name)`. It also exposes `GetDocumentGeneration()`. Do not pass a C# element handle into Lua or assume the C# configuration member list is available there.

## Object

Base class for engine objects.

| Member | Notes |
| --- | --- |
| `name` | Object name |
| `GetInstanceID()` | Instance ID |
| `Destroy(Object, delay = 0)` | Request destruction, optionally delayed by seconds |
| `Instantiate(GameObject)` | Clone an object hierarchy |
| Equality | Compares handles; a destroyed object compares as null |

Accessing an invalid or destroyed object raises a missing reference error. Do not cache and reuse destroyed objects.

## Component

| Member | Notes |
| --- | --- |
| `gameObject` | Host object |
| `transform` | Host Transform |
| `tag` | Host tag |
| `CompareTag(string)` | Tag comparison |
| `GetComponent<T>()` | Component on the same object |

## Behaviour

Base class for script components.

| Member | Notes |
| --- | --- |
| `enabled` | Component's own enabled state |
| `isActiveAndEnabled` | Enabled and the host is active in the hierarchy |

## GameObject

| Member | Notes |
| --- | --- |
| `GameObject(string name = "New GameObject", string tag = "")` | Create an object |
| `name` | Read and write the name |
| `tag` | Read and write the tag |
| `layer` | Read and write the layer |
| `activeSelf` | Self activation state |
| `activeInHierarchy` | Hierarchy activation state |
| `transform` | Transform component |
| `SetActive(bool)` | Toggle activation |
| `AddComponent<T>()` | Add a component |
| `GetComponent<T>()` | Retrieve a component |
| `GameObject.Find(string)` | Find by name |
| `GameObject.FindWithTag(string)` | Find by tag |
| `GameObject CreatePrimitive(string primitiveType = "Cube")` | Instance method; creates geometry in the caller's scene and parents it to the caller |

```csharp
var root = new GameObject("Example Root");
GameObject cube = root.CreatePrimitive("Cube");
cube.transform.localPosition = new Vector3(0.0f, 0.0f, 5.0f);
```

Call `CreatePrimitive` on an instance, for example `root.CreatePrimitive("Cube")`. The signature comes from the [current managed binding](../../../Managed/Nullus.Managed/GeneratedBindings.cs). The caller must belong to a valid scene; an unsupported primitive type or invalid object causes creation to fail.

## Transform

| Member | Notes |
| --- | --- |
| `position` / `rotation` | World-space position and rotation |
| `localPosition` / `localRotation` | Local-space position and rotation |
| `eulerAngles` / `localEulerAngles` | Euler angle access |
| `localScale` / `lossyScale` | Scale |
| `parent` / `root` | Hierarchy relations |
| `forward` / `right` / `up` | Direction vectors |
| `Translate(Vector3)` | Translation, self-relative by default |
| `Rotate(Vector3 eulerAngles)` | Rotation, self-relative by default |
| `LookAt(Vector3)` / `LookAt(Transform)` | Aim at a target |
| `TransformPoint` / `InverseTransformPoint` | Point space conversion |
| `TransformDirection` / `InverseTransformDirection` | Direction space conversion |

Angles are in degrees, and `Quaternion` uses `(x, y, z, w)` components.

## Camera

| Member | Notes |
| --- | --- |
| `fieldOfView` | Perspective field of view |
| `orthographic` | Whether projection is orthographic |
| `orthographicSize` | Orthographic size |
| `nearClipPlane` / `farClipPlane` | Clip planes |
| `clearColor` | Clear color |

## Light

| Member | Notes |
| --- | --- |
| `color` | Color |
| `intensity` | Intensity |
| `range` | Influence range |
| `spotAngle` | Outer cone angle |
| `type` | Light type |

## UICanvasComponent

UI elements are addressed by name. Current script members:

| Member | Notes |
| --- | --- |
| `LoadDocumentScript(string)` | Load a UI document |
| `GetDocumentGeneration()` | Document generation |
| `GetElementHandle(string)` | Resolve an element handle by name |
| `GetElementText(h)` / `SetElementText(h, string)` | Read and write text |
| `GetElementValue(h)` / `SetElementValue(h, float)` | Read and write a numeric value |
| `GetElementChecked(h)` / `SetElementChecked(h, bool)` | Read and write the checked state |
| `FocusElement(h)` | Set focus |
| `GetRenderModeValue()` / `SetRenderModeValue(uint)` | Render mode |
| `GetWorldRenderPathValue()` / `SetWorldRenderPathValue(uint)` | World-space render path |
| `GetSortOrderScript()` / `SetSortOrderScript(int)` | Sort order |
| `GetSortLayer()` / `SetSortLayer(int)` | Sort layer |
| `GetCameraId()` / `SetCameraId(uint64)` | Associated camera |
| `GetDpiMatch()` / `SetDpiMatch(float)` | DPI match |
| `GetPixelsPerUnit()` / `SetPixelsPerUnit(float)` | Pixel density |
| `GetDepthTest()` / `SetDepthTest(bool)` | Depth test |
| `GetDepthWrite()` / `SetDepthWrite(bool)` | Depth write |
| `GetAlwaysOnTop()` / `SetAlwaysOnTop(bool)` | Always-on-top display |

## Time

| Member | Notes |
| --- | --- |
| `deltaTime` | Affected by `timeScale` |
| `unscaledDeltaTime` | Not affected by `timeScale` |
| `time` / `unscaledTime` | Accumulated time |
| `frameCount` / `fixedFrameCount` | Frame counters |
| `timeScale` | Time scaling |
| `fixedDeltaTime` | Fixed step, default `0.02` |
| `maximumDeltaTime` | Maximum step per frame, default `0.1` |

Fixed updates run before regular updates.

## SceneManager and Scene

| Member | Notes |
| --- | --- |
| `SceneManager.GetActiveScene()` | Active scene |
| `SceneManager.LoadScene(string path)` | Request a scene load |
| `Scene.name` / `path` / `buildIndex` / `isLoaded` | Scene information |
| `Scene.IsValid()` | Whether the scene is loaded |

`LoadScene` is a synchronous single-active-scene request that switches after the current script frame ends.

## Input

| Member | Notes |
| --- | --- |
| `GetKey(KeyCode)` / `GetKeyDown` / `GetKeyUp` | Keyboard |
| `GetMouseButton(int)` / `GetMouseButtonDown` / `GetMouseButtonUp` | Mouse buttons |
| `mousePosition` | Mouse position |
| `mouseScrollDelta` | Scroll delta |

## Debug

| Member | Notes |
| --- | --- |
| `Log` / `LogWarning` / `LogError` | Write to the log |
| `LogException` | Record an exception |
| `Assert(bool, message)` | Report a failed assertion as an error |

Lua runtime errors enter script diagnostics and the editor Console. Lua scripts should use the language's own `error` to produce a locatable failure.

## Math types

`Vector2`, `Vector3`, `Vector4`, `Quaternion`, `Color`, `Rect`, `Vector2Int`, `Vector3Int`, `RectInt`, `Bounds`, `BoundsInt`, `Ray`, and `Mathf` provide lowercase fields, constants, operators, and common functions.

## Manifest and boundaries

- `Library/ScriptApi/ScriptApi.json` is the merged API manifest at schema version `2`.
- Per-module source manifests live at `*/Gen/ScriptApi.json`.
- Unsupported portable signatures are rejected at build time rather than silently skipped.

The manifest contains engine-side reflected types, including Animator/Particle System playback and parameter controls plus collider configuration. Cross-references between script-defined types, coroutines, asset loading, general physics queries, and audio APIs are not included yet.

## Next steps

- [UI Overview](../ui/overview.md) for the full `UICanvasComponent` semantics.
- [Reflection and Serialization](../architecture/reflection-serialization.md) explains how the manifest is generated.
