# Reflection and Serialization

Reflection drives three things at once: editor interfaces, scene and Prefab serialization, and the scripting API manifest. All three share one metadata source, which is why "fields that can be serialized" and "fields that can be edited" match by default.

## Reflection pipeline

```text
C++ declarations (CLASS / STRUCT / ENUM / PROPERTY / FUNCTION)
    |
    v
Tools/MetaParser parses
    |
    v
Runtime/<Module>/Gen/ and Project/<Target>/Gen/
    |
    v
runtime registration into the reflection database
    |
    +--> Inspector controls
    +--> object graph serialization
    +--> scripting API manifest
```

Never edit anything under a `Gen/` directory by hand.

## How to declare reflected types

### Inline types

```cpp
CLASS(MyType) : public NLS::meta::Object
{
public:
    GENERATED_BODY()

    PROPERTY()
    float speed = 1.0f;
};
```

### Enums

```cpp
ENUM(MyMode) : uint8_t
{
    A,
    B
};
```

### Accessor properties

`GetXxx` / `SetXxx` pairs marked with `FUNCTION()` form a property automatically. A getter alone gives a read-only property; a setter alone gives a write-only one.

### External declarations

When the original declaration body cannot be changed:

```cpp
MetaExternal(MyType)
REFLECT_EXTERNAL(MyType, "MyType")
```

Private members use the `REFLECT_PRIVATE_*` family for out-of-class binding.

## Parsing paths

- A type body containing `GENERATED_BODY()` goes through text parsing.
- Top-level `ENUM()` declarations also go through text extraction.
- `MetaExternal` / `REFLECT_EXTERNAL` go through out-of-class declaration parsing.
- `CppAst` still exists in the toolchain but is not the parsing path for most reflected type bodies today.

## Admission rules

Stay consumer driven: reflect a type only when a real consumer exists.

| Question | Conclusion |
| --- | --- |
| Does the editor need to view or edit it? | Reflect |
| Does serialization need to walk its fields? | Reflect |
| Does runtime code create, query, or compare it through `meta::Type`? | Reflect |
| Is it a value type or enum another reflected property depends on? | Reflect |
| Is it just a cache, helper implementation, transient state, or a handle no consumer can handle? | Do not reflect |

Being `public` is not a reason to reflect a type.

## Runtime registration

Each runtime module generates its own link function, for example `LinkReflectionTypes_NLS_Base`, `LinkReflectionTypes_NLS_Core`, and `LinkReflectionTypes_NLS_Engine`. Database initialization calls them to complete registration.

## Object graph serialization

Scenes and Prefabs share one object graph format:

| Element | Representation |
| --- | --- |
| Object | GUID-backed `ObjectRecord` |
| Ordinary state | Reflected fields |
| Lifecycle ownership | `$owned` relationship |
| In-scene reference | Non-zero `fileID`, empty `guid`, `type = NonAssetType` |
| Asset reference | Non-zero `fileID`, valid asset `guid`, asset type; `filePath` is only a hint |
| Prefab difference | Patch operations instead of per-object-type payloads |

The legacy `$ref` and `$asset` forms are no longer accepted by the reader. Use `$owned` for ownership and the `fileID`/`guid`/`type` shape for references.

## Read and write flow

`ObjectGraphSerializer` writes reflected fields and records graph relationships separately. `ObjectGraphInstantiator` instantiates in stages:

1. Create known objects.
2. Apply reflected fields.
3. Resolve relationships.
4. Rebuild scene caches.

## Load policies

- **Editor policy**: preserve unknown records and unresolved asset references with diagnostics so a document round-trips without losing recoverable data.
- **Runtime policy**: fail on unknown or invalid data instead of silently dropping it.

The difference is deliberate: the editor needs tolerance, the runtime needs determinism.

## Deprecated formats

The `GameobjectSerialize` adapter and the `SerializedSceneData` / `SerializedActorData` / `SerializedComponentData` payload model are not part of the maintained runtime scene load path.

## Serialization extension points

Components whose state cannot be expressed with reflected fields can override:

```cpp
virtual bool SerializeObjectGraphProperties(
    std::vector<NLS::Engine::Serialize::PropertyRecord>& properties) const;

virtual bool DeserializeObjectGraphProperties(
    const std::vector<NLS::Engine::Serialize::PropertyRecord>& properties);
```

The engine owns the graph format and the component owns its payload, keeping the dependency one-way. Script components use exactly this path to store script asset metadata and field overrides.

## Next steps

- [Prefab Workflow](../prefab/workflow.md) shows how patch operations express overrides.
- [Scripting API Reference](../scripting/api.md) shows how the manifest becomes bindings in two languages.
