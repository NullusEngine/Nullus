# Feature Specification: Unity Inspector Alignment

**Feature Branch**: `unity-inspector-alignment`
**Created**: 2026-05-20
**Status**: First slice implemented; visual/manual parity and full scene object picker remain pending
**Input**: User description: "Align the Nullus editor Inspector controls and behavior with Unity 2018.4 Inspector, including core field types, Unity-style layout, ObjectReference support for assets, scene GameObjects, Components, and script/resource objects, arrays/lists, Range sliders, LayerMask, Tag, and Layer selectors. ObjectReference must match Unity's underlying `PPtr<T>`/`InstanceID`/PersistentManager data model, not only mimic UI behavior or serialize a similar DTO."

**Implemented Slice Note**: This bundle currently documents both the Unity-aligned target and the verified first slice. The first slice supports reflected Rect, Bounds, Range sliders, supported-element arrays, Tag/Layer selectors, LayerMask editing, typed resource `PPtr<T>` ObjectReference display/clear/asset drag-drop, and transient raw pointer display/clear. The next array/list slice upgrades the array path from a hard-coded element whitelist to reflected `NLS::Array<T>` and `std::vector<T>` containers with recursive value-type element drawing, explicit unsupported fallbacks, and recursion guards. It does not yet provide Unity's full searchable object picker, scene GameObject/Component assignment flow, prefab override UI, or manually verified pixel-level style parity.

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Edit Core Fields Like Unity (Priority: P1)

An editor user selects a GameObject or settings object and edits common reflected fields with controls that match Unity Inspector expectations for labels, field layout, and value editing.

**Why this priority**: Scalar, vector, color, enum, rectangle, bounds, and array fields are the daily Inspector workflow. They must be reliable before specialized editors matter.

**Independent Test**: Show a reflected object containing each first-slice supported field type, edit every field, and confirm values are written back without custom per-component drawing code.

**Acceptance Scenarios**:

1. **Given** a reflected field is a supported scalar, vector, quaternion, color, rect, bounds, enum, or string type, **When** Inspector renders it, **Then** the user sees a Unity-style labelled field with the correct editor control.
2. **Given** the user edits a supported field, **When** the value changes, **Then** the backing reflected object is updated and the scene/settings dirty callback is invoked.
3. **Given** a reflected field type is not supported in this phase, **When** Inspector renders it, **Then** the field displays a deterministic unsupported/read-only fallback without crashing.

---

### User Story 2 - Assign Supported Object References Through One Field (Priority: P1)

An editor user assigns supported persistent resource references through one Unity-style object field backed by typed `PPtr<T>` identity data. Scene GameObject and Component assignment are target behavior, but the first slice only displays and clears transient raw pointer fields for those categories.

**Why this priority**: ObjectReference is central to Unity Inspector usability. Without a unified object field, components cannot expose natural asset and scene relationships.

**Independent Test**: Use a reflected object with typed resource `PPtr<T>`, raw GameObject pointer, raw Component pointer, and supported resource pointer fields; assign valid asset drag payloads for supported resource `PPtr<T>` fields, clear values, reject incompatible asset payloads, and confirm stored values remain type-correct.

**Acceptance Scenarios**:

1. **Given** a reflected object-reference field expects a supported resource `PPtr<T>` such as Texture, Mesh, Shader, or Material, **When** Inspector renders it, **Then** one object row shows the current persistent identity/path or empty state, target type, clear affordance, and asset drag-drop target.
2. **Given** a compatible editor asset drag payload is dropped on a supported resource `PPtr<T>` field, **When** the object field accepts it, **Then** the backing reflected value updates to a typed `PPtr<T>` backed by PersistentManager identity.
3. **Given** an incompatible asset payload is dropped, **When** the object field validates it, **Then** the value is not changed and the UI path remains non-crashing.
4. **Given** the current `PPtr<T>` identity is not backed by a live object, **When** Inspector renders the field, **Then** it preserves and displays the persistent path, GUID, or fileID identity where available and still allows clearing or replacing the reference.
5. **Given** an object reference is stored on a runtime component, **When** the backing field is inspected, **Then** it is a typed `PPtr<T>` whose memory value is an `InstanceID`, not a raw pointer and not a reflected DTO.
6. **Given** a `PPtr<T>` is serialized, **When** the object graph writer emits a reference, **Then** it writes Unity-style identity fields `fileID`, `guid`, `type`, and optional path hint resolved through the PersistentManager mapping.

