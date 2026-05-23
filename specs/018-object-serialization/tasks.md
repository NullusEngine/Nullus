# Tasks: Object Graph Serialization

**Input**: Design documents from `specs/018-object-serialization/`  
**Prerequisites**: `plan.md`, `spec.md`, `research.md`, `data-model.md`, `contracts/`, `quickstart.md`

**Tests**: Required. This feature changes runtime identity, reflection serialization, scene loading, prefab behavior, and generated reflection inputs. Follow test-first or test-with-change discipline for every behavior-changing task.

**Organization**: Tasks are grouped by independently testable user stories.

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Establish the feature test fixtures and shared serialization scaffolding.

- [X] T001 Create serialization golden fixture directory in `Tests/Unit/Fixtures/ObjectGraph/`
- [X] T002 [P] Add empty test files `Tests/Unit/GuidTests.cpp`, `Tests/Unit/ObjectGraphDocumentTests.cpp`, `Tests/Unit/SceneObjectGraphSerializationTests.cpp`, `Tests/Unit/PrefabObjectGraphSerializationTests.cpp`, and `Tests/Unit/SerializationDiagnosticTests.cpp`
- [X] T003 [P] Add initial Object Graph headers with no behavior in `Runtime/Engine/Serialize/ObjectId.h`, `Runtime/Engine/Serialize/ObjectGraphDocument.h`, and `Runtime/Engine/Serialize/SerializationDiagnostic.h`
- [X] T004 [P] Add initial GUID header/source placeholders in `Runtime/Base/Guid.h` and `Runtime/Base/Guid.cpp`

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Build the identity and metadata foundation that all scene and prefab work depends on.

**CRITICAL**: No scene or prefab implementation should begin until this phase is complete.

- [X] T005 Write failing GUID parse/format/random/deterministic/hash tests in `Tests/Unit/GuidTests.cpp`
- [X] T006 Implement GUID creation, canonical formatting, parsing, equality, ordering, and hashing in `Runtime/Base/Guid.h` and `Runtime/Base/Guid.cpp`
- [X] T007 Add GUID reflection or serialization value support tests in `Tests/Unit/GuidTests.cpp`
- [X] T008 Implement reflected/JSON GUID value handling through maintained reflection inputs in `Runtime/Base/Reflection/RuntimeMetaProperties.h` or adjacent Base reflection files
- [X] T009 Write failing strong ID tests for `ObjectId` and `AssetId` in `Tests/Unit/ObjectGraphDocumentTests.cpp`
- [X] T010 Implement `ObjectId` and `AssetId` strong GUID wrappers in `Runtime/Engine/Serialize/ObjectId.h`
- [X] T011 Write failing diagnostics construction and severity tests in `Tests/Unit/SerializationDiagnosticTests.cpp`
- [X] T012 Implement serialization diagnostic codes, severity, target fields, and aggregation in `Runtime/Engine/Serialize/SerializationDiagnostic.h`
- [X] T013 Add serialization metadata tests for persistent, transient, owned, object-reference, asset-reference, editor-only, runtime-only, and rename metadata in `Tests/Unit/ObjectGraphDocumentTests.cpp`
- [X] T014 Extend runtime metadata declarations for serialization field intent in `Runtime/Base/Reflection/RuntimeMetaProperties.h`
- [X] T015 Run `cmake --build build --config Debug --target NullusUnitTests ReflectionTest -- /m:1` and fix foundational build or reflection failures

**Checkpoint**: GUID, strong IDs, diagnostics, and serialization metadata are available and tested.

---

## Phase 3: User Story 1 - Save And Load Scenes As Object Graphs (Priority: P1) MVP

**Goal**: Save and load scenes through a deterministic Object Graph document without `worldID`, constructor actor IDs, playing-state references, or legacy serialized scene records.

**Independent Test**: A scene with game objects, components, and parent references round-trips through the Object Graph writer/reader and validates semantic equivalence.

