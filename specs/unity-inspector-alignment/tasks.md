# Tasks: Unity Inspector Alignment

**Input**: Design documents from `specs/unity-inspector-alignment/`  
**Prerequisites**: `plan.md`, `spec.md`

**Tests**: Required. Behavior changes must use test-first where stable entrypoints exist.

**Organization**: Tasks are grouped by independently testable user story slices.

## Phase 1: Setup And Baseline

**Purpose**: Lock current behavior and identify available reflection/UI hooks before code changes.

- [X] T001 Read current `Project/Editor/Panels/ReflectedPropertyDrawer.h`, `Project/Editor/Panels/ReflectedPropertyDrawer.cpp`, `Runtime/UI/GUIDrawer.h`, and `Runtime/UI/GUIDrawer.cpp` and note current supported types.
- [X] T002 Read `Tests/Unit/ReflectedPropertyDrawerTests.cpp` and identify existing helper patterns for classification, widget creation, and write-back tests.
- [X] T003 Read relevant object reference types in `Runtime/Engine/GameObject.h`, `Runtime/Engine/Components/Component.h`, `Project/Editor/Assets/EditorAssetDragPayload.h`, and existing GUIDrawer resource fields.
- [X] T004 Run the current reflected drawer unit test target or the narrowest available NullusUnitTests filter to capture baseline status before adding failing tests.

---

## Phase 2: User Story 1 - Core Field Types (Priority: P1)

**Goal**: Render and write back Unity-style common reflected field types.

**Independent Test**: A reflected test object or existing reflected objects classify and draw Vector2, Color, Rect/Bounds where local types exist, while existing scalar/vector/enum behavior stays intact.

- [X] T005 [US1] Write failing classification tests for `Vector2` and `Maths::Color` in `Tests/Unit/ReflectedPropertyDrawerTests.cpp`.
- [X] T006 [US1] Run the focused reflected drawer tests and verify the new tests fail for unsupported `Vector2`/`Color`.
- [X] T007 [US1] Add `Vector2` and `Color` entries to `ReflectedPropertySupport` in `Project/Editor/Panels/ReflectedPropertyDrawer.h`.
- [X] T008 [US1] Implement `Vector2` and `Color` classification and drawing in `Project/Editor/Panels/ReflectedPropertyDrawer.cpp` using `GUIDrawer::DrawVec2` and `GUIDrawer::DrawColor`.
- [X] T009 [US1] Run focused reflected drawer tests and verify `Vector2`/`Color` pass.
- [X] T010 [US1] Write failing tests for approved runtime `Maths::Rect` and `Render::Geometry::Bounds` Inspector support.
- [X] T011 [US1] Implement reflected `Maths::Rect` and `Render::Geometry::Bounds` types plus Inspector classification/drawing.
- [X] T012 [US1] Run focused reflected drawer tests for all US1 changes. Evidence: `.\Build\bin\Debug\NullusUnitTests.exe --gtest_filter=ReflectedPropertyDrawerTests.*:TagLayerSettingsTests.*:ReflectionRuntimeCoreTests.*:ReflectionRuntimeEngineTests.*:MetaParserGenerationEngineTests.*:MetaParserGenerationDataTests.*:MetaParserGenerationModuleTests.*` passed 45/45.

---

## Phase 3: User Story 2 - Unity-Style ObjectReference (Priority: P1)

**Goal**: Provide one object-reference row model backed by Unity-style `PPtr<T>`/`InstanceID`/PersistentManager identity data. The verified slice covers supported resource `PPtr<T>` asset drag/drop plus transient raw GameObject/Component/resource pointer display/clear; full scene GameObject/Component picker assignment is deferred.

**Independent Test**: Object-reference fields classify correctly, expose a widget/row state, accept compatible editor asset drag payloads for supported resource `PPtr<T>` fields, reject incompatible payloads, clear values, and display empty/unresolved identity states without crashing.