---

### User Story 3 - Work With Arrays Predictably (Priority: P2)

An editor user expands an array/list field, changes its size, edits elements, and adds or removes entries without a custom drawer.

**Why this priority**: Unity's Inspector relies on foldout arrays for serialized lists. Nullus currently only has narrow string/float array special cases.

**Independent Test**: Render reflected `NLS::Array<T>` and `std::vector<T>` fields, expand/collapse them, resize them where the container supports mutation, edit primitive/object-reference/reflected value elements, and confirm the reflected array value is updated or a deterministic fallback is shown for unsupported element semantics.

**Acceptance Scenarios**:

1. **Given** a reflected `NLS::Array<T>` or `std::vector<T>` field whose element type is editable, **When** Inspector renders it, **Then** the user sees a foldout row with size and element rows.
2. **Given** the user changes the array size, **When** the element type is default-constructible and the container is mutable, **Then** new elements are default-initialized and removed elements no longer persist.
3. **Given** an array element is a reflected value struct/class, **When** the element is expanded, **Then** the drawer recursively renders its reflected child fields and writes child edits back through the parent array element.
4. **Given** an array element type is an ObjectReference type such as `PPtr<T>`, **When** Inspector renders the element, **Then** it uses the ObjectReference row rather than recursively expanding the referenced object.
5. **Given** an array element type is unsupported, non-default-constructible for add/resize, a raw pointer, or an object-owned reference without supported ownership metadata, **When** Inspector renders it, **Then** each element or mutation affordance shows a deterministic fallback/disabled state without crashing.
6. **Given** reflected value nesting contains recursive type paths or exceeds the supported depth, **When** Inspector renders the array, **Then** recursion stops at a deterministic fallback row instead of overflowing the stack.

---

### User Story 4 - Use Unity-Like Inspector Styling (Priority: P2)

An editor user experiences the Inspector as a compact Unity-aligned property sheet with predictable component headers, spacing, label widths, and disabled/unsupported states.

**Why this priority**: Field support alone is not enough; the user explicitly asked to align both function and style with Unity Inspector.

**Independent Test**: Open Inspector on representative objects and compare row spacing, label/field alignment, component headers, object fields, arrays, unsupported fields, and edited states against the documented Unity-aligned layout rules.

**Acceptance Scenarios**:

1. **Given** a selected GameObject has several components, **When** Inspector draws them, **Then** component headers use compact foldout styling and stable spacing.
2. **Given** a property row is drawn, **When** it appears in Inspector, **Then** label and field columns stay aligned across scalar, vector, object reference, array, and unsupported rows.
3. **Given** a field is read-only, unsupported, disabled, missing, or invalid, **When** it appears, **Then** the visual state is distinct without breaking layout.

### Edge Cases