### Tests for User Story 1

- [X] T016 [P] [US1] Write failing tests that `Scene` is a reflected object root in `Tests/Unit/SceneObjectGraphSerializationTests.cpp`
- [X] T017 [P] [US1] Write failing tests that `GameObject` construction requires no actor ID or playing-state reference in `Tests/Unit/SceneObjectGraphSerializationTests.cpp`
- [X] T018 [P] [US1] Write failing tests that new scene output contains no `worldID`, `SerializedSceneData`, `SerializedActorData`, or `SerializedComponentData` shape in `Tests/Unit/SceneObjectGraphSerializationTests.cpp`
- [X] T019 [P] [US1] Write failing Object Graph document validation tests for duplicate IDs, invalid GUIDs, missing root, ownership cycle, missing object reference, and orphaned owned object in `Tests/Unit/ObjectGraphDocumentTests.cpp`
- [X] T020 [P] [US1] Write failing deterministic scene golden-output test in `Tests/Unit/SceneObjectGraphSerializationTests.cpp` using `Tests/Unit/Fixtures/ObjectGraph/`
- [X] T021 [P] [US1] Write failing scene round-trip tests for game objects, components, component order, parent refs, and component owner repair in `Tests/Unit/SceneObjectGraphSerializationTests.cpp`

### Implementation for User Story 1

- [X] T022 [US1] Make `Scene` inherit the reflected object model and expose serializable root metadata in `Runtime/Engine/SceneSystem/Scene.h` and `Runtime/Engine/SceneSystem/Scene.cpp`
- [X] T023 [US1] Remove `p_actorID`, `p_playing`, `m_playing`, `worldID`, `GetWorldID`, and `SetWorldID` from `Runtime/Engine/GameObject.h` and `Runtime/Engine/GameObject.cpp`
- [X] T024 [US1] Remove scene ID allocation APIs and `FindActorByID` from `Runtime/Engine/SceneSystem/Scene.h` and `Runtime/Engine/SceneSystem/Scene.cpp`
- [X] T025 [US1] Update runtime/editor call sites that used `GetWorldID` to use object identity or temporary lookup placeholders in `Project/Editor/` and `Runtime/Engine/`
- [X] T026 [US1] Implement transient picking registry replacement in `Runtime/Engine/Serialize/PickRegistry.h` or the appropriate editor/rendering module file selected during implementation
- [X] T027 [US1] Implement `ObjectGraphDocument`, property values, `$owned`, `$ref`, `$asset`, and validation data structures in `Runtime/Engine/Serialize/ObjectGraphDocument.h`
- [X] T028 [US1] Implement Object Graph JSON reader/writer deterministic formatting in `Runtime/Engine/Serialize/ObjectGraphReader.h` and `Runtime/Engine/Serialize/ObjectGraphWriter.h`
- [X] T029 [US1] Implement scene graph build and staged scene instantiation in `Runtime/Engine/Serialize/ObjectGraphSerializer.h` and `Runtime/Engine/Serialize/ObjectGraphInstantiator.h`
- [X] T030 [US1] Replace old scene serialization registration path and remove or neutralize legacy `Runtime/Engine/Serialize/GameobjectSerialize.cpp` behavior
- [X] T031 [US1] Remove long-term dependency on `Runtime/Engine/Serialize/SceneSerializationData.h` and delete it if no remaining build target requires it
- [X] T032 [US1] Rebuild scene fast-access caches after Object Graph load in `Runtime/Engine/SceneSystem/Scene.cpp`
- [X] T033 [US1] Run `cmake --build build --config Debug --target NullusUnitTests ReflectionTest -- /m:1` and `.\build\bin\Debug\ReflectionTest.exe`

**Checkpoint**: User Story 1 is fully functional and testable independently.

---

## Phase 4: User Story 2 - Author And Instantiate Prefabs (Priority: P2)

**Goal**: Save prefab Object Graph assets and instantiate them into scenes with new ObjectIds and explicit overrides.

