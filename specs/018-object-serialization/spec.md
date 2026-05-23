# Feature Specification: Object Graph Serialization

**Feature Branch**: `018-object-serialization`  
**Created**: 2026-05-08  
**Status**: Draft  
**Input**: User description: "Design and implement a modern Object Graph serialization system for Nullus with a shared GUID system, Scene inheriting meta Object, no worldID, no GameObject constructor actor id or playing reference, scene save/load, prefab assets, owned/ref/asset references, deterministic text archives, structured diagnostics, and no legacy GameobjectSerialize adapter."

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Save And Load Scenes As Object Graphs (Priority: P1)

An engine developer can save a `Scene` as a deterministic Object Graph document and load it back into a runtime `Scene` without using `worldID`, constructor-provided actor IDs, or scene-specific legacy payload records.

**Why this priority**: Scene persistence is the foundation for every later prefab, asset, editor, and runtime loading workflow.

**Independent Test**: Create a scene with a hierarchy and components, save it, load it, and verify that object identities, ownership, component data, parent references, and scene caches are restored.

**Acceptance Scenarios**:

1. **Given** a scene containing multiple game objects and components, **When** the scene is saved and loaded, **Then** the loaded scene contains equivalent objects, component state, ownership, and parent relationships using persistent GUID-backed object IDs.
2. **Given** the same scene state saved twice, **When** the resulting documents are compared, **Then** the serialized output is byte-for-byte stable except for intentional content changes.
3. **Given** an invalid object reference, duplicate object ID, unknown type, or malformed GUID in a scene document, **When** the document is read or instantiated, **Then** the system returns structured diagnostics without crashing or silently dropping data.

---

### User Story 2 - Author And Instantiate Prefabs (Priority: P2)

An editor user can save a prefab as an Object Graph asset, instantiate it into a scene with new object IDs, and preserve explicit overrides independently of the source prefab.

**Why this priority**: Prefabs are the main reuse mechanism for scene content and must share the same identity and reference model as scenes.

**Independent Test**: Save a prefab with child objects and components, instantiate it into a scene, edit one property override, save and load the scene, and verify the instance still maps to the prefab source while preserving its override.

**Acceptance Scenarios**:

1. **Given** a prefab asset with a root game object and components, **When** it is instantiated into a scene, **Then** each runtime object receives a new object ID and keeps a mapping to its prefab source object ID.
2. **Given** a prefab instance with property, component, child, or order overrides, **When** the scene is saved and loaded, **Then** the overrides are preserved as explicit patch operations.
3. **Given** a prefab variant based on another prefab, **When** it is loaded, **Then** the base prefab graph is composed before the variant overrides are applied.

---

### User Story 3 - Preserve Asset References And Unknown Data (Priority: P3)

An editor user can open, inspect, validate, and re-save Object Graph documents that contain asset references, missing assets, or unresolved types without destroying recoverable data.

**Why this priority**: Long-lived game assets must survive asset moves, missing imports, plugin/component changes, and partial editor environments.

**Independent Test**: Load a document containing valid asset references, missing asset references, and an unknown component record, then save it again and verify the unresolved records and references are preserved with diagnostics.

**Acceptance Scenarios**:

1. **Given** an object property references an external asset, **When** the document is saved, **Then** the file stores a GUID-backed asset reference rather than inline resource data or a path as the stable identity.
2. **Given** a missing asset reference is encountered in the editor, **When** the document is loaded and saved again, **Then** the original asset GUID is preserved and a diagnostic is available to the editor.
3. **Given** an unknown serializable type is encountered in the editor, **When** the document is round-tripped, **Then** the unknown record is preserved unless the active load policy explicitly rejects it.

### Edge Cases

