# Implementation Plan: Unity Inspector Alignment

**Branch**: `unity-inspector-alignment` | **Date**: 2026-05-20 | **Spec**: `specs/unity-inspector-alignment/spec.md`  
**Input**: Feature specification from `specs/unity-inspector-alignment/spec.md`

## Summary

Align the Nullus editor Inspector with Unity 2018.4 common `PropertyField` behavior by expanding the shared reflected property drawer, adding a first-slice ObjectReference row backed by Unity-style `PPtr<T>`/`InstanceID`/PersistentManager identity data for supported resource references, supporting basic arrays, adding Range/LayerMask/Tag/Layer controls, and tightening Inspector row behavior while preserving current Editor/Game runtime viability.

## Technical Context

**Language/Version**: C++20  
**Primary Dependencies**: Nullus reflection runtime, MetaParser-generated registration, ImGui, existing `Runtime/UI` widgets, `UI::GUIDrawer`, existing editor asset database/drag-drop services, existing scene/GameObject/Component APIs  
**Storage**: Existing ObjectGraph storage updated so persistent component references are typed `PPtr<T>` fields in memory and serialize through PersistentManager to Unity-like `fileID`/`guid`/`type` identity data; raw C++ pointers are resolver outputs only. Runtime object identity lives in a lower module (`NLS_Core`) so rendering resources can derive from the Unity-style Object base without depending on `NLS_Engine`.
**Testing**: `NullusUnitTests`, especially `Tests/Unit/ReflectedPropertyDrawerTests.cpp`, plus focused editor runtime/manual verification for visual layout  
**Target Platform**: Desktop editor; first verified platform Windows  
**Project Type**: Native engine/editor UI feature across `Project/Editor`, `Runtime/UI`, `Runtime/Base/Reflection`, `Runtime/Engine`, and `Tests`  
**Performance Goals**: Inspector property drawing remains per-frame predictable for typical selected GameObjects; type classification and row creation remain O(number of reflected fields)  
**Constraints**: Do not hand-edit generated files under `Runtime/*/Gen/`; preserve current MetaParser flow; keep Editor and Game runnable; avoid rewiring rendering or platform code  
**Scale/Scope**: First phase covers core Unity Inspector field controls, first-slice resource ObjectReference behavior, supported-element arrays, Range metadata, Tag/Layer selectors, and LayerMask editing. Full Unity style parity, scene GameObject/Component assignment, and advanced Unity custom editors are deferred.

## Constitution Check

*GATE: Must pass before implementation. Re-check after design.*

- **Spec-first major change**: Pass. This bundle contains `spec.md`, `plan.md`, and `tasks.md` before code implementation.
- **Validation matches subsystem**: Pass. Reflection/Inspector behavior uses focused unit tests; visual style uses targeted editor runtime/manual evidence.
- **Generated code boundaries**: Pass. Generated files remain outputs only; any reflection metadata changes must flow through the normal MetaParser/build path.
- **Incremental verified delivery**: Pass. The work is split into type coverage, ObjectReference, arrays, styling, and validation slices.
- **Product runtime preservation**: Pass. Existing Inspector behavior and GUIDrawer helpers remain available during migration.

## Project Structure

### Documentation (this feature)

```text
specs/unity-inspector-alignment/
├── spec.md
├── plan.md
└── tasks.md
```

### Source Code (repository root)

```text
Project/Editor/Panels/
├── Inspector.cpp
├── Inspector.h
├── ReflectedPropertyDrawer.cpp
├── ReflectedPropertyDrawer.h
└── InspectorReflectionUtils.h

Project/Editor/Assets/
├── EditorAssetDragPayload.h
├── EditorAssetDragDropBridge.h
├── EditorAssetDatabase.h
└── AssetDatabaseFacade.h

Runtime/UI/
├── GUIDrawer.h
├── GUIDrawer.cpp
└── Widgets/
    ├── Selection/
    ├── Drags/
    ├── InputFields/
    ├── Layout/
    └── Texts/

Runtime/Base/Reflection/
├── MetaProperty.h
├── Field.h
└── Type.h

Runtime/Engine/
├── GameObject.h
├── Components/Component.h
└── Serialize/

Tests/Unit/
└── ReflectedPropertyDrawerTests.cpp
```

**Structure Decision**: The first slice keeps reflected-property control selection in `Project/Editor/Panels/ReflectedPropertyDrawer.*`, keeps reusable primitive widgets in `Runtime/UI`, and adds any editor-only object-reference resolution helpers under `Project/Editor/Panels` or `Project/Editor/Assets`. This avoids coupling runtime UI widgets directly to editor scene/asset services while keeping Inspector and Settings on one reflected property path.

## Unity Reference Baseline

