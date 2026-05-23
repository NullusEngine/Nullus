# Contract: Serialization Services

## Purpose

Defines service responsibilities and expected outcomes for reading, validating, writing, instantiating, and composing Object Graph documents.

## Guid Service

Capabilities:

- Create random production GUIDs.
- Create deterministic test GUIDs from stable labels.
- Parse canonical and uppercase UUID strings.
- Format GUIDs as lowercase canonical strings.
- Hash and compare GUIDs.

Failure cases:

- Invalid text returns a structured parse failure.
- Empty GUID is rejected when used as persistent object or asset identity.

## Document Reader

Input:

- File path or text buffer.
- Load policy.

Output:

- `ObjectGraphDocument`.
- Diagnostics.

Responsibilities:

- Parse text without instantiating runtime objects.
- Validate common header fields.
- Preserve unknown records when the policy allows.
- Report unsupported format/version before object creation.

## Document Writer

Input:

- `ObjectGraphDocument`.
- Destination path or buffer.
- Deterministic formatting options.

Output:

- Write result.
- Diagnostics.

Responsibilities:

- Produce deterministic output.
- Atomically write files when saving to disk.
- Preserve unresolved editor data carried by the document.
- Never emit persistent `worldID`.

## Document Validator

Input:

- `ObjectGraphDocument`.
- Schema selection.
- Load policy.

Output:

- Validation result.
- Diagnostics.

Responsibilities:

- Detect duplicate IDs, invalid GUIDs, missing roots, unknown types, invalid references, ownership cycles, orphaned owned records, malformed prefab overrides, and asset reference issues.
- Distinguish blocking errors from editor-preservable warnings.

## Graph Builder

Input:

- Runtime `Scene`, `GameObject`, `Component`, or prefab root.
- Serialization context.

Output:

- `ObjectGraphDocument`.
- Diagnostics.

Responsibilities:

- Traverse reflected serializable object state.
- Assign missing ObjectIds through the GUID generator.
- Emit `$owned`, `$ref`, and `$asset` relationships according to serialization metadata.
- Skip transient/editor-only/runtime-only fields based on target mode.

## Graph Instantiator

Input:

- Validated `ObjectGraphDocument`.
- Runtime instantiation context.
- Load policy.

Output:

- Runtime scene, prefab root, or object asset.
- Diagnostics.

Responsibilities:

- Create all known objects before resolving references.
- Assign ObjectIds.
- Establish ownership.
- Apply plain properties.
- Resolve object references and asset references.
- Apply prefab overrides.
- Call deserialization and post-load hooks in the defined order.
- Rebuild scene caches and transient lookup registries.

## Prefab Composer

Input:

- Prefab document.
- Optional base prefab document.
- Instance insertion context.

Output:

- Composed Object Graph.
- Source-to-instance ObjectId map.
- Diagnostics.

Responsibilities:

- Compose base prefab and variant overrides.
- Generate new ObjectIds for instances.
- Preserve source ObjectIds for override mapping.
- Apply property, insertion, removal, and reorder patches.
- Report dangling or invalid override targets.

## Pick Registry

Input:

- Runtime/editor objects participating in a picking pass.

Output:

- Transient pick IDs.
- Object resolution for readback.

Responsibilities:

- Replace `worldID` for picking and editor-only transient ID needs.
- Never write pick IDs to scene, prefab, or asset documents.
- Clear or rebuild IDs according to frame/editor lifetime.

## Serialization Hooks

Supported lifecycle:

- `OnBeforeSerialize`: synchronize derived serializable state before graph build.
- `OnAfterDeserialize`: called after raw properties are applied.
- `PostLoad`: called after ownership, references, assets, prefab overrides, and scene caches are complete.

Rules:

- Hooks must not rely on generated files being hand-edited.
- `PostLoad` is the first hook where all references can be assumed resolved under a successful strict load.