- [X] T013 [US2] Write failing classification tests for resource pointer, `Engine::GameObject*`, and `Engine::Components::Component*` object-reference field types in `Tests/Unit/ReflectedPropertyDrawerTests.cpp`.
- [X] T014 [US2] Run focused tests and verify object-reference classification fails for the new cases.
- [X] T015 [US2] Add `ObjectReference` classification support in `Project/Editor/Panels/ReflectedPropertyDrawer.h` and `.cpp`.
- [X] T016 [US2] Design a small editor-only object-reference row/state helper in `Project/Editor/Panels/ReflectedPropertyDrawer.cpp` or a focused new `Project/Editor/Panels/InspectorObjectReference.*` pair if the helper exceeds reflected drawer scope.
- [X] T017 [US2] Write failing tests for object-reference write-back and clear behavior using a local reflected fixture.
- [X] T018 [US2] Implement object-reference display, clear, type validation, and write-back for null and direct pointer-backed references.
- [X] T019 [US2] Historical note: this task introduced `ObjectReferenceValue` as a Unity-like serialized identity DTO. This direction is superseded by T047-T064 because Unity's component storage is `PPtr<T>`, not a reflected ObjectReference DTO.
- [X] T020 [US2] Write failing ObjectGraph tests in `Tests/Unit/ObjectGraphDocumentTests.cpp` proving ObjectReference and asset references serialize to `fileID`/`guid`/`type` fields and round-trip through the reader.
- [X] T021 [US2] Write failing Inspector classification tests in `Tests/Unit/ReflectedPropertyDrawerTests.cpp` for fields of type `NLS::Engine::Serialize::ObjectReferenceValue`.
- [X] T022 [US2] Implement ObjectReferenceValue reader/writer support in `Runtime/Engine/Serialize/ObjectGraphReader.h` and `Runtime/Engine/Serialize/ObjectGraphWriter.h`, accepting only Unity-style `fileID`/`guid`/`type` object-reference data.
- [X] T023 [US2] Update ObjectGraph validation, prefab instantiation, asset resolution, and editor prefab helpers to consume `ObjectReferenceValue` as the only persistent scene/asset reference shape.
- [X] T024 [US2] Register ObjectReferenceValue with external reflection in a non-generated runtime header and include it through the normal engine assembly path.
- [X] T025 [US2] Update reflected drawer classification/display/clear behavior so identity-backed ObjectReferenceValue fields are the primary supported ObjectReference path and raw pointers remain transient editor resolver inputs only.
- [X] T026 [US2] Run focused ObjectGraph and ReflectedPropertyDrawer tests for all ObjectReference behavior. Evidence: `.\Build\bin\Debug\NullusUnitTests.exe --gtest_filter=ObjectGraphDocumentTests.ReaderRejectsGuidOnlyObjectReferenceLikeShape:ObjectGraphDocumentTests.WriterRejectsInvalidAssetObjectReferences:PrefabObjectGraphSerializationTests.NonArrayObjectReferenceDoesNotClearMaterialReferenceArray` passed 3/3; full focused validation below passed 175/175.

### Phase 3B: User Story 2 Correction - Real Unity PPtr Model (Priority: P1)

**Purpose**: Replace the temporary `ObjectReferenceValue` component/Inspector model with Unity's real `PPtr<T>` runtime model while keeping `fileID`/`guid`/`type` as the serialized identity shape.