Unity 2018.4 baseline files used for this feature:

- `D:\VSProject\Unity2018.4.0f1\Runtime\BaseClasses\PPtr.h`: runtime `PPtr<T>` stores `InstanceID`; transfer serializes persistent local identity through `m_FileID` and `m_PathID`.
- `D:\VSProject\Unity2018.4.0f1\Modules\BuildPipeline\Editor\Public\ObjectIdentifier.h`: build-time object identity is `guid`, `localIdentifierInFile`, `fileType`, and `filePath`.
- `D:\VSProject\Unity2018.4.0f1\Modules\BuildPipeline\Editor\Managed\ObjectIdentifier.cs`: managed equivalent exposes `guid`, `localIdentifierInFile`, `fileType`, and `filePath`, with `ToString()` displaying `guid/fileID/type/path`.
- `D:\VSProject\Unity2018.4.0f1\Editor\Mono\SerializedProperty.bindings.cs`: `SerializedPropertyType.ObjectReference`, `objectReferenceValue`, `objectReferenceInstanceIDValue`, `ValidateObjectReferenceValue`, and `objectReferenceTypeString`.
- `D:\VSProject\Unity2018.4.0f1\Editor\Mono\EditorGUI.cs`: default `PropertyField` dispatch and common field controls.
- `D:\VSProject\Unity2018.4.0f1\Editor\Mono\ScriptAttributeGUI\Implementations\PropertyDrawers.cs`: Range, Multiline, TextArea, ColorUsage, GradientUsage, Delayed drawers.
- `D:\VSProject\Unity2018.4.0f1\Editor\Mono\ScriptAttributeGUI\Implementations\DecoratorDrawers.cs`: Header and Space decorators.

Unity behavior is a semantic/style reference only, but ObjectReference storage must follow Unity's identity shape. Nullus implementation must use Nullus reflection, UI widgets, asset database, and scene object ownership.

## Implementation Architecture

### Reflected Property Classification

`ReflectedPropertySupport` expands from the current narrow set to include:

- `Vector2`
- `Color`
- `ObjectReference`
- `Array`
- optional metadata-specialized variants such as `RangeScalar`, `MultilineString`, and `TextAreaString`

Classification remains deterministic and must prefer arrays before scalar element types. Unsupported types keep the existing fallback behavior.

Rect/Bounds mapping: this slice adds `NLS::Maths::Rect` and `NLS::Render::Geometry::Bounds` as real reflected value types. `Rect` follows Unity's common `x/y/width/height` data shape. `Bounds` keeps Nullus runtime `center/size` storage, while the Inspector presents Unity-style `Center` and `Extents` controls and converts extents to/from size. Existing `Render::Geometry::BoundingSphere` remains supported.

### Unity-Style Rows

Introduce or consolidate internal helpers around these concepts:

- label normalization and display name,
- fixed label width,
- compact row height and spacing,
- field state colors for normal, disabled, unsupported, and missing,
- common row prefix/suffix controls for clear buttons, object picker buttons, and array foldouts.

The row helper should be private to the reflected drawer until there is evidence another panel needs it as public API.

### ObjectReference

ObjectReference is handled by a unified row model over Unity's actual runtime shape. The verified first slice is intentionally narrower than Unity's full object field: it supports typed resource `PPtr<T>` display/clear and compatible editor asset drag-drop for registered resource target types, plus transient raw pointer display/clear for existing reflected pointer fields. Search picker UI and scene GameObject/Component assignment are target behavior for a later slice.

0. Introduce the Unity-style Object hierarchy below Engine:
   - `NLS::Object` owns an `InstanceID`, registers itself in a global `InstanceID -> Object*` registry, and unregisters on destruction.
   - `NLS::NamedObject` adds a stable display name for resource/assets that Unity models as `NamedObject`.
   - `Material`, `Mesh`, `Shader`, `Texture`, `Texture2D`, and other persistent resource targets derive from `NamedObject`.
   - The object registry is runtime identity only. Serialization archive identity remains in `Engine::Serialize::PersistentManager`.
1. Store component references as `NLS::Engine::Serialize::PPtr<T>`.
   - `PPtr<T>` contains an `InstanceID`, matching Unity's runtime `PPtr<T>` storage.
- `PPtr<T>` can be constructed from `T*`, stores only `T::GetInstanceID()`, and dereferences through `NLS::Object::IDToPointer()` with type validation.
   - `PPtr<T>` is the reflected field type for persistent ObjectReference rows.
2. Add the Unity identity mapping layer:
   - `InstanceID`: transient runtime object handle.
   - `LocalSerializedObjectIdentifier`: local serialized file index plus local file/path ID.
   - `SerializedObjectIdentifier`: local identifier plus external file identifier data.
   - `FileIdentifier`: asset `guid`, numeric `type`, and optional path.
   - `PersistentManager`: maps `InstanceID` to/from those identifiers during serialization and load.