- Duplicate object IDs in one document must be rejected with diagnostics before instantiation.
- Empty GUID values must not be accepted as persistent object or asset IDs.
- Ownership cycles must be rejected; normal object reference cycles may be allowed and resolved after all objects are created.
- Owned objects that are not reachable from the document root must be reported as orphaned records.
- References to deleted prefab source objects must produce dangling override diagnostics.
- Component ordering must survive save/load and prefab override application.
- Editor-only and transient state must not enter runtime scene or prefab documents.
- Legacy `GameobjectSerialize.cpp` scene payloads are not supported by the new runtime load path.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The system MUST provide a shared GUID value type with canonical string parsing, formatting, validation, hashing, deterministic test generation, and random production generation.
- **FR-002**: The system MUST introduce GUID-backed strong identity types for persistent object IDs and asset IDs so they cannot be accidentally interchanged in code or serialized documents.
- **FR-003**: `Scene` MUST participate as a serializable object graph root and inherit from the runtime reflection object model.
- **FR-004**: `GameObject` construction MUST NOT require an actor ID or a playing-state reference.
- **FR-005**: The system MUST remove persistent `worldID` semantics from game object serialization and runtime scene identity.
- **FR-006**: Editor picking and other temporary runtime/editor IDs MUST use transient registries that do not participate in scene or prefab persistence.
- **FR-007**: Scene save/load MUST use one Object Graph document model instead of `SerializedSceneData`, `SerializedActorData`, `SerializedComponentData`, or a legacy `GameobjectSerialize.cpp` adapter.
- **FR-008**: Object Graph documents MUST distinguish owned references, ordinary object references, and asset references.
- **FR-009**: Object Graph loading MUST be staged so all objects are created and assigned IDs before ownership, properties, object references, asset references, prefab overrides, and post-load callbacks are resolved.
- **FR-010**: The system MUST provide structured diagnostics for invalid formats, versions, GUIDs, type names, property values, object references, asset references, ownership cycles, and prefab overrides.
- **FR-011**: Text Object Graph output MUST be deterministic for version control use.
- **FR-012**: Prefab assets MUST use the same Object Graph model as scenes and support source-to-instance object ID mapping.
- **FR-013**: Prefab instances MUST represent overrides as explicit patch operations for property replacement, owned object insertion/removal/reorder, component insertion/removal/reorder, and nested prefab composition.
- **FR-014**: Asset references MUST preserve the original asset ID when the target asset is missing in the editor.
- **FR-015**: Unknown object records and unresolved properties MUST be preservable by editor load policies so opening and saving a document does not destroy recoverable data.
- **FR-016**: The system MUST define serialization metadata for persistent fields, transient fields, owned references, object references, asset references, editor-only data, runtime-only data, former property names, and type rename aliases.
- **FR-017**: The system MUST prohibit default persistence of raw pointers unless a property explicitly declares owned, object-reference, asset-reference, or transient behavior.
- **FR-018**: Scene loading MUST rebuild transient scene caches and editor/runtime lookup structures after object graph instantiation.
- **FR-019**: Saving a scene or prefab MUST use an atomic write flow so a failed save does not corrupt the previous file.
- **FR-020**: Runtime cooked/binary formats, when introduced, MUST be generated from the Object Graph document model and must not define a separate persistence semantic model.

### Key Entities *(include if feature involves data)*

- **Guid**: A 128-bit globally unique value with canonical string representation and deterministic test generation support.
- **ObjectId**: A strong GUID wrapper identifying one object record inside an Object Graph document.
- **AssetId**: A strong GUID wrapper identifying one external asset across moves and renames.
- **ObjectGraphDocument**: A pure data representation of a scene, prefab, or asset graph before runtime object instantiation.
- **ObjectRecord**: A document node containing one object ID, stable type identity, debug path/name, state, and typed properties.
- **PropertyRecord**: A named property with a typed value such as primitive value, GUID, owned reference, object reference, asset reference, array, or object value.
- **OwnedReference**: A reference that establishes lifecycle and serialization ownership.
- **ObjectReference**: A normal reference to another object record without ownership.
- **AssetReference**: A reference to an external asset by asset ID.
- **PatchOperation**: A typed prefab or edit operation such as replace property, insert owned object, remove owned object, or reorder owned object.
- **SerializationDiagnostic**: A structured validation or load/save message with code, severity, target object/property, and human-readable text.
- **LoadPolicy**: A mode that controls behavior for unknown types, missing assets, invalid records, and preservation of unresolved data.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: A scene containing at least 10 game objects, parent-child relationships, and reflected component fields can be saved, loaded, and verified for semantic equivalence by automated tests.
- **SC-002**: Saving the same scene twice without changes produces identical text output in automated deterministic-save tests.
- **SC-003**: At least 12 diagnostic cases covering invalid GUIDs, duplicate IDs, unknown types, missing references, missing assets, and invalid prefab overrides are covered by automated tests.
- **SC-004**: A prefab with a root, child object, component list, and at least one property override can be instantiated, saved in a scene, loaded, and verified by automated tests.
- **SC-005**: Legacy scene data structures and `worldID` fields are absent from the new scene/prefab output fixtures.
- **SC-006**: Unknown editor-preserved object records can be loaded and saved again without losing their original object ID, type identity, or raw property values.

## Assumptions

- Nullus intentionally breaks compatibility with the current `GameobjectSerialize.cpp` scene file shape for this feature.
- Any future migration from old scene files will be handled by a separate offline converter, not by the new runtime scene load path.
- JSON is the first text archive target; YAML or binary/cooked formats may be added later over the same Object Graph document model.
- The initial asset GUID source may be a `.meta` file or equivalent manifest, but stable asset identity must be GUID-based rather than path-based.
- Reflection registration continues to be generated through the existing MetaParser pipeline; generated files under `Runtime/*/Gen/` are not hand-edited.
- Tests are required for this feature because it changes runtime identity, reflection serialization, scene loading, and prefab behavior.