- [X] T047 [US2] Update `spec.md` and `plan.md` to state that `ObjectReferenceValue` is not a final model and that persistent component fields must be typed `PPtr<T>`.
- [X] T048 [US2] Write failing unit tests for `InstanceID`, `LocalSerializedObjectIdentifier`, `FileIdentifier`, `SerializedObjectIdentifier`, and `PersistentManager` mapping behavior.
- [X] T049 [US2] Implement Unity-style identity primitives and `PersistentManager` in `Runtime/Engine/Serialize/`.
- [X] T050 [US2] Write failing unit tests proving `PPtr<T>` stores `InstanceID`, compares by `InstanceID`, clears to null, and resolves through the registry/PersistentManager bridge.
- [X] T051 [US2] Implement `NLS::Engine::Serialize::PPtr<T>` and type-name/reflection helpers for typed `PPtr` fields.
- [X] T052 [US2] Update MetaParser generated field type resolution to recognize `NLS::Engine::Serialize::PPtr<...>` and `NLS::Array<NLS::Engine::Serialize::PPtr<...>>`.
- [X] T053 [US2] Superseded by Phase 3D: early tests targeted `MeshRenderer.mesh` and `MaterialRenderer.materials`; final static mesh ownership is `MeshFilter.mesh`, with material references remaining on `MeshRenderer`.
- [X] T054 [US2] Migrate `MeshRenderer` persistent mesh storage from `ObjectReferenceValue` to typed `PPtr<Mesh>` storage exposed through cache-invalidating reflected accessors while keeping `Model*` as runtime cache/container bridge only.
- [X] T055 [US2] Superseded by Phase 3D/component merge: persistent material storage lives on `MeshRenderer`, exposed through cache-invalidating reflected accessors while keeping `Material*` as runtime cache only.
- [X] T056 [US2] Rename or replace serialization-layer `ObjectReferenceValue` with a Unity-role name such as `SerializedObjectIdentifier`/`ObjectIdentifier`, and keep it out of component field reflection.
- [X] T057 [US2] Update ObjectGraph reader/writer to serialize `PPtr<T>` values through PersistentManager into `fileID`/`guid`/`type`/`filePath`.
- [X] T058 [US2] Update ObjectGraph instantiation to seed PersistentManager mappings before applying reflected fields, then resolve runtime caches from `PPtr` where resource managers are available.
- [X] T059 [US2] Update prefab, asset import/build, editor drag/drop, and runtime asset resolution call sites to produce/consume `PPtr` mappings instead of constructing component `ObjectReferenceValue` values.
- [X] T060 [US2] Update `ReflectedPropertyDrawer` classification/display/clear/write-back so typed `PPtr<T>` fields are the primary ObjectReference path.
- [X] T061 [US2] Remove `ObjectReferenceValue` external reflection registration and tests that expect it as a user-facing reflected type.
- [X] T062 [US2] Run focused PPtr/ObjectGraph/Prefab/ReflectedPropertyDrawer/MetaParser tests and record exact evidence. Evidence: `.\Build\bin\Debug\NullusUnitTests.exe --gtest_filter=PPtrTests.*:ObjectSystemTests.*:ObjectGraphDocumentTests.*:SerializationDiagnosticTests.*:SceneObjectGraphSerializationTests.*:PrefabObjectGraphSerializationTests.*:ReflectedPropertyDrawerTests.*:MetaParserGenerationEngineTests.*:MetaParserGenerationDataTests.*:MetaParserGenerationModuleTests.*:ReflectionRuntimeTestFixture.*:ReflectionRuntimeEngineTests.*:AssetPrefabPipelineTests.BuildsGeneratedModelPrefabHierarchyWithRendererAssetReferences:AssetPrefabPipelineTests.GeneratedModelPrefabInstantiationResolvesSubAssetHintsToArtifacts:AssetPrefabPipelineTests.GeneratedPrefabInstantiationLeavesAmbiguousAssetHintsUnresolved` passed 132/132.
- [X] T063 [US2] Run a normal Debug build target that regenerates reflection through the standard MetaParser path and confirm no generated files were hand-edited. Evidence: `cmake --build Build --target NullusUnitTests --config Debug -- /p:UseSharedCompilation=false /p:UseMultiToolTask=true /m:1 /nodeReuse:false` passed.
- [X] T064 [US2] Self-review the PPtr migration for compatibility shims, raw pointer persistence, generated file boundaries, and Unity source alignment.

### Phase 3C: User Story 2 Correction - Unity Object Runtime Root (Priority: P1)

**Purpose**: Make `PPtr<T>` target Unity-style runtime Object instances, not arbitrary C++ DTO/resource classes, and start moving render resources into the Object hierarchy.