- The selected object has no reflected fields.
- A field has no setter or is otherwise read-only.
- A reflected object has nested or generic class fields that are not part of the first slice.
- A field refers to an asset path/GUID that no longer resolves.
- A transient raw pointer field refers to a scene object or Component that was destroyed after selection.
- A future Component `PPtr<T>` object reference points to the correct GameObject but the specific Component type is missing.
- A drag/drop payload has the right payload kind but the wrong target type.
- Arrays are empty, resized to zero, or resized beyond the old size.
- Multiple components expose fields with identical display labels.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: Inspector MUST use the reflected property drawing path for supported reflected GameObject Component fields and reflected editor setting fields.
- **FR-002**: The reflected property drawer MUST support bool, signed integer, float, string, enum, Vector2, Vector3, Vector4, Quaternion, Color, Rect, Bounds, and the existing BoundingSphere field type.
- **FR-003**: The reflected property drawer MUST support Unity-style object-reference rows for supported resource `PPtr<T>` asset references and transient raw GameObject/Component/resource pointer display/clear. Full scene GameObject/Component assignment through a picker is deferred.
- **FR-004**: Object-reference fields MUST present current reference name/type, empty state, clear action, and an asset drag-drop target for supported resource `PPtr<T>` fields through one consistent row pattern. Missing-state presentation is limited to preserving the stored persistent identity when no live object is resolved.
- **FR-005**: Object-reference fields MUST validate editor asset drag payloads against the reflected `PPtr<T>` target type before writing values.
- **FR-006**: Object-reference fields MUST migrate supported Nullus asset/resource references to Unity's `PPtr<T>` identity convention instead of preserving a parallel persistent reference shape. Scene object and Component persistent references remain future work.
- **FR-014**: ObjectReference backing storage MUST match Unity's runtime model: typed `PPtr<T>` fields store an `InstanceID`; PersistentManager maps `InstanceID` to local `fileID`/path IDs and external asset `guid`/`type`/path data during serialization.
- **FR-015**: Raw pointers to `GameObject`, `Component`, or resources MUST NOT be treated as the persistent ObjectReference data model. They may only be transient dereference/resolver results of a `PPtr<T>`.
- **FR-016**: Existing ObjectGraph scene and asset references MUST serialize from the Unity-style `PPtr`/PersistentManager pipeline, so serialized reference data has one authoritative `fileID`/`guid`/`type` shape and no legacy `$ref`/`$asset` object-reference format.
- **FR-017**: `ObjectReferenceValue` MUST NOT remain as the component/Inspector backing field type. If a serialized identity struct is needed inside `ObjectGraphDocument`, it MUST be named for Unity's serialized identifier role, not as the runtime object-reference model.
- **FR-018**: MetaParser and runtime reflection MUST understand typed `PPtr<T>` property types such as `NLS::Engine::Serialize::PPtr<NLS::Render::Resources::Mesh>` and arrays such as `NLS::Array<NLS::Engine::Serialize::PPtr<NLS::Render::Resources::Material>>`. Static mesh references MUST be exposed by `MeshFilter.mesh`, not by `MeshRenderer.mesh`; accessor-backed reflected properties remain allowed where setters are required to invalidate runtime caches.
- **FR-019**: Nullus MUST introduce a Unity-style runtime object root below the rendering and engine modules so render resources can participate in object identity without making `NLS_Render` depend on `NLS_Engine`.
- **FR-020**: Runtime objects that can be the target of persistent object references MUST derive from the Nullus Unity-style Object hierarchy, own a non-zero `InstanceID`, and register/unregister that ID with a global object registry during lifetime.
- **FR-021**: `PPtr<T>` MUST only model references to Object-system types, store the target `InstanceID`, and dereference through the Object registry with type validation rather than storing or serializing raw pointers.
- **FR-022**: `Material`, `Mesh`, `Shader`, `Texture`, and texture specializations MUST enter the Object hierarchy as named asset/resource objects. They may keep their existing rendering/runtime APIs, but object identity must come from the shared Object base.
- **FR-023**: Model-package/container data MUST NOT be the Inspector object-reference target for mesh rendering. Mesh references MUST use `PPtr<Mesh>` plus renderer material references; any reintroduced `PPtr<Model>` usage is a migration blocker, not an accepted compatibility layer.
- **FR-034**: Static mesh GameObjects MUST follow Unity's component split: `MeshFilter` owns the serialized/reflected `PPtr<Mesh>` mesh reference, while `MeshRenderer` owns renderer/frustum state and consumes the mesh through the sibling `MeshFilter` at runtime.
- **FR-007**: The reflected property drawer MUST support basic arrays of supported element types with foldout, size, element rows, add/remove, and write-back behavior.
- **FR-008**: The reflected property drawer MUST keep deterministic fallbacks for unsupported scalar, object, array, and nested field types.
- **FR-009**: The Inspector visual layout MUST continue moving toward Unity-aligned compact rows, stable label width, right-side field area, consistent spacing, component foldout headers, and disabled/missing/unsupported visual states. The first slice has automated row/widget coverage, but the manual visual parity pass is not complete.
- **FR-010**: The implementation MUST reuse existing Nullus UI widgets and GUIDrawer helpers where they already match the needed behavior.
- **FR-011**: The implementation MUST add metadata-driven hooks for Range sliders, multiline/text area strings, header/space/tooltip decorators, or document any first-slice metadata gaps in the same spec bundle.
- **FR-012**: The feature MUST include focused tests for new type classification, widget creation, value write-back, object-reference validation, and array behavior where stable test entrypoints exist.
- **FR-013**: Existing Inspector behavior for Transform and already supported reflected fields MUST remain compatible unless explicitly improved by the new Unity-aligned row pattern.
- **FR-024**: Nullus MUST introduce reflected runtime `Maths::Rect` and `Render::Geometry::Bounds` value types so reflected Inspector fields can model Unity's common Rect and Bounds controls instead of treating them as unsupported gaps.
- **FR-025**: Numeric reflected fields with `meta::Range(min,max)` metadata MUST render through slider widgets for supported int and float fields and MUST preserve the existing drag/input behavior for fields without Range metadata.
- **FR-026**: `GameObject` MUST expose a Unity-style integer layer alongside its tag, and the Inspector header MUST render Tag and Layer using editor-managed selector choices instead of free-form Tag text.
- **FR-027**: Reflected `LayerMask` fields MUST render a general reflected control that edits the stored bit mask without requiring per-component custom drawer code. The first slice uses a 32-checkbox layer list rather than Unity's compact MaskField dropdown.
- **FR-028**: MetaParser and runtime reflection MUST recognize both `NLS::Array<T>` and `std::vector<T>` as sequential reflected array/list containers, including nested reflected value element types.
- **FR-029**: `ArrayWrapper` MUST support mutable operations for arbitrary reflected element `T` where the C++ type permits them: get, set/write-back, remove, default construction, insert, and resize. Unsupported operations MUST be reported through capability flags instead of relying on hard crashes or template instantiation failures.
- **FR-030**: The reflected property drawer MUST recursively draw reflected value struct/class array elements using the same field classification path as top-level reflected values.
- **FR-031**: The recursive reflected drawer MUST enforce a finite depth limit and cycle/type-path guard so self-referential or deeply nested reflected values render a fallback instead of overflowing.
- **FR-032**: The drawer MUST distinguish ObjectReference (`PPtr<T>` and supported transient object pointers), ordinary value types, and owned-reference semantics. ObjectReference values MUST remain object fields; ordinary reflected value types may expand inline; owned references are editable only when explicit ownership metadata/storage support exists, otherwise they use fallback.
- **FR-033**: Unsupported array/list element categories MUST have an explicit fallback state covering raw pointers, unreflected class values, abstract/non-default-constructible add targets, non-assignable element values, and unsupported owned references.