**Independent Test**: A prefab with child objects and components instantiates into a scene, records source-to-instance mapping, preserves property and owned-object overrides, and round-trips through save/load.

### Tests for User Story 2

- [X] T034 [P] [US2] Write failing prefab document save/load tests in `Tests/Unit/PrefabObjectGraphSerializationTests.cpp`
- [X] T035 [P] [US2] Write failing prefab instantiation tests for new ObjectIds and source-to-instance mapping in `Tests/Unit/PrefabObjectGraphSerializationTests.cpp`
- [X] T036 [P] [US2] Write failing prefab override tests for replace property, insert owned, remove owned, and move owned operations in `Tests/Unit/PrefabObjectGraphSerializationTests.cpp`
- [X] T037 [P] [US2] Write failing prefab variant/nested prefab diagnostic tests in `Tests/Unit/PrefabObjectGraphSerializationTests.cpp`

### Implementation for User Story 2

- [X] T038 [US2] Implement prefab document model and base prefab reference support in `Runtime/Engine/Serialize/PrefabDocument.h`
- [X] T039 [US2] Implement prefab composition and source-to-instance ObjectId mapping in `Runtime/Engine/Serialize/ObjectGraphInstantiator.h`
- [X] T040 [US2] Implement patch operations for replace property, insert owned, remove owned, move owned, add prefab instance, and remove object in `Runtime/Engine/Serialize/ObjectGraphDocument.h`
- [X] T041 [US2] Integrate prefab instance insertion with scene Object Graph save/load in `Runtime/Engine/Serialize/ObjectGraphSerializer.h`
- [X] T042 [US2] Add prefab deterministic golden fixtures in `Tests/Unit/Fixtures/ObjectGraph/`
- [X] T043 [US2] Run focused prefab tests through `cmake --build build --config Debug --target NullusUnitTests -- /m:1`

**Checkpoint**: User Story 2 works without depending on editor UI.

---

## Phase 5: User Story 3 - Preserve Asset References And Unknown Data (Priority: P3)

**Goal**: Preserve asset references, missing assets, unknown types, and raw unresolved data under editor-friendly policies.

**Independent Test**: A document with asset references, missing assets, and unknown object records can load with diagnostics and save again without losing the original IDs or raw property data.

### Tests for User Story 3

- [X] T044 [P] [US3] Write failing asset reference serialization and missing-asset preservation tests in `Tests/Unit/SceneObjectGraphSerializationTests.cpp`
- [X] T045 [P] [US3] Write failing unknown type preservation tests in `Tests/Unit/ObjectGraphDocumentTests.cpp`
- [X] T046 [P] [US3] Write failing load policy tests for editor preserve, runtime fail, missing asset fallback, and invalid reference handling in `Tests/Unit/SerializationDiagnosticTests.cpp`

### Implementation for User Story 3

- [X] T047 [US3] Implement `AssetReference` payloads with asset ID, expected type, and path hint in `Runtime/Engine/Serialize/ObjectGraphDocument.h`
- [X] T048 [US3] Implement load policies for unknown types, missing assets, and invalid references in `Runtime/Engine/Serialize/ObjectGraphInstantiator.h`
- [X] T049 [US3] Preserve raw unknown records and properties in reader/writer round-trips in `Runtime/Engine/Serialize/ObjectGraphReader.h` and `Runtime/Engine/Serialize/ObjectGraphWriter.h`
- [X] T050 [US3] Add diagnostics for unknown types, missing assets, and preserved raw data in `Runtime/Engine/Serialize/SerializationDiagnostic.h`
- [X] T051 [US3] Run focused preservation and diagnostics tests through `cmake --build build --config Debug --target NullusUnitTests -- /m:1`

**Checkpoint**: Editor-preserving policies protect recoverable data while runtime policies remain strict.

---

## Phase 6: Polish & Cross-Cutting Concerns