- [X] T065 [US2] Update `spec.md`, `plan.md`, and `tasks.md` to add the Object/NamedObject runtime root, resource inheritance requirement, and `PPtr<Model>` migration warning.
- [X] T066 [US2] Write failing tests proving Object instances receive non-zero `InstanceID`s, are found by `Object::IDToPointer`, and unregister on destruction.
- [X] T067 [US2] Write failing tests proving `PPtr<T>` constructed from an Object-derived pointer stores only the target `InstanceID`, dereferences through the registry, clears to null, and rejects incompatible target types.
- [X] T068 [US2] Implement `NLS::Object` and `NLS::NamedObject` in a lower module than Rendering, with a process-wide registry and deterministic test reset hook.
- [X] T069 [US2] Update `PPtr<T>` to use the Object registry for pointer assignment and dereference while preserving PersistentManager serialized identity mapping.
- [X] T070 [US2] Write failing tests proving `Material`, `Mesh`, `Shader`, and `Texture2D` are Object-system/NamedObject resources with valid `InstanceID`s.
- [X] T071 [US2] Migrate `Material`, `Mesh`, `Shader`, `Texture`, and texture specializations to the Object hierarchy without changing their rendering-facing APIs.
- [X] T072 [US2] Add design/tests for the Unity-aligned `MeshRenderer.mesh` + `PPtr<Mesh>` target path and mark `PPtr<Model>` call sites as rejected migration blockers rather than accepted final behavior.
- [X] T073 [US2] Run focused Object/PPtr/resource/reflection tests and record exact evidence. Evidence: `cmake --build Build --target NullusUnitTests --config Debug -- /p:UseSharedCompilation=false /p:UseMultiToolTask=true /m:1 /nodeReuse:false` passed; `.\Build\bin\Debug\NullusUnitTests.exe --gtest_filter=PPtrTests.*:ObjectSystemTests.*:ObjectGraphDocumentTests.*:SerializationDiagnosticTests.*:SceneObjectGraphSerializationTests.*:PrefabObjectGraphSerializationTests.*:ReflectedPropertyDrawerTests.*:MetaParserGenerationEngineTests.*:MetaParserGenerationDataTests.*:MetaParserGenerationModuleTests.*:ReflectionRuntimeTestFixture.*:ReflectionRuntimeEngineTests.*:AssetPrefabPipelineTests.BuildsGeneratedModelPrefabHierarchyWithRendererAssetReferences:AssetPrefabPipelineTests.GeneratedModelPrefabInstantiationResolvesSubAssetHintsToArtifacts:AssetPrefabPipelineTests.GeneratedPrefabInstantiationLeavesAmbiguousAssetHintsUnresolved` passed 132/132, including Object registry lifetime, typed PPtr dereference, resource NamedObject type reporting, `MeshRenderer.mesh : PPtr<Mesh>`, and generated prefab mesh references.
- [X] T074 [US2] Self-review Object-system implementation for module layering, global registry lifetime, type validation, and absence of compatibility-only DTO paths.

### Phase 3D: User Story 2 Correction - Unity MeshFilter Mesh Ownership (Priority: P1)

**Purpose**: Move static mesh object references to a Unity-style `MeshFilter` component. `MeshRenderer` keeps renderer/frustum/material-facing behavior and may temporarily delegate legacy runtime helper calls, but it must not expose or serialize a reflected `mesh` field.

- [X] T091 [US2] Verify Unity 2018 source shape: `MeshFilter` owns serialized `PPtr<Mesh> m_Mesh`; `MeshRenderer` is not the primary Inspector mesh field owner.
- [X] T092 [US2] Write failing reflection/MetaParser tests expecting `MeshFilter.mesh : PPtr<Mesh>` and no reflected `MeshRenderer.mesh`.
- [X] T093 [US2] Implement `NLS::Engine::Components::MeshFilter` with typed `PPtr<Mesh>` storage, model-path/resource-resolution bridge, and normal MetaParser-generated registration.
- [X] T094 [US2] Update generated model prefab building so mesh object references are emitted on `MeshFilter`, while `MeshRenderer` only emits renderer state such as `frustumBehaviour`.
- [X] T095 [US2] Update object graph deferred asset resolution to bind mesh references through `MeshFilter` instead of `MeshRenderer`.
- [X] T096 [US2] Update editor/runtime creation paths for built-in preview/primitive objects to add and populate `MeshFilter`.
- [X] T097 [US2] Replace remaining legacy `MeshRenderer` mesh helper call sites with direct `MeshFilter` usage and remove the temporary non-reflected delegation methods. Evidence: grep over `Runtime/Engine/Components/MeshRenderer.h/.cpp` found no `GetMeshReference`/`SetMeshReference`/`SetMeshObjectIdentifier`/`SetModel`/`GetModel`/`ResolveModel`/`GetModelPath`/`m_model`/`modelChanged` symbols.
- [X] T098 [US2] Run focused ObjectGraph/Prefab/RenderScene unit tests after unrelated Material test compilation blockers are resolved. Evidence: `cmake --build Build --target NullusUnitTests --config Debug -- /p:UseSharedCompilation=false /p:UseMultiToolTask=false /m:1 /nodeReuse:false /p:LinkIncremental=false` passed; focused `NullusUnitTests` filter for MeshFilter/ObjectGraph/Prefab/RenderScene/import/reflection/PPtr passed 51/51; `cmake --build Build --target ReflectionTest --config Debug -- /p:UseSharedCompilation=false /p:UseMultiToolTask=false /m:1 /nodeReuse:false /p:LinkIncremental=false` passed; `ReflectionTest.exe` passed all registration checks.

---

## Phase 4: User Story 3 - Arrays (Priority: P2)