3. Keep `ObjectId` as Nullus' internal authoring identity for object records only. Serialized object references point at object-record `fileID` values; `ObjectId` lookup is performed through ObjectGraph/PersistentManager setup, not a hidden compatibility registry.
4. Keep `AssetId` as Nullus' GUID-backed asset identity, mapped into `FileIdentifier.guid`. External asset references must carry a non-zero `fileID`/local identifier; Nullus does not persist a `fileID = 0` plus `guid/type/path` half-reference.
5. Resolve current value into display state: Empty or stored persistent identity for supported `PPtr<T>` fields, and assigned/empty state for transient raw pointer fields. A richer Unity-style Missing/Incompatible visual badge is deferred.
6. Accept compatible editor asset drag/drop payloads for supported `PPtr<T>` resource targets. Picker selections and scene object/component candidates are deferred.
7. Validate asset payload compatibility before `SetFieldValue`.
8. Write back a typed `PPtr<T>`; raw pointers may be produced by resolver services but are not persistent ObjectReference storage.

First-slice value shapes:

- `NLS::Engine::Serialize::PPtr<NLS::Render::Resources::Mesh>` fields classify as `ObjectReference`. Static mesh ownership follows Unity: `MeshFilter.mesh` is the reflected/persistent `PPtr<Mesh>` field. `MeshRenderer` must not expose a reflected `mesh` property; it only consumes the sibling `MeshFilter` during runtime rendering.
- `NLS::Array<NLS::Engine::Serialize::PPtr<NLS::Render::Resources::Material>>` fields classify as ObjectReference arrays through reflected getter/setter metadata for the same cache-invalidation reason.
- Existing local-object and asset-reference call sites must create/register PersistentManager mappings so the serialized shape remains `fileID`/`guid`/`type`/`filePath`; there is no separate asset-reference property kind or string-path compatibility layer.
- Existing resource pointers used by `GUIDrawer` for `Texture2D*`, `Mesh*`, `Shader*`, and `Material*` are treated as transient resolver/editor inputs only until their owning components migrate to `PPtr<T>`. `Model*` remains a runtime model-package cache and is not an Inspector ObjectReference target.
- `Engine::GameObject*` and `Engine::Components::Component*` are transient display/clear fields in this slice and future resolver results, not serialized reference fields.
- `ObjectReferenceValue` is removed as a final component field/Inspector model. A serialized identifier helper may remain only if renamed to match Unity's identifier role and kept inside the serialization layer.
- Supported resource `PPtr<T>` target metadata, including display label, artifact type, and default sub-asset key prefix, is kept in the shared `PPtrResourceTypes` target table so classification and drag/drop compatibility do not drift.

Missing state must not dereference destroyed pointers. For `PPtr<T>` references, the first slice preserves the mapped `fileID/guid/type/filePath` identity and avoids clearing the `InstanceID`; a richer missing badge/picker replacement flow remains deferred.

### Unity Object System

Unity's Object system is mirrored semantically rather than copied source-for-source:

- `NLS::Object` is the only Object root in Nullus and mirrors Unity's runtime identity/type root. The old `NLS::meta::Object` concept is removed rather than kept as a compatibility alias.
- Every `Object` receives a non-zero `InstanceID` at construction unless a loader explicitly assigns a valid ID.
- The registry supports `Object::IDToPointer(instanceID)` and clears entries when an object is destroyed.
- `PPtr<T>` requires `T` to derive from `NLS::Object` at compile time for pointer construction/dereference operations.
- Type mismatches during dereference return `nullptr` in release-friendly code paths and are covered by tests.
- Object lifetime registration is independent from persistent archive mapping. The PersistentManager can map unloaded asset identities to InstanceIDs before the runtime Object is available, and later object loading can attach that InstanceID to a live Object.
- Unity's load-on-dereference `PPtr` behavior is not part of this slice; Nullus `PPtr::Get()` currently resolves loaded objects through the Object registry, while resource managers and ObjectGraph instantiation perform explicit binding.

Rendering resource migration order:

1. Add `Object`/`NamedObject` in `NLS_Core`.
2. Migrate `Material`, `Mesh`, `Shader`, and `Texture` base classes to `NamedObject`.
3. Keep `Model` as a package/container in this slice to avoid pretending Unity has `PPtr<Model>` renderer fields.
4. Use `MeshFilter.mesh : PPtr<Mesh>` as the persistent static mesh field. `MeshRenderer` keeps renderer/frustum state and consumes resolved Mesh/Model runtime caches through its sibling `MeshFilter`.

### Arrays And Lists

