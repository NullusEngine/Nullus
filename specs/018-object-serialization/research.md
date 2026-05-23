# Research: Object Graph Serialization

## Decision: Use GUID as the shared persistent identity primitive

**Rationale**: Object identity, asset identity, prefab source identity, and test fixtures need stable values that survive save/load, renames, and editor sessions. A shared low-level GUID type avoids each subsystem inventing string IDs.

**Alternatives considered**:

- Incrementing integer IDs: rejected because they are local, fragile across prefab instancing, and currently tied to `worldID`.
- Human-readable path IDs: rejected because object names and hierarchy paths are editable and unsuitable as stable identity.
- Raw strings everywhere: rejected because object IDs and asset IDs become easy to mix up.

## Decision: Use strong wrappers for ObjectId and AssetId

**Rationale**: `ObjectId` and `AssetId` have different semantics even if both contain GUIDs. Strong wrappers prevent accidental interchange in code and in Object Graph APIs.

**Alternatives considered**:

- Bare `Guid`: rejected because it relies on naming discipline.
- Type-prefixed strings: rejected because prefixes are weaker than type checking and still require parsing.

## Decision: Make Scene a reflected object graph root

**Rationale**: The user explicitly selected `Scene` inheriting the reflection object model. This avoids a special case where scenes are containers while game objects and components are objects. It also lets scene save/load use the same Object Graph path as prefabs and object assets.

**Alternatives considered**:

- Keep Scene as a special non-object container: rejected because it creates one-off serialization rules.
- Serialize only GameObjects under an implicit scene root: rejected because scene-level settings and future subscene/cooked metadata need a first-class root object.

## Decision: Remove worldID from persistence and runtime object identity

**Rationale**: Persistent identity must be GUID-backed ObjectId. Rendering/editor picking can use temporary registries. Keeping `worldID` would leave two identity systems and encourage serialization paths to depend on transient runtime IDs.

**Alternatives considered**:

- Keep `worldID` as a runtime-only field: rejected for the initial feature because existing code already exposes it through reflection and serialization-sensitive APIs.
- Rename `worldID` to `ObjectId`: rejected because integer and GUID semantics differ.

## Decision: Do not keep GameobjectSerialize.cpp as a legacy adapter

**Rationale**: The user explicitly rejected retaining `GameobjectSerialize.cpp` as a legacy adapter. Removing it from the new runtime path prevents the old actor/component payload shape from constraining Object Graph scenes and prefabs.

**Alternatives considered**:

- Legacy read adapter in runtime: rejected by user direction and because it increases test and maintenance burden.
- Automatic migration on load: rejected because the new design should not silently accept old files. A future offline converter can be scoped separately.

## Decision: Represent ownership with `$owned`, object references with `$ref`, and asset references with `$asset`

**Rationale**: Ownership and reference relationships are not interchangeable. Parent/child hierarchy does not necessarily describe memory ownership, components have ownership without hierarchy, and asset references must not inline resource data.

**Alternatives considered**:

- `owner` field on every object record: rejected as more redundant and harder to keep in sync than explicit owned-reference properties.
- Infer ownership from property names: rejected because validation and migration need explicit semantics.
- Treat all references as `$ref`: rejected because lifecycle, deletion, and prefab override behavior would be ambiguous.

## Decision: Load in staged phases

**Rationale**: Object references, cycles, owner pointers, components, asset references, and prefab overrides cannot be resolved safely while objects are still partially created. Staged loading makes diagnostics deterministic and prevents callbacks from observing half-built graphs.

**Alternatives considered**:

- Immediate recursive creation/deserialization: rejected because it handles cycles poorly and mixes ownership, property, and reference resolution.
- Per-type custom load order: rejected because it does not scale to plugins and reflection-driven objects.

## Decision: Preserve unknown records in editor-oriented load policies

**Rationale**: Long-lived projects can contain components from plugins, old branches, or temporarily missing modules. Opening and saving a scene in the editor must not destroy recoverable unknown data by default.

**Alternatives considered**:

- Fail every unknown type: appropriate for strict runtime load policy, but too destructive for editor workflows.
- Drop unknown records: rejected because it destroys user data.

## Decision: Keep text archive deterministic

**Rationale**: Scenes and prefabs are collaborative source-controlled assets. Stable ordering and formatting are required for reviewable diffs, golden-file tests, and reliable prefab patch generation.

**Alternatives considered**:

- Preserve insertion order from runtime vectors only: rejected because runtime mutation history should not affect unrelated save output.
- Optimize first with binary format: rejected because editor authoring and reviewability matter first. Cooked binary can be generated later.