### Key Entities

- **Reflected Property Field**: A reflected field plus metadata, value access, write-back rules, visual state, and supported-control classification.
- **Inspector Row**: A Unity-style label/field row with stable layout, optional decorator metadata, and field-specific content.
- **PPtr<T>**: A Unity-style typed persistent pointer. It stores only an `InstanceID` in memory, dereferences through the object/PersistentManager registry, and is the reflected component field type for persistent ObjectReference fields.
- **Object**: The Nullus equivalent of Unity's native `Object`. It owns an `InstanceID`, registers the instance in a process-wide `InstanceID -> Object*` registry, exposes virtual type/name hooks, and is the only legal runtime target family for `PPtr<T>`.
- **NamedObject**: The Nullus equivalent of Unity's `NamedObject` resource base. It adds a user-facing object name on top of Object identity and is the base for asset-like resources such as Material, Mesh, Shader, and Texture.
- **InstanceID**: A transient runtime object handle used by `PPtr<T>`. It may resolve to a loaded runtime object or to a PersistentManager record that can be loaded/resolved later.
- **PersistentManager**: The identity mapping service that converts between `InstanceID`, local serialized object identifiers, and external asset identifiers.
- **Serialized Object Identifier**: The object graph/archive representation of a `PPtr`, containing `fileID`, `guid`, `type`, and optional `filePath`. It is not the runtime field type.
- **Object Reference Resolver**: The editor service that validates and resolves object-reference candidates. In the verified first slice this covers editor asset drag payloads for supported resource `PPtr<T>` fields; scene object, component, and generic reflected-object candidates remain target behavior.
- **Array Property View**: A foldout state plus size row and element rows for reflected array values.
- **Sequential Array Wrapper**: A reflection runtime adapter over `NLS::Array<T>` or `std::vector<T>` that exposes size, element access, mutation capability flags, default insertion, removal, resize, and write-back without converting between container families.
- **Reflected Value Element**: A value-type array/list element whose reflected fields can be drawn inline and whose edited copy is written back to the owning container.
- **Owned Reference Element**: A reference-like element whose lifetime is owned by the serialized object or container. This is separate from ObjectReference identity; without explicit ownership metadata and construction/destruction semantics, it is intentionally shown as unsupported.
- **Rect**: A math value with `x`, `y`, `width`, and `height` fields, drawn as a Unity-style four-scalar compound field.
- **Bounds**: A geometry value with `center` and `size` vector fields, drawn as a Unity-style compound field.
- **LayerMask**: A reflected runtime bit-mask value representing one or more selected layers.
- **Tag/Layer Settings**: Editor-managed default tag and layer name lists used by GameObject header selectors and LayerMask display.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: A representative reflected object containing all first-slice supported non-array field types can be edited through Inspector with 100% write-back success in focused tests.
- **SC-002**: A representative supported resource `PPtr<T>` object-reference field can accept compatible asset drag payloads, reject incompatible payloads, clear values, and display unresolved persistent identities without crashing; raw GameObject/Component pointer fields can be displayed and cleared as transient editor values.
- **SC-006**: Automated tests prove that ObjectReference component storage uses typed `PPtr<T>`, ObjectGraph serialization emits Unity-style `fileID`/`guid`/`type` fields through PersistentManager mapping, and Inspector classification recognizes typed `PPtr<T>` properties. For static meshes, the passing shape is `MeshFilter.mesh : PPtr<Mesh>` and no reflected `MeshRenderer.mesh` field.
- **SC-003**: A supported reflected array can be expanded, resized, edited, and collapsed in a focused test or documented manual editor pass.
- **SC-004**: Adding a new reflected field of a first-slice supported type requires no hand-written Inspector code for that component.
- **SC-005**: The Inspector remains runnable and visually aligned after the change, with no regressions in existing `ReflectedPropertyDrawerTests`.

