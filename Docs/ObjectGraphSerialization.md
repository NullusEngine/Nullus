# Object Graph Serialization

Nullus scene and prefab persistence is centered on `Runtime/Engine/Serialize`.

The document model stores objects as GUID-backed `ObjectRecord` entries. Ordinary object state is written through reflection fields. Graph structure is explicit:

- `$owned` expresses lifecycle ownership, such as Scene to GameObject or GameObject to Component.
- Non-owning object references use Unity-style object identifiers with `fileID`, `guid`, `type`, and optional `filePath` fields. Local object references use a non-zero `fileID`, an empty `guid`, and `type` set to `NonAssetType`.
- External asset references use the same object identifier shape, but require a non-zero `fileID`, a valid asset `guid`, and an asset `type`; `filePath` is only a hint.
- Prefab changes are represented as patch operations, not as ad hoc object-specific payloads.

Legacy `$ref` and `$asset` objects are not accepted by `ObjectGraphReader`. Use `$owned` only for lifecycle ownership and the `fileID`/`guid`/`type` shape for object or asset references.

`ObjectGraphSerializer` writes reflected fields for `NLS::meta::Object` instances and adds graph relationships separately. `ObjectGraphInstantiator` creates known objects in stages, applies reflected fields, then resolves relationships and rebuilds scene caches.

Editor-preserving load policies may keep unknown records and unresolved asset references with diagnostics so a document can round-trip without losing recoverable data. Runtime policies should fail on unknown or invalid data instead of silently dropping it.

The old `GameobjectSerialize` adapter and `SerializedSceneData` / `SerializedActorData` / `SerializedComponentData` payload model are intentionally not part of the maintained runtime scene load path.