**Purpose**: Align docs, remove dead paths, and collect final evidence.

- [X] T052 [P] Update `Docs/Reflection/ReflectionWorkflow.zh-CN.md` and `Docs/Reflection/ReflectionWorkflow.en.md` with serialization metadata and Object Graph reflection expectations
- [X] T053 [P] Add or update developer notes for scene/prefab Object Graph format in `Docs/`
- [X] T054 Remove any obsolete CMake references to deleted serialization files in `Runtime/Engine/CMakeLists.txt` or related build configuration
- [X] T055 Run `rg "worldID|GetWorldID|SetWorldID|SerializedSceneData|SerializedActorData|SerializedComponentData|p_actorID|p_playing|m_playing" Runtime Project Tests` and resolve intentional leftovers
- [X] T056 Run `cmake --build build --config Debug --target NullusUnitTests ReflectionTest -- /m:1`
- [X] T057 Run `ctest --test-dir build -C Debug --output-on-failure -R NullusUnitTests`
- [X] T058 Run `.\build\bin\Debug\ReflectionTest.exe`
- [X] T059 Self-review changed Runtime, Project, Tests, and Docs files for generated-file edits, product runtime viability, missing diagnostics, and cross-platform claims
- [X] T060 Update `specs/018-object-serialization/quickstart.md` with final exact validation commands and known platform coverage

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies.
- **Foundational (Phase 2)**: Depends on Setup and blocks all user stories.
- **User Story 1 (Phase 3)**: Depends on Foundational and is the MVP.
- **User Story 2 (Phase 4)**: Depends on User Story 1 document and scene object graph infrastructure.
- **User Story 3 (Phase 5)**: Depends on User Story 1 document reader/writer and can proceed partly in parallel with User Story 2 after shared document APIs stabilize.
- **Polish (Phase 6)**: Depends on selected story scope completion.

### User Story Dependencies

- **User Story 1 (P1)**: Required first.
- **User Story 2 (P2)**: Requires Object Graph document, object IDs, and scene instantiation from US1.
- **User Story 3 (P3)**: Requires document reader/writer from US1; asset/unknown preservation can be implemented after base diagnostics exist.

### Within Each User Story

- Tests must be written before behavior implementation.
- Reflection input changes must be followed by MetaParser-backed build validation.
- Generated files under `Runtime/*/Gen/` must not be hand-edited.
- Product runtime/editor call sites must be repaired before each checkpoint build.

## Parallel Opportunities

- T002, T003, and T004 can run in parallel.
- T016 through T021 can be authored in parallel because they touch distinct test concerns.
- T034 through T037 can be authored in parallel.
- T044 through T046 can be authored in parallel.
- Documentation tasks T052 and T053 can run in parallel after implementation behavior is stable.

## Parallel Example: User Story 1

```text
Task: "Write Scene reflected root tests in Tests/Unit/SceneObjectGraphSerializationTests.cpp"
Task: "Write Object Graph validation tests in Tests/Unit/ObjectGraphDocumentTests.cpp"
Task: "Write deterministic golden-output test in Tests/Unit/SceneObjectGraphSerializationTests.cpp"
```

## Implementation Strategy

### MVP First

1. Complete Phase 1.
2. Complete Phase 2.
3. Complete User Story 1 only.
4. Validate scene save/load, deterministic output, diagnostics, `ReflectionTest`, and absence of legacy scene output.

### Incremental Delivery

1. Establish GUID and strong IDs.
2. Remove `worldID` and constructor identity/play coupling.
3. Add Object Graph scene save/load.
4. Add prefab documents and overrides.
5. Add asset reference and unknown data preservation.
6. Complete docs, dead-path removal, and final evidence.

## Notes

- This task list intentionally does not include a legacy runtime load adapter.
- If old scene migration becomes necessary, create a separate spec for an offline converter.
- Do not hand-edit generated reflection files.
- Keep `Editor` and `Game` buildable at every checkpoint.
