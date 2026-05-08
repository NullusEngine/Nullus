# Object Graph Serialization

Nullus scene and prefab persistence is centered on `Runtime/Engine/Serialize`.

The document model stores objects as GUID-backed `ObjectRecord` entries. Ordinary object state is written through reflection fields. Graph structure is explicit:

- `$owned` expresses lifecycle ownership, such as Scene to GameObject or GameObject to Component.
- `$ref` expresses non-owning object references, such as GameObject parent links.
- `$asset` expresses external asset references through stable `AssetId` values plus optional type and path hints.
- Prefab changes are represented as patch operations, not as ad hoc object-specific payloads.

`ObjectGraphSerializer` writes reflected fields for `NLS::meta::Object` instances and adds graph relationships separately. `ObjectGraphInstantiator` creates known objects in stages, applies reflected fields, then resolves relationships and rebuilds scene caches.

Editor-preserving load policies may keep unknown records and unresolved asset references with diagnostics so a document can round-trip without losing recoverable data. Runtime policies should fail on unknown or invalid data instead of silently dropping it.

The old `GameobjectSerialize` adapter and `SerializedSceneData` / `SerializedActorData` / `SerializedComponentData` payload model are intentionally not part of the maintained runtime scene load path.