**Goal**: Replace narrow array handling with a Unity-style foldout/size/element path for supported element arrays.

**Independent Test**: Supported arrays classify through one array path, draw a foldout-style view, resize where reflection supports mutation, and write back edited element values.

- [X] T027 [US3] Write failing tests that existing string, float, and object-reference arrays classify as a generic supported array category while retaining their current element behavior.
- [X] T028 [US3] Run focused tests and verify the generic array test fails.
- [X] T029 [US3] Introduce an array support descriptor in `ReflectedPropertyDrawer.cpp` that identifies supported element type and write-back capability.
- [X] T030 [US3] Implement Unity-style array foldout/size/element drawing for currently mutable supported arrays without regressing existing behavior.
- [X] T031 [US3] Add unsupported-element fallback tests for arrays of unsupported reflected element types if a local fixture exists.
- [X] T032 [US3] Run focused reflected drawer tests for array behavior. Evidence: `.\Build\bin\Debug\NullusUnitTests.exe --gtest_filter=ReflectedPropertyDrawerTests.GenericArrayFieldDrawsSizeAndElementWidgets` passed; final focused suite `.\Build\bin\Debug\NullusUnitTests.exe --gtest_filter=ReflectedPropertyDrawerTests.*:TagLayerSettingsTests.*:ReflectionRuntimeCoreTests.*:ReflectionRuntimeEngineTests.*:MetaParserGenerationEngineTests.*:MetaParserGenerationDataTests.*:MetaParserGenerationModuleTests.*` passed 45/45.

### Phase 4B: User Story 3 - Generic Reflected Arrays And Lists (Priority: P2)

**Purpose**: Upgrade the first array slice from a drawer whitelist to real reflected sequential container support for `NLS::Array<T>` and `std::vector<T>`, including reflected value elements, recursive draw/write-back, capability-aware mutation, and unsupported fallback semantics.

- [X] T081 [US3] Update `spec.md`, `plan.md`, and `tasks.md` with the generic `NLS::Array<T>`/`std::vector<T>` scope, value/ObjectReference/owned-reference semantics, recursion guard, and unsupported fallback rules.
- [X] T082 [US3] Write failing runtime reflection tests for `MetaTraits`, `Variant`, and `ArrayWrapper` support of `std::vector<T>` and reflected value `T`, including default insert, resize, set/write-back, and capability flags.
- [X] T083 [US3] Write failing MetaParser generation/validation tests proving `std::vector<T>`, unqualified `vector<T>`, `NLS::Array<T>`, and nested reflected value elements are accepted and registered as array fields, while invalid element categories fail deterministically.
- [X] T084 [US3] Write failing reflected drawer tests for arrays/lists of reflected value structs/classes, including recursive child field drawing and child edit write-back to the owning array element.
- [X] T085 [US3] Write failing reflected drawer tests for recursion-depth/type-cycle fallback and explicit unsupported element fallback for raw pointers, unreflected class values, unsupported owned references, and non-default-constructible add/resize targets.
- [X] T086 [US3] Implement `MetaTraits`, `Variant`, `ArrayVariantContainer`, `ArrayWrapper`, and `ArrayWrapperContainer` support for `std::vector<T>` and arbitrary reflected `T` with operation capability flags.
- [X] T087 [US3] Implement MetaParser generic sequential container recognition/normalization for `NLS::Array<T>` and `std::vector<T>` through validation and generated registration templates.
- [X] T088 [US3] Replace drawer whitelist-only element support with recursive reflected value drawing, ObjectReference/value/owned-reference classification, capability-aware array controls, and deterministic fallback rows.
- [X] T089 [US3] Run focused red/green validation for Phase 4B runtime, MetaParser, and drawer tests and record exact evidence. Evidence: `cmake --build Build --target NullusUnitTests --config Debug -- /p:UseSharedCompilation=false /p:UseMultiToolTask=false /m:1 /nodeReuse:false /p:LinkIncremental=false` passed; `.\Build\bin\Debug\NullusUnitTests.exe --gtest_filter=ReflectionRuntimeTestFixture.*:ReflectedPropertyDrawerTests.*:MetaParserGenerationEngineTests.*:MetaParserGenerationDataTests.*:MetaParserGenerationModuleTests.*:TagLayerSettingsTests.*:PPtrTests.*` passed 125/125; `cmake --build Build --target ReflectionTest --config Debug -- /p:UseSharedCompilation=false /p:UseMultiToolTask=false /m:1 /nodeReuse:false /p:LinkIncremental=false` passed; `.\Build\bin\Debug\ReflectionTest.exe` passed all registration checks.
- [X] T090 [US3] Self-review Phase 4B for generated-file boundaries, recursion safety, container mutation correctness, accessor-backed write-back, and ObjectReference vs owned-reference separation. Evidence: added regression coverage for exact `GameObject::GetComponent(Component, includeSubType=false)` after standalone `ReflectionTest` exposed a reflected lookup semantic mismatch; added const `NLS::Array<T>` and `std::vector<T>` `Variant` regression coverage so array wrappers preserve read-only capability flags; `docs/REVIEW_PATTERNS.md` was absent in this checkout, so the known-pattern grep gate could not be executed.