## Assumptions

- Unity 2018.4 source under `D:\VSProject\Unity2018.4.0f1` is the functional/style reference for common `EditorGUI.PropertyField` behavior.
- The first implementation aligns semantics and layout; it does not port Unity source or recreate every Unity custom editor.
- Full multi-object editing, prefab override indicators, UnityEvent editing, AnimationCurve/Gradient popups, scene GameObject/Component assignment, and advanced search object picker behavior are deferred.
- Existing Nullus reflection metadata is the source of truth for reflected fields. New metadata may be added only when needed to model Unity-style field attributes safely.
- ObjectReference support is delivered incrementally behind one row model: typed resource `PPtr<T>` and PersistentManager identity first, asset drag/drop for supported resources in this slice, scene GameObject/Component resolvers next, and generic reflected object pointers only as transient resolver outputs when reflection can express them safely. Nullus currently aligns storage and serialized identity with Unity-style `PPtr`; Unity's automatic `PPtr` dereference/load-on-miss behavior is not implemented in this slice.
- Existing dirty-state callbacks and scene/settings persistence paths remain responsible for persistence after property write-back.
- Unity alignment uses Nullus module boundaries: Object/NamedObject identity lives below `NLS_Render`, while serialization-specific `ObjectIdentifier` and PersistentManager archive mapping remain in `NLS_Engine::Serialize`.
- Tag/layer project settings persistence can be introduced as an editor-side static/default settings service in this slice; full project asset serialization for tag/layer definitions is deferred unless already available.
