# Reflection Workflow

This is the maintained English guide for Nullus reflection and MetaParser usage.

For the Chinese version, see [ReflectionWorkflow.zh-CN.md](./ReflectionWorkflow.zh-CN.md).

## Overview

Nullus uses one maintained reflection path:

- declare reflected types in runtime and project headers
- let `Tools/MetaParser` parse the declarations during the normal CMake build
- generate glue into `Runtime/<Module>/Gen/` or `Project/<Target>/Gen/`
- register types and members through the generated module link functions at runtime

Do not hand-edit files under `Runtime/*/Gen/` or `Project/*/Gen/`.

## How the current system really works

### Input patterns

Nullus currently supports these reflection inputs:

1. Inline reflected types:

```cpp
CLASS(MyType) : public NLS::meta::Object
{
public:
    GENERATED_BODY()
};
```

2. Reflected enums:

```cpp
ENUM(MyMode) : uint8_t
{
    A,
    B
};
```

3. Auto properties from `FUNCTION()`-marked `GetXxx` / `SetXxx` pairs
4. Explicit properties through `PROPERTY(...)`
5. External reflection declarations through `MetaExternal(...)` plus `REFLECT_EXTERNAL(...)`
6. Explicit private external bindings through `REFLECT_PRIVATE_*`

### Parser behavior

The current repository behavior is:

- reflected type bodies that contain `GENERATED_BODY()` are parsed through the maintained text parser path
- top-level `ENUM()` declarations are parsed from text
- external reflection declarations are parsed from `MetaExternal(...)` / `REFLECT_EXTERNAL(...)`
- `CppAst` is still present, but it is not the primary path for the reflected type bodies used by the repository today

That means older notes claiming “CppAst is always the default path and text parsing is only fallback” are no longer accurate for the current maintained workflow.

### Runtime registration

Each runtime module emits a module-specific link function such as:

- `LinkReflectionTypes_NLS_Base`
- `LinkReflectionTypes_NLS_Core`
- `LinkReflectionTypes_NLS_Engine`

The runtime reflection database calls the generated module link macro during initialization, then completes type registration through the reflection module registry.

## When a type should be reflected

Use a consumer-driven rule set. A type should usually be reflected when at least one current repository consumer needs runtime metadata for it:

- editor inspection or editing
- serialization
- runtime `meta::Type` creation, lookup, or inheritance checks
- a reflected property whose value type must itself be reflected

Do not reflect a type only because it is public.

### Quick inclusion rules

Reflect a type when the answer to at least one of these is "yes":

| Question | Reflect? | Why |
| --- | --- | --- |
| Does the editor need to inspect or edit it? | Yes | Reflection provides the stable metadata surface |
| Does serialization need to walk its fields? | Yes | Reflected fields become the maintained serialization surface |
| Does runtime code create, query, or compare it through `meta::Type`? | Yes | The type needs stable runtime metadata |
| Is it a value type or enum used by another reflected property? | Yes | Reflected properties must use reflectable value types |
| Is it only a cache, helper, temporary state, or unsupported resource handle? | Usually no | This adds noise without helping a real consumer |

## What should be reflected

### Classes and structs

Reflect classes or structs when they expose stable state that current editor, serialization, or runtime metadata consumers actually use.

Good fits:

- gameplay components with editable state
- serializable data records
- stable value types used by reflected fields
- owned runtime types created or queried through `meta::Type`

Usually not a good fit:

- transient caches
- internal lifecycle state
- helper-only implementation types
- resource pointers or heavy handles that current consumers cannot safely edit or serialize

### Enums

Reflect enums when they:

- appear in reflected fields
- must show up in editor choice lists
- need stable serialized names or reflected lookup

### Properties

Reflect properties when they represent stable consumer-facing state.

Prefer these patterns:

1. Auto property inference when `FUNCTION()` marks a clean `GetXxx` / `SetXxx` pair
2. Explicit `PROPERTY(...)` when the exposed property name differs from accessor naming
3. External property declarations when the type should stay free of inline reflection macros

Do not expose internal cache values or fields whose read or write behavior is misleading for metadata consumers.

### Functions

Reflect functions when they are:

- property getters and setters
- intentionally exposed runtime operations
- metadata-driven lookup points
- maintained sample coverage for supported generator patterns

Do not expose lifecycle callbacks or purely internal helpers unless a real consumer needs metadata access to them.

## Supported patterns with examples

### Pattern 1: Inline reflected type

```cpp
CLASS(CameraComponent) : public Component
{
public:
    GENERATED_BODY()

    FUNCTION()
    void SetFov(float value);

    FUNCTION()
    float GetFov() const;
};
```

Because the names line up, `GetFov` + `SetFov` becomes the reflected property `fov`.

### Pattern 2: Explicit property name

Use this when the property name and accessor names do not match cleanly:

```cpp
PROPERTY(name = active, getter = IsSelfActive, setter = SetActive)
```

Current repository examples:

- `GameObject.active`
- `MeshFilter.mesh`

### Pattern 3: External reflection declaration

Use this when the target type should not be annotated directly:

```cpp
MetaExternal(NLS::Maths::Vector3)

REFLECT_EXTERNAL(
    NLS::Maths::Vector3,
    Fields(
        REFLECT_FIELD(float, x),
        REFLECT_FIELD(float, y),
        REFLECT_FIELD(float, z)
    ),
    Methods(
        REFLECT_METHOD_EX(Length, static_cast<float(NLS::Maths::Vector3::*)() const>(&NLS::Maths::Vector3::Length))
    ),
    StaticMethods(
        REFLECT_STATIC_METHOD(Dot, static_cast<float(*)(const NLS::Maths::Vector3&, const NLS::Maths::Vector3&)>(&NLS::Maths::Vector3::Dot))
    )
)
```