---

## Phase 5: User Story 4 - Unity-Like Styling (Priority: P2)

**Goal**: Make reflected Inspector rows visually consistent with Unity 2018 style while preserving existing panel behavior.

**Status**: Not completed in this slice. Automated drawer/widget behavior is covered, but no manual editor visual pass has been recorded, and the private style helper work remains pending.

**Independent Test**: Representative reflected rows use stable label width, compact spacing, component headers, disabled/missing/unsupported visual states, and do not overlap.

- [ ] T033 [US4] Audit current `Inspector.cpp`, `GroupCollapsable`, `Columns`, and reflected drawer row creation for label width, spacing, and component header behavior.
- [ ] T034 [US4] Add non-rendering tests for row label generation, unsupported display, and badge/tooltip text behavior where possible.
- [ ] T035 [US4] Implement a private Unity-style row helper in `ReflectedPropertyDrawer.cpp` for label, field, disabled, missing, unsupported, and compact spacing states.
- [ ] T036 [US4] Update `Inspector.cpp` component/header usage only where required to align spacing and object-reference rows.
- [ ] T037 [US4] Manually verify Inspector layout in the editor on a representative GameObject and record exact evidence in final summary.

---

## Phase 6: Metadata Hooks

**Purpose**: Add the first metadata-driven Unity attribute equivalents only if existing reflection can support them safely.

- [X] T038 Inspect current `MetaProperty` and generated metadata support for field-level custom properties.
- [X] T039 If safe, add minimal metadata types for Range, Multiline/TextArea, Header, Space, and Tooltip through normal reflection sources, not generated files. First slice implemented `Range(min,max)`; other decorators remain deferred.
- [X] T040 Write failing tests for metadata detection in `ReflectedPropertyDrawerTests.cpp`.
- [X] T041 Implement metadata-aware classification/decorators for supported metadata.
- [X] T042 Run reflection generation/build path if metadata source changes require generated output. Evidence: `cmake --build Build --target NullusUnitTests --config Debug -- /p:UseSharedCompilation=false /p:UseMultiToolTask=true /m:1 /nodeReuse:false` passed and regenerated Base/Editor reflection through MetaParser.

---

## Phase 6B: Tag, Layer, And LayerMask Selectors

**Purpose**: Match Unity's common GameObject header selectors and LayerMask reflected field behavior.

- [X] T075 [US1] Write failing tests for `GameObject.layer` runtime access/reflection and default Tag/Layer settings choices.
- [X] T076 [US1] Implement `Engine::LayerMask` and reflected `GameObject.layer` through normal source/generation flow.
- [X] T077 [US1] Add editor-side `TagLayerSettings` with default tags and 32 layer slots.
- [X] T078 [US1] Write failing reflected drawer tests for `LayerMask` classification and write-back.
- [X] T079 [US1] Implement `LayerMask` Inspector drawing and Inspector header Tag/Layer `ComboBox` selectors.
- [X] T080 [US1] Run focused GameObject/reflected drawer/settings tests for Tag/Layer/LayerMask. Evidence: `.\Build\bin\Debug\NullusUnitTests.exe --gtest_filter=ReflectedPropertyDrawerTests.*:TagLayerSettingsTests.*` passed 20/20 before later regression tests; final combined focused suite passed 45/45.

---

## Phase 7: Validation And Review

**Purpose**: Prove the first slice and run the required quality gate.

