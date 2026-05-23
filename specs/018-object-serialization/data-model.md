# Data Model: Object Graph Serialization

## Guid

Represents a 128-bit persistent identity value.

Fields:

- `bytes`: 16-byte value.

Validation:

- Empty/all-zero GUID is allowed only as an in-memory "unassigned" value.
- Persistent object and asset IDs must be non-empty.
- Text form uses lowercase canonical UUID format.
- Parsing accepts uppercase input but normalizes output.

Relationships:

- Wrapped by `ObjectId` and `AssetId`.

## ObjectId

Strong identity for an object record inside an Object Graph document.

Fields:

- `value`: `Guid`.

Validation:

- Must be unique within one document.
- Must not be empty when written to a document.
- Must not be used as an asset identity.

Relationships:

- Used by `ObjectRecord.id`.
- Used by `OwnedReference`, `ObjectReference`, and prefab source/instance mappings.

## AssetId

Strong identity for an external asset.

Fields:

- `value`: `Guid`.

Validation:

- Must not be empty when written to a document.
- Remains stable across asset path changes.

Relationships:

- Used by `AssetReference`.
- May be stored in asset `.meta` files or an equivalent asset manifest.

## ObjectGraphDocument

Pure data representation of a scene, prefab, or object asset.

Fields:

- `format`: stable document format name, such as `Nullus.ObjectGraph.Scene`.
- `version`: schema version.
- `documentId`: GUID identifying this document instance.
- `root`: root `ObjectId`.
- `objects`: list of `ObjectRecord`.
- `overrides`: list of `PatchOperation` for prefab documents or prefab instances.
- `dependencies`: optional asset references required by the document.
- `diagnostics`: validation and load/save messages.

Validation:

- Format must match the schema selected for the operation.
- Version must be supported or migratable.
- Root object must exist.
- Object IDs must be unique.
- Owned graph must be reachable from root unless explicitly preserved as unknown data.

## ObjectRecord

One persisted object node.

Fields:

- `id`: persistent object ID.
- `type`: stable serializable type identity.
- `debugName`: optional human-readable name for diagnostics and diffs.
- `debugPath`: optional human-readable object path for diagnostics only.
- `state`: `Alive` or `Removed` for composed prefab graphs.
- `properties`: ordered collection of `PropertyRecord`.
- `rawProperties`: preserved data when type or property schema is unknown.

Validation:

- Type must resolve for runtime instantiation unless load policy preserves unknown types.
- Object state must be valid for the document mode.
- Properties must match reflected serializable metadata when type is known.

Relationships:

- May be owned by another object through an `OwnedReference` property.
- May reference other object records through `ObjectReference`.
- May reference assets through `AssetReference`.

## PropertyRecord

One named serializable property.

Fields:

- `name`: stable property name.
- `value`: `PropertyValue`.
- `metadata`: optional serialized metadata, such as former names or editor/runtime flags.

Validation:

- Name must be stable and migrate through former-name metadata when renamed.
- Value type must match the reflected property schema unless preserved as raw unknown data.

## PropertyValue

Typed property payload.

Variants:

- `Null`
- `Bool`
- `Integer`
- `Number`
- `String`
- `Guid`
- `OwnedReference`
- `ObjectReference`
- `AssetReference`
- `Array`
- `Object`

Validation:

- Raw pointer-like fields cannot serialize unless represented as owned, object reference, asset reference, or transient metadata.
- Arrays containing owned references must preserve ordering.

## OwnedReference

Lifecycle and serialization ownership edge.

Fields:

- `target`: `ObjectId`.

Validation:

- Target object must exist or be preserved as unresolved unknown data.
- Ownership cycles are invalid.
- A known object may have only one owning path within one composed document.

Examples:

- Scene owns GameObjects.
- GameObject owns Components.

## ObjectReference

Normal object reference without lifecycle ownership.

Fields:

- `target`: `ObjectId`.

Validation:

- Target must exist after graph composition unless policy allows a broken reference.
- Reference cycles are allowed.

Examples:

- GameObject parent relationship.
- Component target relationship.

## AssetReference

External asset reference.

Fields:

- `asset`: `AssetId`.
- `expectedType`: optional stable type identity.
- `pathHint`: optional non-authoritative editor path.

Validation:

- Asset ID is the stable identity.
- Missing assets produce diagnostics and may be preserved depending on load policy.
- Path hints must not become the authoritative identity.

## PatchOperation

Typed prefab or edit operation.

Variants:

- `ReplaceProperty`
- `InsertOwned`
- `RemoveOwned`
- `MoveOwned`
- `AddPrefabInstance`
- `RemoveObject`

Common fields:

- `target`: object affected by the operation.
- `property`: property name when the operation affects a property or ordered owned list.
- `value`: new value or object reference for insertion/replacement.
- `index`: optional list position.

Validation:

- Target and property must exist in the composed source graph unless the operation is allowed to preserve unresolved data.
- Operations must be deterministic and order-sensitive.
- Removing an owned object must also resolve or diagnose references to that object.

## SerializationDiagnostic

Structured validation, read, write, or instantiation message.

Fields:

- `code`: stable diagnostic code.
- `severity`: info, warning, error.
- `objectId`: optional target object.
- `property`: optional target property.
- `message`: human-readable message.

Validation:

- Errors block strict runtime instantiation.
- Warnings may be acceptable in editor-preserving policies.

## LoadPolicy

Controls read and instantiate behavior.

Fields:

- `unknownTypePolicy`: preserve, drop, or fail.
- `missingAssetPolicy`: keep broken reference, use fallback, or fail.
- `invalidReferencePolicy`: preserve, clear, or fail.
- `targetMode`: editor, runtime, or cooked conversion.

Validation:

- Runtime mode must not silently drop invalid data.
- Editor mode should preserve recoverable unknown records by default.

## State Transitions

Scene load:

1. Text document parsed.
2. Document migrated to current version.
3. Document validated.
4. Runtime objects created.
5. Object IDs assigned and registered.
6. Ownership attached.
7. Plain properties applied.
8. Object and asset references resolved.
9. Prefab overrides applied.
10. Post-load callbacks and scene caches updated.

Prefab instantiate:

1. Source prefab document loaded.
2. Base/variant prefab graph composed.
3. Instance object IDs generated.
4. Source-to-instance map recorded.
5. Overrides applied.
6. Instance inserted into target scene through owned references.