### Pattern 4: Private external binding

Use only when needed, and only deliberately:

```cpp
REFLECT_EXTERNAL(
    NLS::meta::PrivateReflectionExternalSample,
    Fields(
        REFLECT_PRIVATE_FIELD(int, m_hiddenValue)
    ),
    Methods(
        REFLECT_PRIVATE_METHOD(GetHiddenValue)
    )
)
```

This works through generated access helpers and should remain the exception, not the default.

## Supported pattern matrix

| Pattern | Use when | Typical macros | Current repository examples |
| --- | --- | --- | --- |
| Inline reflected type | The engine owns the type and consumers need stable runtime metadata | `CLASS` / `STRUCT` / `GENERATED_BODY` / `FUNCTION` | `TransformComponent`, `CameraComponent`, `GameObject`, `Scene` |
| Reflected enum | The enum appears in reflected fields or editor choices | `ENUM` | `EProjectionMode`, `ELightType`, `MeshRenderer::EFrustumBehaviour` |
| Auto property | Getter and setter names line up cleanly as `GetXxx` / `SetXxx` | `FUNCTION` on both accessors | `fov`, `near`, `far`, `lightType`, `materialPaths` |
| Explicit property | The exposed property name differs from accessor naming | `PROPERTY(...)` plus matching accessors | `GameObject.active`, `MeshFilter.mesh` |
| External reflection | The target type should stay free of inline reflection macros | `MetaExternal` + `REFLECT_EXTERNAL` | `NLS::Maths::Vector3`, `NLS::Maths::Quaternion` |
| Private external reflection | A private member must be exposed deliberately and exceptionally | `REFLECT_PRIVATE_*` | `PrivateReflectionExternalSample` |

## Current repository examples

Correct inline reflection coverage today includes:

- `TransformComponent`
- `CameraComponent`
- `LightComponent`
- `MeshFilter`
- `MeshRenderer`
- `GameObject`
- `Scene`
- `BoundingSphere`
- `EProjectionMode`
- `ELightType`
- `MeshRenderer::EFrustumBehaviour`

Correct external reflection usage today includes:

- `NLS::Maths::Vector3`
- `NLS::Maths::Quaternion`
- `PrivateReflectionExternalSample`

## Reflection Serialization Expectations

Object Graph serialization uses reflected object fields as the maintained ordinary-property surface. `ObjectGraphSerializer` walks `object.GetType().GetFields()` for value fields and converts each reflected value through the reflection JSON path before writing Object Graph properties.

Relationships that define graph structure are not ordinary reflected fields. Scene ownership of game objects and game object ownership of components or children are represented with `$owned`. Parent object references and asset references are represented with the current Object Graph object-reference shape: `fileID`, `guid`, `type`, and optional `filePath`. Prefab overrides are represented as patch operations in `Runtime/Engine/Serialize`.

Do not write legacy `$ref` or `$asset` objects. `ObjectGraphReader` rejects those shapes; local object references use an empty `guid` with a non-zero `fileID`, while asset references require a valid `guid`, non-zero `fileID`, and asset `type`.

Types intended for scene or prefab persistence should therefore expose stable serializable state through reflected fields, mark transient/editor-only/runtime-only intent with runtime metadata where appropriate, and avoid raw pointer persistence unless the field is explicitly modeled as owned, object reference, asset reference, or transient. The legacy `SerializedSceneData`, `SerializedActorData`, `SerializedComponentData`, and `GameobjectSerialize` adapter path are no longer the maintained scene persistence surface.

## Validation steps

Use the normal build flow so MetaParser runs exactly the same way it does in real development.

### Windows

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug --target NullusUnitTests ReflectionTest -- /m:1
ctest --test-dir build -C Debug --output-on-failure -R NullusUnitTests
.\build\bin\Debug\ReflectionTest.exe
```

The MetaParser build log now reports the parser route for each reflected header, for example:

```text
[MetaParser] Parsing ... [routes: text-type-body, external-declaration]
```

For text-parsed reflected classes and structs, the build log also reports a member-discovery summary, for example:

```text
[MetaParser] Members NLS::Engine::Components::CameraComponent [fields: inline=0, explicit=0, auto=8, rejected=0, total=8] [methods: inline=17, explicit=0, rejected=0, overload-rejected=0, total=17]
```

Read that summary as:

- `inline` means directly marked reflected members discovered from the class body
- `explicit` means standalone `PROPERTY(...)` or `FUNCTION(...)` directives
- `auto` means inferred properties built from `Get*` / `Set*`, `Has*`, or `Is*` method pairs
- `rejected` or `overload-rejected` means the parser saw a reflected candidate but intentionally skipped registering it

### Linux / macOS

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

## Known constraints

- `Runtime/*/Gen/` and `Project/*/Gen/` are generated output only
- support claims must be tied to exact tested evidence
- Windows-first reflection evidence does not prove Linux or macOS generation behavior
- if a reflected property uses an unsupported type, fix the reflected type support instead of adapting it implicitly in generated code
- `std::vector<T>` and `NLS::Array<T>` reflected value fields are supported; MetaParser also normalizes local shorthand spellings such as `vector<T>` and `Array<T>` after CppAst resolves them, but fully qualified spellings remain the preferred source style.

## Related docs

- Chinese guide: [ReflectionWorkflow.zh-CN.md](./ReflectionWorkflow.zh-CN.md)
- Test overview: [../Testing.md](../Testing.md)