- [X] T043 Run focused `ReflectedPropertyDrawerTests`/`NullusUnitTests` validation and record exact command/output. Evidence before final rerun: `cmake --build Build --target NullusUnitTests --config Debug -- /p:UseSharedCompilation=false /p:UseMultiToolTask=true /m:1 /nodeReuse:false` passed; `.\Build\bin\Debug\NullusUnitTests.exe --gtest_filter=ReflectedPropertyDrawerTests.GenericArrayFieldCapsAddAtInspectorSizeLimit:ReflectedPropertyDrawerTests.ObjectReferenceArrayElementsCanBeClearedAndWriteBack:ReflectedPropertyDrawerTests.ObjectReferenceWidgetDisplaysAndClearsScenePointers` passed 3/3; `.\Build\bin\Debug\NullusUnitTests.exe --gtest_filter=ReflectionRuntimeTestFixture.ArrayWrapperOwnsWrapperContainerWithMoveOnlySemantics:ReflectionRuntimeTestFixture.RemoveComponentNotifiesRemovedComponentBeforeStorageIsReleased` passed 2/2; `.\Build\bin\Debug\NullusUnitTests.exe --gtest_filter=ReflectedPropertyDrawerTests.*:TagLayerSettingsTests.*:ReflectionRuntimeCoreTests.*:ReflectionRuntimeEngineTests.*:MetaParserGenerationEngineTests.*:MetaParserGenerationDataTests.*:MetaParserGenerationModuleTests.*` passed 45/45; `.\Build\bin\Debug\NullusUnitTests.exe --gtest_filter=EditorSettingsPersistenceTests.*:ObjectGraphDocumentTests.*:SceneObjectGraphSerializationTests.*:SerializationDiagnosticTests.*` passed 70/70. Final evidence: `cmake --build Build --target NullusUnitTests --config Debug -- /p:UseSharedCompilation=false /p:UseMultiToolTask=true /m:1 /nodeReuse:false /p:LinkIncremental=false` passed; `cmake --build Build --target Editor --config Debug -- /p:UseSharedCompilation=false /p:UseMultiToolTask=true /m:1 /nodeReuse:false /p:LinkIncremental=false` passed; `.\Build\bin\Debug\NullusUnitTests.exe --gtest_filter=DebugSceneLifecycleTests.*:RenderSceneCacheTests.SynchronizeRetriesDeferredMeshAndMaterialReferencesAfterResourceRegistration:EditorRenderPathContractTests.EditorPickingAndOutlineRetryDeferredModelAndMaterialReferencesAtSubmitTime:EditorRenderPathContractTests.SceneRenderFrameDoesNotLoadRendererAssetsInsideMainPass:EditorRenderPathContractTests.InspectorDelayedComponentCloseResolvesOwnerByInstanceId:ReflectedPropertyDrawerTests.UnityObjectReferenceDisplayRefreshesAfterClear:ReflectedPropertyDrawerTests.ObjectReferenceArrayElementsCanBeClearedAndWriteBack:ReflectedPropertyDrawerTests.GenericArraySizeGatherUsesSharedArrayState:MetaParserGenerationModuleTests.RejectsPrivateAccessorPropertiesBeforeGeneratingInvalidPrivateFieldShim` passed 22/22; `.\Build\bin\Debug\NullusUnitTests.exe --gtest_filter=ReflectedPropertyDrawerTests.*:TagLayerSettingsTests.*:ReflectionRuntimeCoreTests.*:ReflectionRuntimeEngineTests.*:MetaParserGenerationEngineTests.*:MetaParserGenerationDataTests.*:MetaParserGenerationModuleTests.*` passed 57/57; `.\Build\bin\Debug\NullusUnitTests.exe --gtest_filter=PPtrTests.*:ObjectSystemTests.*:ObjectGraphDocumentTests.*:SerializationDiagnosticTests.*:SceneObjectGraphSerializationTests.*:PrefabObjectGraphSerializationTests.*` passed 99/99; `.\Build\bin\Debug\NullusUnitTests.exe --gtest_filter=DebugSceneLifecycleTests.*:RenderSceneCacheTests.*` passed 29/29. P0/P1 regression fixes added after review: `PPtrTests.PersistentManagerTreatsAssetPathAsHintNotIdentity`, `SceneObjectGraphSerializationTests.ReflectedMeshReferenceWriteClearsRendererRuntimeModelCache`, `SceneObjectGraphSerializationTests.ReflectedMaterialReferencesWriteClearsRendererRuntimeMaterialCache`, model/material retry tests, scene destroy fast-access tests, ObjectReference stale-display test, inspector delayed-close test, MetaParser private accessor rejection test, and shared array-state gather test.
- [X] T044 Run any required reflection/MetaParser validation if metadata or reflected types changed. Evidence: the focused validation includes `MetaParserGenerationEngineTests.*`, `MetaParserGenerationDataTests.*`, `MetaParserGenerationModuleTests.*`, `ReflectionRuntimeCoreTests.*`, and `ReflectionRuntimeEngineTests.*`; `cmake --build Build --target ReflectionTest --config Debug -- /p:UseSharedCompilation=false /p:UseMultiToolTask=true /m:1 /nodeReuse:false /p:LinkIncremental=false` passed; `.\Build\bin\Debug\ReflectionTest.exe` passed all registration checks.
- [X] T045 Perform a self-review for unsupported type fallbacks, ObjectReference invalidation, generated-file boundaries, and Editor/Game runtime preservation. Evidence: added `OwnedVariantDispatchersOutliveCallerAndStillWriteBack` after self-review found widget lambdas could outlive stack-local `meta::Variant`; fixed `ArrayWrapper` ownership, component-removal notification lifetime, object-reference array clear/write-back, array size cap enforcement, stale raw pointer display safety, clean-build include gap, renderer cache invalidation through accessor-backed reflected properties, PersistentManager identity equality ignoring path hints, renderer resource retry after first miss, Inspector delayed callback lifetime capture, generic array shared state reuse, Unity-style Bounds extents UI over Nullus center/size storage, and PPtr resource target traits sharing one table for label/artifact/subasset metadata.
- [X] T046 Run `/plan-review` quality review loop to the repository-required threshold before reporting completion. Evidence: R1 adversarial review and deeper audit found 0 P0; reported P1 items were reconciled against current code/tests, with the real const container capability gap fixed by `ReflectionRuntimeTestFixture.ConstVectorVariantExposesReadOnlyArrayWrapper` and `ReflectionRuntimeTestFixture.ConstNlsArrayVariantExposesReadOnlyArrayWrapper`. Final review status: 72/80, no remaining P0/P1 in the completed generic array/list slice; remaining notes are P2/future-scope items such as project-backed Tag/Layer settings and PPtr target table polish. Fresh validation after review: `cmake --build Build --target NullusUnitTests --config Debug -- /p:UseSharedCompilation=false /p:UseMultiToolTask=false /m:1 /nodeReuse:false /p:LinkIncremental=false` passed; a concurrent `ReflectionTest` build first hit an Autodesk FBX SDK `.lastbuildstate` MSBuild file lock, then the sequential rerun of `cmake --build Build --target ReflectionTest --config Debug -- /p:UseSharedCompilation=false /p:UseMultiToolTask=false /m:1 /nodeReuse:false /p:LinkIncremental=false` passed; `.\Build\bin\Debug\NullusUnitTests.exe --gtest_filter=ReflectionRuntimeTestFixture.*:ReflectedPropertyDrawerTests.*:MetaParserGenerationEngineTests.*:MetaParserGenerationDataTests.*:MetaParserGenerationModuleTests.*:TagLayerSettingsTests.*:PPtrTests.*` passed 125/125; `.\Build\bin\Debug\ReflectionTest.exe` passed all registration checks. `docs/REVIEW_PATTERNS.md` / `Docs/REVIEW_PATTERNS.md` was absent in this checkout, so the known-pattern grep gate could not be executed.

## Dependencies & Execution Order

- Phase 1 blocks all implementation phases.
- Phase 2 and Phase 3 both depend on Phase 1 and may proceed independently after baseline is understood.
- Phase 3C depends on Phase 3B because `PPtr<T>` must exist before it can be restricted to Object-system targets.
- Phase 4 depends on Phase 2 because array element drawing reuses core type support.
- Phase 5 depends on Phase 2 and Phase 3 because the style helper must cover both regular and object-reference rows.
- Phase 6 can run after Phase 2 if metadata is needed for Range/text/decorator behavior.
- Phase 7 depends on selected implementation phases.

## Implementation Strategy

1. Deliver US1 + the identity-backed subset of US2 first as the MVP because they cover the user's selected scope and ObjectReference data-model requirement.
2. Keep array and metadata work incremental, preserving current string/float array behavior while expanding architecture.
3. Reuse `GUIDrawer` and existing widgets before adding new UI primitives.
4. Keep all generated files untouched unless the normal generation pipeline updates them.
5. Validate every behavior-changing slice with focused tests before moving to the next slice.