Arrays move from hard-coded `StringArray`/`FloatArray` special cases toward a generic sequential container path. The drawer and runtime reflection treat both `NLS::Array<T>` and `std::vector<T>` as array/list views without converting one container family into the other.

The generic path includes:

- foldout row,
- size editing when the wrapper reports resize/default-construction support,
- add/remove when the wrapper reports insertion/removal support,
- primitive, enum, math, color, Rect/Bounds, LayerMask, and ObjectReference element rows,
- reflected value struct/class element foldouts that recursively draw child fields and write edited copies back to the owning container,
- deterministic unsupported fallback for element categories the drawer cannot safely edit.

Runtime reflection owns the sequential adapter responsibilities:

- `MetaTraits` recognizes `NLS::Array<T>` and `std::vector<T>` as reflected arrays.
- `Variant` and `ArrayWrapper` preserve the original container type and expose a common wrapper.
- `ArrayWrapper` exposes capability flags for constness, set, insert, default insert, remove, and resize. Non-default-constructible and non-assignable element types compile cleanly and simply report unsupported operations.
- `ArrayWrapper` mutates the original container by reference. Accessor-backed array properties still rely on the existing getter/setter write-back path after the edited copy is updated.

MetaParser responsibilities:

- Parse and normalize `NLS::Array<T>`, unqualified `Array<T>`, `std::vector<T>`, and unqualified `vector<T>` field/property types.
- Validate array element types recursively instead of only accepting `NLS::Array<T>`.
- Generate registration using the original container type for getter/setter and wrapper construction, while `meta::Type` continues to expose the established common array view (`element type + isArray`) for classification and serialization.

Drawer recursion semantics:

- `PPtr<T>` and supported object pointers are ObjectReference elements, not expandable value objects.
- Object-derived direct values and raw pointers are not recursively expanded unless a future owned-reference metadata path explicitly says the drawer owns lifetime and mutation.
- Plain reflected struct/class values may expand inline.
- The maximum recursive reflected value depth is 8 for this slice. The drawer also tracks the active type/path stack; repeated type paths render an unsupported recursive fallback.
- Unsupported elements keep their array foldout and index row, but mutation buttons that would require unsupported construction or assignment are disabled or omitted.

### Metadata

Unity-style attributes map to Nullus metadata where possible:

- `Range` -> slider for int/float,
- `Multiline`/`TextArea` -> larger string editor,
- `Header`/`Space`/`Tooltip` -> row decorators.

The implemented first slice includes `Range(min,max)` for int/float reflected fields. `Multiline`, `TextArea`, `Header`, `Space`, and `Tooltip` remain documented metadata gaps until the row decorator/style pass resumes.

### Tags, Layers, And LayerMask

Add the Unity-common tag/layer header behavior without coupling runtime objects to editor persistence:

- `GameObject` stores an integer `layer` with getter/setter auto-property reflection alongside existing `name`, `active`, and `tag`.
- Editor-side `TagLayerSettings` owns default tags and the 32 Unity-style layer slots. It exposes stable choices for Inspector dropdowns.
- Inspector header draws Tag and Layer through `ComboBox` choices. Tags remain stored as strings; layers are stored as integers.
- `NLS::Engine::LayerMask` stores a bit mask and is reflected as a supported Inspector field. The first-slice drawer exposes all 32 layers as checkboxes, including unnamed slots so hidden bits can be cleared. A Unity-style compact MaskField dropdown remains deferred.

## Validation Strategy

- Add failing tests in `Tests/Unit/ReflectedPropertyDrawerTests.cpp` before implementation for each first-slice support category.
- Run targeted unit tests for reflected property drawer behavior.
- If metadata types affect generated reflection, run the normal generation/build path and relevant reflection tests.
- Manually open the editor Inspector on representative GameObjects/settings after tests pass and record visual/runtime evidence before claiming full style parity. If no manual pass is performed, final reporting must state that visual parity remains unverified.

## Complexity Tracking

| Violation | Why Needed | Simpler Alternative Rejected Because |
|-----------|------------|-------------------------------------|
| Editor object-reference resolver | Unity-style ObjectReference spans assets and scene objects | The first slice deliberately supports resource asset drag/drop first; scene object/component assignment remains a later increment |
| Unity-like PPtr/PersistentManager layer | User explicitly requires data structure alignment, not only similar behavior | A DTO named ObjectReferenceValue only mimics serialization and does not match Unity's runtime field model |
| Expanded reflected drawer | Inspector and Settings need one shared drawing path | Per-component hand-written drawers would duplicate behavior and drift from Unity |
| Metadata hooks | Range/text/decorator behavior is field-specific | Hard-coded field names would not scale and would violate reflection source-of-truth |
