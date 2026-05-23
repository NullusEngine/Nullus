# Tasks: Asset Management System

**Input**: Design documents from `specs/026-asset-management-system/`
**Prerequisites**: `plan.md`, `spec.md`, `research.md`, `data-model.md`, `quickstart.md`

**Tests**: Required for behavior-changing work.

## Phase 1: Setup (Shared Infrastructure)

- [x] T001 Create `Runtime/Core/Assets/` source directory for shared asset identity and scan primitives
- [x] T002 [P] Create `specs/026-asset-management-system/contracts/source-asset-database.md` describing scan and lookup behavior

---

## Phase 2: Foundational (Blocking Prerequisites)

- [x] T003 [P] Add failing asset foundation tests in `Tests/Unit/AssetFoundationTests.cpp`
- [x] T004 Implement asset IDs and diagnostics in `Runtime/Core/Assets/AssetId.h` and `Runtime/Core/Assets/AssetDiagnostics.h`
- [x] T005 Implement meta sidecar read/write in `Runtime/Core/Assets/AssetMeta.h` and `Runtime/Core/Assets/AssetMeta.cpp`
- [x] T006 Implement asset path helpers in `Runtime/Core/Assets/AssetPath.h` and `Runtime/Core/Assets/AssetPath.cpp`
- [x] T007 Implement source asset scan and lookup in `Runtime/Core/Assets/SourceAssetDatabase.h` and `Runtime/Core/Assets/SourceAssetDatabase.cpp`
- [x] T008 Run targeted `NullusUnitTests` asset foundation tests and fix compile/runtime failures

---

## Phase 3: User Story 1 - Stable Asset Identity (Priority: P1) MVP

**Goal**: Scan source asset roots, create stable `.meta` GUIDs, and resolve assets by GUID/path.

**Independent Test**: `NullusUnitTests --gtest_filter=AssetFoundationTests.*`

- [x] T009 [US1] Preserve existing `.meta` importer settings during scan in `Runtime/Core/Assets/AssetMeta.cpp`
- [x] T010 [US1] Detect duplicate GUID aliases and emit diagnostics in `Runtime/Core/Assets/SourceAssetDatabase.cpp`
- [x] T011 [US1] Add rename-with-meta stability coverage in `Tests/Unit/AssetFoundationTests.cpp`

---

## Phase 4: User Story 2 - External Model Import (Priority: P2)

**Goal**: Convert glTF/GLB/FBX/OBJ through a common imported-scene representation and generate deterministic Nullus assets.

**Independent Test**: Fixture imports produce deterministic generated asset records and diagnostics.

- [x] T012 [P] [US2] Create `specs/026-asset-management-system/contracts/scene-import-pipeline.md` for glTF/GLB/FBX/OBJ import behavior
- [x] T013 [P] [US2] Define imported scene records in `Runtime/Rendering/Assets/ImportedScene.h`
- [x] T014 [US2] Define scene importer registry in `Runtime/Rendering/Assets/SceneImportPipeline.h` and `Runtime/Rendering/Assets/SceneImportPipeline.cpp`
- [x] T015 [US2] Add importer registry and deterministic sub-asset tests in `Tests/Unit/AssetImportPipelineTests.cpp`
- [x] T016 [US2] Add glTF importer contract tests for mesh, PBR material, external/embedded buffers/images, skeleton, animation, and morph data in `Tests/Unit/AssetImportPipelineTests.cpp`
- [x] T017 [US2] Add FBX importer contract tests for parser-exposed scene, mesh, material, texture, skeleton, animation, skin, and morph data in `Tests/Unit/AssetImportPipelineTests.cpp`
- [x] T018 [US2] Add OBJ importer contract tests for mesh, MTL material, texture, and unsupported animation/skeleton diagnostics in `Tests/Unit/AssetImportPipelineTests.cpp`
- [x] T019 [US2] Implement OBJ and FBX import adapters using the available parser path while preserving common `ImportedScene` output
- [x] T020 [US2] Implement or integrate a glTF 2.0 importer path and document unsupported extension diagnostics
- [x] T021 [US2] Add artifact manifest records for model-derived mesh, material, texture, skeleton, skin, animation, morph, model, and prefab outputs in `Runtime/Core/Assets/ArtifactManifest.h`

---

## Phase 5: User Story 3 - Prefab Authoring And Instancing (Priority: P2)

**Goal**: Treat `.prefab` as a first-class asset type and generate model prefabs from imported scenes.

**Independent Test**: Prefab source import, variant validation, model generated prefab creation, and instantiation preserve asset references and object ID mappings.

- [x] T022 [P] [US3] Create `specs/026-asset-management-system/contracts/prefab-pipeline.md` for source prefab, variant, instance, generated model prefab, and build behavior
- [x] T023 [US3] Extend source asset classification so `.prefab` files use asset type `Prefab` and importer `prefab` in `Runtime/Core/Assets/AssetMeta.h` and `Runtime/Core/Assets/AssetMeta.cpp`
- [x] T024 [US3] Add prefab classification coverage in `Tests/Unit/AssetFoundationTests.cpp`
- [x] T025 [US3] Define runtime prefab artifact records in `Runtime/Engine/Assets/PrefabAsset.h` and `Runtime/Engine/Assets/PrefabAsset.cpp`
- [x] T026 [US3] Add prefab pipeline tests in `Tests/Unit/AssetPrefabPipelineTests.cpp` for `.prefab` import, base prefab references, variant cycles, invalid override targets, and source-to-instance mapping
- [x] T027 [US3] Implement prefab import validation using `Runtime/Engine/Serialize` object graph diagnostics
- [x] T028 [US3] Define generated model prefab builder in `Runtime/Engine/Assets/ModelPrefabBuilder.h` and `Runtime/Engine/Assets/ModelPrefabBuilder.cpp`
- [x] T029 [US3] Add generated model prefab tests that map imported nodes to `TransformComponent`, `MeshFilter`, `MeshRenderer`, and `$asset` references
- [x] T030 [US3] Add diagnostics for skinned animation and morph runtime component capability gaps until runtime playback components exist
- [x] T031 [US3] Add editor-facing generated prefab state and read-only/variant/unpack policy notes to `Project/Editor/Assets/EditorAssetDatabase.h`

---

## Phase 6: User Story 4 - Incremental Reimport And Hot Reload (Priority: P3)

**Goal**: Track dependencies and stale state for source assets, imported artifacts, prefabs, and loaded runtime resources.

**Independent Test**: Changing one dependency marks only dependent assets stale.

- [x] T032 [US4] Add asset version records and dependency kinds in `Runtime/Core/Assets/AssetVersion.h`
- [x] T033 [US4] Add resolver state model in `Runtime/Core/Assets/AssetResolver.h`
- [x] T034 [US4] Persist import diagnostics in `Runtime/Core/Assets/ImportDiagnostics.h`
- [x] T035 [US4] Add dependency graph tests for source file, source GUID, artifact, path-to-GUID, prefab base, override target, importer version, postprocessor version, and target platform dependencies
- [x] T036 [US4] Add atomic artifact commit tests that preserve previous successful artifacts after failed import
- [x] T037 [US4] Integrate asset refresh scheduling with existing `Project/Editor/Core/AssetFileWatcher`
- [x] T038 [US4] Add hot reload policy tests for loaded mesh, material, texture, prefab, and scene references

---

## Phase 7: User Story 5 - Runtime Resolution And Builds (Priority: P4)

**Goal**: Load converted runtime artifacts and prefab graphs through build manifests.

**Independent Test**: A scene manifest resolves all reachable asset and prefab artifacts by GUID/sub-asset ID.

- [x] T039 [P] [US5] Create `specs/026-asset-management-system/contracts/runtime-manifest.md` for runtime manifest schema and resolution behavior
- [x] T040 [US5] Define runtime asset manifest records in `Runtime/Engine/Assets/RuntimeAssetDatabase.h` and `Runtime/Engine/Assets/RuntimeAssetDatabase.cpp`
- [x] T041 [US5] Add manifest unit coverage in `Tests/Unit/AssetManifestTests.cpp`
- [x] T042 [US5] Add dependency-closure tests for scenes, prefabs, generated model prefabs, nested prefabs, mesh/material/texture/skeleton/animation/morph artifacts
- [x] T043 [US5] Add runtime resolver tests proving packaged builds do not read `.gltf`, `.glb`, `.fbx`, `.obj`, `.meta`, or editor cache databases
- [x] T044 [US5] Add migration bridge tests proving existing path-based `AResourceManager` consumers still work while GUID-backed references are preferred

---

## Final Phase: Polish & Cross-Cutting Concerns

- [x] T045 Run `cmake --build build --config Debug --target NullusUnitTests -- /m:1`
- [x] T046 Run `ctest --test-dir build -C Debug --output-on-failure -R NullusUnitTests`
- [x] T047 Update implementation notes in `specs/026-asset-management-system/quickstart.md`
- [x] T048 Run a spec consistency pass over `spec.md`, `plan.md`, and `tasks.md`

---

## Phase 8: Acceptance Gaps From Latest Validation

**Goal**: Close the known gaps before the asset system can be called complete.

**Independent Test**: `ctest --test-dir build -C Debug --output-on-failure -R NullusUnitTests` passes with no failing tests.

- [x] T049 [US4] Add a regression test that proves `AssetFileWatcher` does not miss the first create event after `Start()` in `Tests/Unit/AssetFileWatcherTests.cpp`
- [x] T050 [US4] Fix Windows `ReadDirectoryChangesW` watcher startup readiness and first-event race in `Project/Editor/Core/AssetFileWatcher.cpp`
- [x] T051 [US4] Re-run `build\bin\Debug\NullusUnitTests.exe --gtest_filter=AssetFileWatcherTests.*` and confirm watcher events pass
- [x] T052 Re-run full `ctest --test-dir build -C Debug --output-on-failure -R NullusUnitTests` and only mark T046 complete after it passes
- [x] T053 Update implementation notes in `specs/026-asset-management-system/quickstart.md` with the watcher validation result

---

## Phase 9: Full Importer And Artifact Completion

**Goal**: Move model import from contract-level intermediate records to real runtime artifact payloads.

**Independent Test**: Import representative glTF/GLB/FBX/OBJ fixtures and resolve generated mesh/material/texture/model/prefab artifacts through `RuntimeAssetDatabase`.

- [x] T054 [US2] Add fixture-based glTF/GLB payload tests for buffers, bufferViews, accessors, indices, vertex streams, images, and embedded binary chunks in `Tests/Unit/AssetImportPipelineTests.cpp`
- [x] T055 [US2] Implement GLB container parsing and glTF accessor extraction in `Runtime/Rendering/Assets/SceneImportPipeline.cpp`
- [x] T056 [US2] Add runtime artifact writer contracts for mesh, material, texture, model, skeleton, skin, animation, morph, and prefab outputs in `specs/026-asset-management-system/contracts/scene-import-pipeline.md`
- [x] T057 [US2] Implement artifact payload writer and atomic commit staging in `Runtime/Core/Assets/ArtifactWriter.h` and `Runtime/Core/Assets/ArtifactWriter.cpp`
- [x] T058 [US2] Add OBJ MTL and texture dependency fixture tests in `Tests/Unit/AssetImportPipelineTests.cpp`
- [x] T059 [US2] Expand FBX parser adapter tests for hierarchy, material channels, texture paths, skeleton, animation, skin, and morph data exposed by the parser in `Tests/Unit/AssetImportPipelineTests.cpp`

---

## Phase 10: Prefab Editor Workflow Completion

**Goal**: Complete the user-facing prefab workflow on top of the object graph and generated model prefab foundation.

**Independent Test**: Create, instantiate, override, apply, revert, variant, and unpack prefab workflows preserve stable IDs and asset references.

- [x] T060 [US3] Add prefab create/save workflow tests using `Runtime/Engine/Serialize/ObjectGraphSerializer` in `Tests/Unit/AssetPrefabPipelineTests.cpp`
- [x] T061 [US3] Add prefab instance override apply/revert tests for stable source object IDs in `Tests/Unit/AssetPrefabPipelineTests.cpp`
- [x] T062 [US3] Implement prefab variant base-chain cycle and missing-base validation helpers in `Runtime/Engine/Assets/PrefabAsset.cpp`
- [x] T063 [US3] Add editor actions for generated model prefab variant creation and unpack policy in `Project/Editor/Assets/EditorAssetDatabase.h` and `Project/Editor/Assets/EditorAssetDatabase.cpp`
- [x] T064 [US3] Add nested prefab and unresolved `$asset` reference diagnostics coverage in `Tests/Unit/AssetPrefabPipelineTests.cpp`
- [x] T068 [US3] Define prefab editor operation result, override record, prefab stage state, variant request, and unpack result structs in `Project/Editor/Assets/PrefabEditorWorkflow.h`
- [x] T069 [US3] Add prefab editor workflow serialization tests for selected-root creation, multi-root rejection or wrapper policy, and stable object IDs in `Tests/Unit/PrefabEditorWorkflowTests.cpp`
- [x] T070 [US3] Implement create-prefab-from-selection service using `Serialize::ObjectGraphSerializer` in `Project/Editor/Assets/PrefabEditorWorkflow.cpp`
- [x] T071 [US3] Add prefab instantiation persistence tests proving scenes store prefab asset reference, instance root, source-to-instance map, and local patches in `Tests/Unit/PrefabEditorWorkflowTests.cpp`
- [x] T072 [US3] Implement prefab instance creation and scene attachment helpers in `Project/Editor/Assets/PrefabEditorWorkflow.cpp`
- [x] T073 [US3] Add override discovery tests for reflected property changes, added/removed/reordered components, added/removed/reordered child GameObjects, and removed objects in `Tests/Unit/PrefabEditorWorkflowTests.cpp`
- [x] T074 [US3] Implement deterministic override diff generation and patch normalization in `Runtime/Engine/Assets/PrefabAsset.h` and `Runtime/Engine/Assets/PrefabAsset.cpp`
- [x] T075 [US3] Add apply-selected-override tests for property, component, and child hierarchy patches targeting the nearest editable prefab layer in `Tests/Unit/PrefabEditorWorkflowTests.cpp`
- [x] T076 [US3] Implement apply-selected-override and apply-all transaction helpers with failed-import rollback in `Project/Editor/Assets/PrefabEditorWorkflow.cpp`
- [x] T077 [US3] Add revert-selected-override tests proving sibling overrides and nested overrides are preserved in `Tests/Unit/PrefabEditorWorkflowTests.cpp`
- [x] T078 [US3] Implement revert-selected-override and revert-all helpers in `Project/Editor/Assets/PrefabEditorWorkflow.cpp`
- [x] T079 [US3] Add prefab stage tests for open, dirty tracking, save, discard, generated-read-only rejection, and source prefab editability in `Tests/Unit/PrefabEditorWorkflowTests.cpp`
- [x] T080 [US3] Implement prefab stage state machine and save/discard commands in `Project/Editor/Assets/PrefabEditorWorkflow.cpp`
- [x] T081 [US3] Add prefab variant creation tests for source prefabs and generated model prefabs preserving `basePrefab` references in `Tests/Unit/PrefabEditorWorkflowTests.cpp`
- [x] T082 [US3] Implement create-editable-variant command and destination conflict handling in `Project/Editor/Assets/PrefabEditorWorkflow.cpp`
- [x] T083 [US3] Add nested prefab tests for nested asset references, nested override chains, cycle diagnostics, and missing nested prefab diagnostics in `Tests/Unit/PrefabEditorWorkflowTests.cpp`
- [x] T084 [US3] Implement nested prefab dependency extraction and cycle checks in `Runtime/Engine/Assets/PrefabAsset.cpp`
- [x] T085 [US3] Add unpack tests for source prefab instances and generated model prefab instances preserving resolved hierarchy and `$asset` references in `Tests/Unit/PrefabEditorWorkflowTests.cpp`
- [x] T086 [US3] Implement unpack-to-scene command that removes prefab dependency and writes ordinary scene-owned object records in `Project/Editor/Assets/PrefabEditorWorkflow.cpp`
- [x] T087 [US3] Add editor diagnostics tests for missing base prefab, missing nested prefab, invalid override target, unresolved `$asset`, and unknown editor-safe records in `Tests/Unit/PrefabEditorWorkflowTests.cpp`
- [x] T088 [US3] Implement prefab editor diagnostics aggregation for source import, stage edit, apply/revert, variant, and unpack operations in `Project/Editor/Assets/PrefabEditorWorkflow.cpp`
- [x] T089 [US3] Add dependency graph tests proving apply, revert, variant creation, unpack, and nested prefab edits mark dependent variants, scenes, and build manifests stale in `Tests/Unit/AssetDependencyPipelineTests.cpp`
- [x] T090 [US3] Wire prefab workflow dependency records into `Runtime/Core/Assets/AssetVersion.cpp` and `Project/Editor/Assets/PrefabEditorWorkflow.cpp`
- [x] T091 [US3] Add asset browser and inspector command-surface tests for create prefab, open prefab, create variant, apply/revert overrides, and unpack in `Tests/Unit/PrefabEditorWorkflowTests.cpp`
- [x] T092 [US3] Add editor command descriptors for prefab workflow actions in `Project/Editor/Assets/EditorAssetDatabase.h` and `Project/Editor/Assets/EditorAssetDatabase.cpp`

---

## Phase 11: Runtime Playback Capability Follow-Up

**Goal**: Replace static capability diagnostics with actual runtime playback support where the engine has components for it.

**Independent Test**: Imported skeleton, skin, animation, and morph artifacts instantiate with runtime components and no longer emit missing-capability diagnostics.

- [x] T065 [US2] Define runtime component requirements for skinned mesh playback and morph target playback in `specs/026-asset-management-system/spec.md`
- [ ] T066 [US2] Add generated model prefab tests for future skinned mesh, animation, and morph components in `Tests/Unit/AssetPrefabPipelineTests.cpp` (blocked until reflected runtime playback components exist; current coverage intentionally asserts capability diagnostics)
- [ ] T067 [US2] Implement runtime component mapping in `Runtime/Engine/Assets/ModelPrefabBuilder.cpp` after playback components exist

---

## Phase 12: Editor Compatibility Matrix

**Goal**: Define the exact reference behavior set that Nullus must match before implementation claims editor compatibility.

**Independent Test**: A compatibility matrix maps every required reference editor concept to supported, supported with Nullus-native naming, diagnostic-only, deferred, or out-of-scope status.

- [x] T093 [P] Create editor compatibility matrix in `specs/026-asset-management-system/contracts/editor-compatibility-matrix.md`
- [x] T094 [P] Record reference editor entry points for `AssetDatabase`, `AssetImporter`, `ModelImporter`, `TextureImporter`, `AssetPostprocessor`, `PrefabUtility`, `PrefabImporter`, and asset pack workflows in `specs/026-asset-management-system/research.md`
- [x] T095 [US2] Add editor compatibility test fixture plan for model, texture, material, prefab, variant, nested prefab, missing prefab, and pack metadata assets in `specs/026-asset-management-system/quickstart.md`
- [x] T096 Add `Tests/Unit/AssetDatabaseFacadeTests.cpp` for cross-cutting editor-compatible asset workflow contract tests

---

## Phase 13: AssetDatabase Facade

**Goal**: Provide `AssetDatabaseFacade` editor semantics over Nullus source assets, artifact manifests, dependency graph, and runtime manifests.

**Independent Test**: editor-compatible AssetDatabase facade tests cover GUID/path lookup, create/delete/copy/move/rename, refresh/import batching, search filters, labels, dependency queries, main/sub-asset loading, and pack metadata.

- [x] T097 [US1] Define editor-compatible AssetDatabase facade API in `Project/Editor/Assets/AssetDatabaseFacade.h`
- [x] T098 [US1] Add GUID/path lookup tests for `AssetPathToGUID`, `GUIDToAssetPath`, `LoadMainAssetAtPath`, `LoadAllAssetsAtPath`, and sub-asset lookup in `Tests/Unit/AssetDatabaseFacadeTests.cpp`
- [x] T099 [US1] Implement GUID/path lookup and main/sub-asset query bridge in `Project/Editor/Assets/AssetDatabaseFacade.cpp`
- [x] T100 [US1] Add create/delete/copy/move/rename/folder operation tests preserving `.meta` identity where the reference workflow preserves identity in `Tests/Unit/AssetDatabaseFacadeTests.cpp`
- [x] T101 [US1] Implement create/delete/copy/move/rename/folder operations and `.meta` handling in `Project/Editor/Assets/AssetDatabaseFacade.cpp`
- [x] T102 [US4] Add `Refresh`, `ImportAsset`, `StartAssetEditing`, and `StopAssetEditing` batching tests in `Tests/Unit/AssetDatabaseFacadeTests.cpp`
- [x] T103 [US4] Implement refresh/import batching semantics and queued import scheduling in `Project/Editor/Assets/AssetDatabaseFacade.cpp`
- [x] T104 [US5] Add dependency query tests for direct dependencies, recursive dependencies, and artifact/sub-asset dependencies in `Tests/Unit/AssetDatabaseFacadeTests.cpp`
- [x] T105 [US5] Implement dependency query bridge over `AssetDependencyGraph` and `ArtifactManifest` in `Project/Editor/Assets/AssetDatabaseFacade.cpp`
- [x] T106 [US1] Add search filter tests for name, type, label, folder scope, and deterministic result ordering in `Tests/Unit/AssetDatabaseFacadeTests.cpp`
- [x] T107 [US1] Implement editor-compatible search filters and label metadata in `Project/Editor/Assets/AssetDatabaseFacade.cpp` and `Runtime/Core/Assets/AssetMeta.h`
- [x] T108 [US5] Add asset-pack-style name and variant metadata tests mapped to Nullus asset packs in `Tests/Unit/AssetDatabaseFacadeTests.cpp`
- [x] T109 [US5] Implement pack name/variant metadata bridge in `Project/Editor/Assets/AssetDatabaseFacade.cpp` and `Runtime/Engine/Assets/RuntimeAssetDatabase.cpp`

---

## Phase 14: Asset Importer Facade

**Goal**: Align import settings, reimport, remaps, postprocessors, and model/texture importer semantics with reference workflows.

**Independent Test**: Importer facade tests cover importer lookup, serialized settings mutation, dirty state, `SaveAndReimport`, external object remaps, user data, version invalidation, platform overrides, postprocessors, and scripted/custom importer registration.

- [x] T110 [US2] Define editor-compatible importer facade records for importer settings, dirty state, remaps, user data, platform overrides, and diagnostics in `Project/Editor/Assets/AssetImporterFacade.h`
- [x] T111 [US2] Add `GetAtPath`, serialized settings mutation, dirty state, and `SaveAndReimport` tests in `Tests/Unit/AssetImporterFacadeTests.cpp`
- [x] T112 [US2] Implement importer lookup, settings serialization, dirty state, and `SaveAndReimport` bridge in `Project/Editor/Assets/AssetImporterFacade.cpp`
- [x] T113 [US2] Add external object remap tests for model material and texture remapping in `Tests/Unit/AssetImporterFacadeTests.cpp`
- [x] T114 [US2] Implement external object remap records in `Runtime/Core/Assets/AssetMeta.h` and `Project/Editor/Assets/AssetImporterFacade.cpp`
- [x] T115 [US2] Add ModelImporter setting tests for scale, axis/unit conversion, hierarchy policy, normals, tangents, UVs, material extraction, rig/skeleton, animation clips, and camera/light import policy in `Tests/Unit/AssetImporterFacadeTests.cpp`
- [x] T116 [US2] Implement ModelImporter-equivalent settings and mapping into `ImportedScene` conversion in `Runtime/Rendering/Assets/SceneImportPipeline.h` and `Runtime/Rendering/Assets/SceneImportPipeline.cpp`
- [x] T117 [US2] Add TextureImporter setting tests for texture type, sRGB, alpha, mipmaps, compression intent, sampler/wrap/filter, max size, and platform overrides in `Tests/Unit/AssetImporterFacadeTests.cpp`
- [x] T118 [US2] Implement TextureImporter-equivalent settings records in `Runtime/Core/Assets/AssetMeta.h` and `Project/Editor/Assets/AssetImporterFacade.cpp`
- [x] T119 [US2] Add AssetPostprocessor-equivalent tests for pre-import, post-import, dependency declaration, diagnostics, ordered callback execution, and version invalidation in `Tests/Unit/AssetImporterFacadeTests.cpp`
- [x] T120 [US2] Implement asset postprocessor registry and ordered callback execution in `Project/Editor/Assets/AssetImporterFacade.cpp`
- [x] T121 [US2] Add scripted/custom importer registration tests for custom extensions producing artifacts, dependencies, diagnostics, and sub-assets in `Tests/Unit/AssetImporterFacadeTests.cpp`
- [x] T122 [US2] Implement scripted/custom importer registry extension points in `Project/Editor/Assets/AssetImporterFacade.cpp` and `Runtime/Rendering/Assets/SceneImportPipeline.cpp`

---

## Phase 15: Prefab Utility Facade

**Goal**: Align Prefab type/status queries, save/connect, Prefab Stage, granular overrides, variants, nested prefabs, missing prefab handling, and unpack modes with reference workflows.

**Independent Test**: PrefabUtility facade tests cover regular prefab, model prefab, variant, nested prefab, missing prefab, Prefab Stage, property/component/object overrides, apply/revert levels, and unpack/unpack completely.

- [x] T123 [US3] Define editor-compatible prefab asset type, instance status, override type, unpack mode, and prefab operation result records in `Project/Editor/Assets/PrefabUtilityFacade.h`
- [x] T124 [US3] Add prefab type/status query tests for not prefab, regular prefab, model prefab, variant, missing asset, connected instance, disconnected/unpacked instance, and invalid/corrupt instance in `Tests/Unit/PrefabUtilityFacadeTests.cpp`
- [x] T125 [US3] Implement prefab type/status query bridge in `Project/Editor/Assets/PrefabUtilityFacade.cpp`
- [x] T126 [US3] Add save-as-prefab, save-and-connect, load-prefab-contents, save-prefab-contents, and unload-prefab-contents tests in `Tests/Unit/PrefabUtilityFacadeTests.cpp`
- [x] T127 [US3] Implement save/connect and prefab contents stage bridge in `Project/Editor/Assets/PrefabUtilityFacade.cpp`
- [x] T128 [US3] Add granular property override tests equivalent to reference editor property modifications in `Tests/Unit/PrefabUtilityFacadeTests.cpp`
- [x] T129 [US3] Implement property modification records with source object ID, instance object ID, property path, base value, local value, and owning prefab layer in `Runtime/Engine/Assets/PrefabAsset.cpp`
- [x] T130 [US3] Add component override tests for added, removed, reordered, apply, and revert component operations in `Tests/Unit/PrefabUtilityFacadeTests.cpp`
- [x] T131 [US3] Implement component override bridge and stable component ordering in `Runtime/Engine/Assets/PrefabAsset.cpp` and `Project/Editor/Assets/PrefabUtilityFacade.cpp`
- [x] T132 [US3] Add child GameObject override tests for added, removed, reordered, apply, and revert object operations in `Tests/Unit/PrefabUtilityFacadeTests.cpp`
- [x] T133 [US3] Implement child object override bridge and stable sibling ordering in `Runtime/Engine/Assets/PrefabAsset.cpp` and `Project/Editor/Assets/PrefabUtilityFacade.cpp`
- [x] T134 [US3] Add apply/revert tests for single override, selected object/component override group, and whole prefab instance in `Tests/Unit/PrefabUtilityFacadeTests.cpp`
- [x] T135 [US3] Implement apply/revert levels and nearest-editable-layer targeting in `Project/Editor/Assets/PrefabUtilityFacade.cpp`
- [x] T136 [US3] Add nested prefab tests for override chains, variant-on-nested-prefab, cycle rejection, and missing nested asset recovery in `Tests/Unit/PrefabUtilityFacadeTests.cpp`
- [x] T137 [US3] Implement nested prefab bridge and missing prefab recovery records in `Runtime/Engine/Assets/PrefabAsset.cpp` and `Project/Editor/Assets/PrefabUtilityFacade.cpp`
- [x] T138 [US3] Add unpack mode tests for preserving nested prefab links and unpacking completely in `Tests/Unit/PrefabUtilityFacadeTests.cpp`
- [x] T139 [US3] Implement unpack and unpack-completely modes in `Project/Editor/Assets/PrefabUtilityFacade.cpp`
- [x] T140 [US3] Add read-only model prefab tests proving scene overrides and variants are allowed but generated asset writes are rejected in `Tests/Unit/PrefabUtilityFacadeTests.cpp`
- [x] T141 [US3] Implement model prefab read-only enforcement in `Project/Editor/Assets/PrefabUtilityFacade.cpp` and `Project/Editor/Assets/PrefabEditorWorkflow.cpp`

---

## Phase 15A: PrefabUtility Review Remediation

**Goal**: Fix blocking review findings before claiming editor-compatible PrefabUtility semantics are safe enough to build on.

**Independent Test**: `NullusUnitTests --gtest_filter=PrefabUtilityFacadeTests.*:PrefabEditorWorkflowTests.*:AssetPrefabPipelineTests.*:PrefabObjectGraphSerializationTests.*`

- [x] T195 [US3] Add regression coverage for save-and-connect object identity, prefab stage unload, generated model prefab internal read-only enforcement, and live revert state in `Tests/Unit/PrefabUtilityFacadeTests.cpp`
- [x] T196 [US3] Replace facade-local model prefab registration with persisted `PrefabArtifact::generatedModelPrefab` state and generated model builder propagation in `Runtime/Engine/Assets/PrefabAsset.h`, `Runtime/Engine/Assets/ModelPrefabBuilder.cpp`, and `Project/Editor/Assets/PrefabUtilityFacade.cpp`
- [x] T197 [US3] Implement save-and-connect as connecting the existing scene object instead of instantiating a replacement and deleting the original in `Project/Editor/Assets/PrefabEditorWorkflow.cpp` and `Project/Editor/Assets/PrefabUtilityFacade.cpp`
- [x] T198 [US3] Make `UnloadPrefabContents` close and invalidate the prefab contents stage, while preserving `DiscardPrefabStage` as an edit discard operation in `Project/Editor/Assets/PrefabUtilityFacade.cpp`
- [x] T199 [US3] Revert selected/all scalar property overrides back into live prefab instances for covered GameObject state in `Project/Editor/Assets/PrefabEditorWorkflow.cpp`
- [x] T200 [US3] Add recursive child hierarchy override discovery and tests in `Project/Editor/Assets/PrefabEditorWorkflow.cpp` and `Tests/Unit/PrefabEditorWorkflowTests.cpp`
- [x] T201 [US3] Preserve variant `basePrefab` and `baseChain` when saving Prefab Stage edits in `Project/Editor/Assets/PrefabEditorWorkflow.cpp`
- [x] T202 [US3] Treat removed object records as inactive during validation and prefab instantiation, including owned subtree removal, in `Runtime/Engine/Serialize/ObjectGraphDocument.h`, `Runtime/Engine/Serialize/ObjectGraphInstantiator.h`, and `Project/Editor/Assets/PrefabEditorWorkflow.cpp`
- [x] T203 [US3] Implement full corresponding-source / nearest-editable-layer apply routing for nested prefab and variant layers instead of requiring the caller to pass the exact mutable artifact
- [x] T204 [US3] Implement real nested prefab instance records with independent instance roots, source mappings, and override chains rather than dependency-only `$asset` references
- [x] T205 [US3] Extend revert to restore structural live state for added/removed/reordered components and child GameObjects, not only currently covered scalar GameObject fields and patch bookkeeping
- [x] T206 [US3] Preserve full missing prefab recovery payloads, including added component/child object records, source-to-instance mappings, and local patch payloads
- [x] T207 [US3] Make `UnpackPrefabInstance(OutermostRoot)` retain nested prefab instance connections in scene state instead of only returning preserved nested prefab references

---

## Phase 16: Asset Pack Build Packaging And Runtime Loading

**Goal**: Map asset pack metadata to Nullus runtime artifact packs and enforce editor/runtime API separation.

**Independent Test**: Asset pack tests cover pack name/variant grouping, dependency closure, content hashes, runtime load by manifest, and rejection of editor-only APIs.

- [x] T142 [US5] Define asset pack records for pack name, variant, artifact entries, dependencies, hashes, and loader IDs in `Runtime/Engine/Assets/RuntimeAssetDatabase.h`
- [x] T143 [US5] Add asset pack build tests for pack name/variant grouping and dependency closure in `Tests/Unit/AssetDatabaseFacadeTests.cpp`
- [x] T144 [US5] Implement asset pack grouping and runtime manifest emission in `Runtime/Engine/Assets/RuntimeAssetDatabase.cpp`
- [x] T145 [US5] Add runtime load tests proving packaged assets resolve by manifest and editor-only AssetDatabase/importer APIs are unavailable in runtime mode in `Tests/Unit/AssetDatabaseFacadeTests.cpp`
- [x] T146 [US5] Implement runtime/editor API boundary checks in `Runtime/Engine/Assets/RuntimeAssetDatabase.cpp` and `Project/Editor/Assets/AssetDatabaseFacade.cpp`

---

## Phase 17: AssetDatabase And Prefab Semantics Completion

**Goal**: Close remaining editor-compatible API semantics that are needed for drag/drop, material extraction, and complete Prefab workflow parity.

**Independent Test**: editor compatibility tests cover create/extract/containment, dirty import setting writes, default overrides, and corresponding-source lookup.

- [x] T147 [US1] Add AssetDatabase create/add/extract/unique-path/containment tests in `Tests/Unit/AssetDatabaseFacadeTests.cpp`
- [x] T148 [US1] Implement create/add/extract/unique-path/containment bridge in `Project/Editor/Assets/AssetDatabaseFacade.cpp` and `Runtime/Core/Assets/AssetMeta.h`
- [x] T149 [US2] Add `WriteImportSettingsIfDirty` and importer dirty-persistence tests in `Tests/Unit/AssetImporterFacadeTests.cpp`
- [x] T150 [US2] Implement `WriteImportSettingsIfDirty` bridge in `Project/Editor/Assets/AssetImporterFacade.cpp` and `Runtime/Core/Assets/AssetMeta.cpp`
- [x] T151 [US3] Add editor compatibility fixture helpers for prefab source IDs, nested instances, generated model prefab bases, and material sub-assets in `Tests/Unit/PrefabUtilityFacadeTests.cpp`
- [x] T152 [US3] Add default prefab override classification tests for root transform/name/layer-style changes in `Tests/Unit/PrefabUtilityFacadeTests.cpp`
- [x] T153 [US3] Implement default override classification in `Runtime/Engine/Assets/PrefabAsset.cpp` and `Project/Editor/Assets/PrefabUtilityFacade.cpp`
- [x] T154 [US3] Add corresponding source, original source, nearest instance root, and outermost instance root lookup tests in `Tests/Unit/PrefabUtilityFacadeTests.cpp`
- [x] T155 [US3] Implement corresponding source and instance root lookup bridge in `Project/Editor/Assets/PrefabUtilityFacade.cpp`

---

## Phase 18: Editor Asset Browser And Hierarchy Drag/Drop

**Goal**: Support bidirectional drag/drop between assets and the Hierarchy for instantiation, material assignment, prefab creation, variants, and diagnostics.

**Independent Test**: `EditorAssetDragDropTests` simulate drag payloads and drop targets without a live UI and verify scenes/assets/dependencies are updated correctly.

- [x] T156 [P] [US6] Define editor drag payload, hierarchy drop target, drag operation kind, command result, and diagnostics records in `Project/Editor/Assets/AssetDragDropWorkflow.h`
- [x] T157 [US6] Add Asset Browser to Hierarchy prefab and generated model prefab drop tests in `Tests/Unit/EditorAssetDragDropTests.cpp`
- [x] T158 [US6] Implement prefab and generated model prefab instantiation drops using `PrefabEditorWorkflow` in `Project/Editor/Assets/AssetDragDropWorkflow.cpp`
- [x] T159 [US6] Add material asset to renderer/material-slot drop tests in `Tests/Unit/EditorAssetDragDropTests.cpp`
- [x] T160 [US6] Implement GUID-backed material assignment drops and undo/redo command descriptors in `Project/Editor/Assets/AssetDragDropWorkflow.cpp`
- [x] T161 [US6] Add texture asset drop tests for deterministic create-material-and-assign policy in `Tests/Unit/EditorAssetDragDropTests.cpp`
- [x] T162 [US6] Implement texture-to-material creation and assignment workflow in `Project/Editor/Assets/AssetDragDropWorkflow.cpp` and `Project/Editor/Assets/EditorAssetDatabase.cpp`
- [x] T163 [US6] Add invalid drop, read-only destination, generated artifact mutation, and missing renderer diagnostics tests in `Tests/Unit/EditorAssetDragDropTests.cpp`
- [x] T164 [US6] Add Hierarchy object to Asset Browser save-as-prefab tests in `Tests/Unit/EditorAssetDragDropTests.cpp`
- [x] T165 [US6] Implement Hierarchy-to-Asset-Browser save-as-prefab drop using `PrefabEditorWorkflow` in `Project/Editor/Assets/AssetDragDropWorkflow.cpp`
- [x] T166 [US6] Add connected prefab and generated model prefab instance to Asset Browser variant/unpacked-copy tests in `Tests/Unit/EditorAssetDragDropTests.cpp`
- [x] T167 [US6] Implement variant and unpacked-copy creation drops with deterministic destination conflict handling in `Project/Editor/Assets/AssetDragDropWorkflow.cpp`
- [x] T168 [US6] Add dirty scene/asset, dependency refresh, selection, and command-history tests for drag/drop operations in `Tests/Unit/EditorAssetDragDropTests.cpp`
- [x] T169 [US6] Wire drag/drop command descriptors into `Project/Editor/Assets/EditorAssetDatabase.h` and `Project/Editor/Assets/EditorAssetDatabase.cpp`

---

## Phase 19: Non-Blocking Import Progress And Cancellation

**Goal**: Ensure long asset imports do not freeze the editor and expose visible progress, diagnostics, and safe cancellation.

**Independent Test**: `AssetImportProgressTests` use fake long-running import phases to verify progress events, cancellation, rollback, and editor responsiveness contracts.

- [x] T170 [P] [US6] Define import job ID, import phase, progress event, batch progress, cancellation token, and terminal result records in `Project/Editor/Assets/ImportProgressTracker.h`
- [x] T171 [US6] Add import phase progress event tests for dependency copy, parse, conversion, artifact write, postprocess, and commit in `Tests/Unit/AssetImportProgressTests.cpp`
- [x] T172 [US6] Implement import progress tracker event recording and subscription in `Project/Editor/Assets/ImportProgressTracker.cpp`
- [x] T173 [US6] Add non-blocking import scheduling tests proving editor commands can run while fake imports advance in `Tests/Unit/AssetImportProgressTests.cpp`
- [x] T174 [US6] Integrate async import scheduling and progress publication with `Project/Editor/Assets/EditorAssetDatabase.cpp` and `Runtime/Rendering/Assets/SceneImportPipeline.cpp`
- [x] T175 [US6] Add cancellation and failed-import rollback tests that preserve the previous committed artifact in `Tests/Unit/AssetImportProgressTests.cpp`
- [x] T176 [US6] Implement cooperative cancellation and staged artifact cleanup in `Runtime/Core/Assets/ArtifactWriter.cpp` and `Project/Editor/Assets/ImportProgressTracker.cpp`
- [x] T177 [US6] Add Asset Browser import progress/status command-surface tests in `Tests/Unit/AssetImportProgressTests.cpp`

---

## Phase 20: Imported Material Visual Correctness

**Goal**: Convert glTF/FBX/OBJ materials into Nullus artifacts that preview and render with the correct supported visual intent.

**Independent Test**: `AssetMaterialConversionTests` verify channel mapping, color-space policy, sampler state, alpha behavior, diagnostics, and generated prefab material references; `AssetMaterialViewportTests` plus RenderDoc capture or deterministic screenshot validation prove generated model prefabs display the converted materials correctly.

- [x] T178 [P] [US6] Define material conversion input, texture slot, sampler, color-space policy, alpha mode, and diagnostics records in `Runtime/Rendering/Assets/MaterialConversion.h`
- [x] T179 [US6] Add glTF PBR material conversion tests for base color, metallic-roughness, normal, occlusion, emissive, alpha, UV transform, and sampler state in `Tests/Unit/AssetMaterialConversionTests.cpp`
- [x] T180 [US6] Implement glTF PBR material conversion in `Runtime/Rendering/Assets/MaterialConversion.cpp`
- [x] T181 [US6] Add FBX and OBJ MTL material conversion tests for parser-exposed channels and unsupported-channel diagnostics in `Tests/Unit/AssetMaterialConversionTests.cpp`
- [x] T182 [US6] Implement FBX and OBJ material conversion in `Runtime/Rendering/Assets/MaterialConversion.cpp` and `Runtime/Rendering/Assets/SceneImportPipeline.cpp`
- [x] T183 [US6] Add color-space, normal-map, sampler, alpha-mode, missing-texture, and unsupported-texture diagnostics tests in `Tests/Unit/AssetMaterialConversionTests.cpp`
- [x] T184 [US6] Implement material color-space, sampler, alpha, fallback, and diagnostics handling in `Runtime/Rendering/Assets/MaterialConversion.cpp`
- [x] T185 [US6] Add generated model prefab material artifact reference tests in `Tests/Unit/AssetPrefabPipelineTests.cpp`
- [x] T186 [US6] Wire generated model prefabs to converted material artifacts in `Runtime/Engine/Assets/ModelPrefabBuilder.cpp` and `Runtime/Rendering/Assets/SceneImportPipeline.cpp`
- [x] T187 [US6] Add editor preview and scene viewport material validation tests for generated model prefab fixtures in `Tests/Rendering/AssetMaterialViewportTests.cpp`
- [x] T188 [US6] Wire converted material artifacts into editor preview and scene viewport material binding in `Project/Editor/Assets/EditorAssetDatabase.cpp`, `Runtime/Engine/Assets/ModelPrefabBuilder.cpp`, and `Runtime/Engine/Rendering/SceneRendererFactory.h`
- [x] T189 [US6] Run RenderDoc capture or deterministic renderer screenshot validation for glTF/FBX/OBJ material fixtures and record evidence in `specs/026-asset-management-system/quickstart.md`

---

## Phase 21: Editor Compatibility Acceptance

**Goal**: Prove Nullus asset import and Prefab workflows are behaviorally aligned with the targeted reference workflow set after all editor-compatible implementation phases are complete.

**Independent Test**: All editor compatibility tests, asset pipeline tests, prefab workflow tests, importer tests, material viewport tests, and runtime manifest tests pass.

- [x] T190 Run `build\bin\Debug\NullusUnitTests.exe --gtest_filter=AssetDatabaseFacadeTests.*:AssetImporterFacadeTests.*:PrefabUtilityFacadeTests.*`
- [x] T191 Run asset pipeline regression tests `build\bin\Debug\NullusUnitTests.exe --gtest_filter=AssetFoundationTests.*:AssetImportPipelineTests.*:AssetPrefabPipelineTests.*:AssetDependencyPipelineTests.*:AssetManifestTests.*:PrefabEditorWorkflowTests.*:EditorAssetDragDropTests.*:AssetImportProgressTests.*:AssetMaterialConversionTests.*:AssetMaterialViewportTests.*`
- [x] T192 Update `specs/026-asset-management-system/contracts/editor-compatibility-matrix.md` with final supported/deferred/out-of-scope status for every editor-compatible behavior
- [x] T193 Run full `ctest --test-dir build -C Debug --output-on-failure -R NullusUnitTests`
- [x] T194 Update `specs/026-asset-management-system/quickstart.md` with editor compatibility validation steps and known intentional differences
- [x] T208 Harden source asset root validation so direct empty or filesystem-root scans fail closed in `Runtime/Core/Assets/SourceAssetDatabase.cpp`
- [x] T209 Harden runtime manifest artifact path validation so packaged builds reject absolute, traversal, and drive-rooted artifact paths in `Runtime/Engine/Assets/RuntimeAssetDatabase.cpp`
- [x] T210 Add final hardening regression coverage and record the 159-test asset regression plus full CTest evidence in `Tests/Unit/AssetFoundationTests.cpp`, `Tests/Unit/AssetManifestTests.cpp`, and `specs/026-asset-management-system/quickstart.md`
- [x] T211 Remove the legacy `Actor`/`ActorLoader`/`ActorManager` model-loading path and route editor model drops through generated prefab `GameObject` instantiation in `Project/Editor/Core/EditorActions.cpp`, `Project/Editor/Panels/SceneView.cpp`, and `Project/Editor/Assets/EditorAssetDragDropBridge.cpp`
- [x] T212 Add GameObject-centered model import regression coverage in `Tests/Unit/GameObjectAssetImportTests.cpp` and remove the old loader-specific scene insertion tests
- [x] T213 Wire Hierarchy-to-Asset-Browser folder drops to save selected `GameObject` hierarchies as `.prefab` source assets through `AssetDragDropWorkflow` in `Project/Editor/Panels/AssetBrowser.cpp`
- [x] T214 Rename editor-facing scene object creation, selection, hierarchy, inspector, shortcut, and startup-validation surfaces from Actor wording to GameObject wording while preserving existing serialized object graph compatibility
- [x] T215 Align imported model presentation with source-file-first Project Browser behavior: hide generated model prefab virtual `.prefab` entries while keeping direct model-file drag/drop instantiation through the internal prefab artifact.
- [x] T216 Key model artifact roots by source asset GUID instead of filename stem so same-stem formats such as `Sponza.gltf` and `Sponza.fbx` can coexist without overwriting each other.

---

## Phase 22: Connected Prefab Editing UX

**Goal**: Make prefab drag/drop, hierarchy presentation, prefab stage editing, and open-scene instance refresh behave like a connected prefab workflow instead of ordinary GameObject duplication.

**Independent Test**: Prefab UI workflow tests prove Asset Browser prefab drops create connected instances, Hierarchy rows expose prefab visual state, double-clicking a `.prefab` opens an isolated prefab edit stage, and saving the stage refreshes all connected instances while preserving local overrides.

- [x] T217 [US3] Add connected prefab drop persistence tests proving `prefabAssetId`, `prefabSubAssetKey`, source-to-instance mapping, selected instance root, and scene insertion survive Asset Browser to Hierarchy drops in `Tests/Unit/EditorAssetDragDropTests.cpp`.
- [x] T218 [US3] Add Hierarchy prefab presentation tests for prefab root, prefab child, local override, generated read-only model prefab, and missing prefab states in `Tests/Unit/PrefabEditorWorkflowTests.cpp`.
- [x] T219 [US3] Implement Hierarchy prefab presentation descriptors and row color tokens in `Project/Editor/Assets/PrefabEditorWorkflow.h`, `Project/Editor/Assets/PrefabEditorWorkflow.cpp`, and `Project/Editor/Panels/Hierarchy.cpp`.
- [x] T220 [US3] Add Asset Browser double-click tests proving `.prefab` files open a prefab stage instead of generic file preview and generated model prefab sources route to variant/unpack guidance in `Tests/Unit/PrefabEditorWorkflowTests.cpp`.
- [x] T221 [US3] Wire `.prefab` double-click and context Open commands through Prefab Stage loading in `Project/Editor/Panels/AssetBrowser.cpp` and `Project/Editor/Assets/PrefabUtilityFacade.cpp`.
- [x] T222 [US3] Add prefab stage save propagation tests proving all connected open-scene instances refresh from saved source prefab root properties and preserve local property overrides in `Tests/Unit/PrefabEditorWorkflowTests.cpp`.
- [x] T223 [US3] Implement open-scene connected instance refresh after prefab stage save for root/property updates in `Project/Editor/Assets/PrefabEditorWorkflow.cpp` and `Project/Editor/Assets/PrefabUtilityFacade.cpp`.
- [x] T224 [US3] Run focused prefab UX regressions and update `specs/026-asset-management-system/quickstart.md` with the connected prefab editing workflow.

---

## Phase 23: Prefab Stage Full Structural Editing Completion

**Goal**: Complete the remaining connected prefab edit-mode behavior beyond root/property propagation so prefab stage edits can add/remove/reorder components and child GameObjects, then synchronize those structural changes to every connected scene instance while preserving local overrides.

**Independent Test**: Prefab stage structural edit tests prove added/removed/reordered children and components propagate to all registered instances, local structural overrides stay intact, and generated model prefab stages remain read-only with clear variant/unpack guidance.

- [ ] T225 [US3] Add prefab stage structural save tests for added child GameObjects, removed child GameObjects, reordered children, added components, removed components, and reordered components in `Tests/Unit/PrefabEditorWorkflowTests.cpp`.
- [ ] T226 [US3] Extend `RefreshConnectedInstanceFromPrefab` to rebuild connected instance child/component structure from the saved prefab graph while preserving local property/component/child overrides in `Project/Editor/Assets/PrefabEditorWorkflow.cpp`.
- [ ] T227 [US3] Route Hierarchy create/delete/reparent and component add/remove actions to the active prefab stage scene when a prefab stage is open, instead of mutating the main scene in `Project/Editor/Core/EditorActions.cpp`, `Project/Editor/Panels/Hierarchy.cpp`, and `Project/Editor/Panels/Inspector.cpp`.
- [ ] T228 [US3] Add explicit Prefab Stage UI affordances for save, discard/close, breadcrumb/path display, generated read-only guidance, and variant/unpack commands in editor panels.
- [ ] T229 [US3] Add manual editor validation notes and automated regressions proving Prefab Stage structural edits propagate to multiple open-scene instances without losing local overrides.

---

## Phase 24: Native Mesh Artifact Cache Completion

**Goal**: Make imported `.nmesh` artifacts contain runtime-loadable internal mesh data instead of generic placeholder metadata, so model loads can skip source-format parsing after import.

**Independent Test**: Importing a valid glTF fixture writes a `.nmesh` with real vertex/index payload, and `ModelLoader` can load a `.nmesh` directly into a `Model`.

- [x] T230 [US2] Add native mesh artifact regression coverage in `Tests/Unit/AssetDatabaseFacadeTests.cpp` and `Tests/Unit/MeshBoundingSphereTests.cpp`.
- [x] T231 [US2] Define the Nullus native mesh artifact binary format and reader/writer in `Runtime/Rendering/Assets/MeshArtifact.h` and `Runtime/Rendering/Assets/MeshArtifact.cpp`.
- [x] T232 [US2] Expose parser mesh vertex/index data and write real `.nmesh` payloads during external model import in `Runtime/Rendering/Resources/Parsers/AssimpParser.*` and `Project/Editor/Assets/ExternalAssetImporter.cpp`.
- [x] T233 [US5] Route `.nmesh` runtime loads through `ModelLoader` without re-parsing glTF/GLB/FBX/OBJ source files.

---

## Phase 25: Usable Large-Asset Import Feedback And Startup Responsiveness

**Goal**: Make large model imports and editor startup visibly responsive, with accurate current-task progress and no eager decoding of every texture in the asset tree.

**Independent Test**: Importing a model with external textures writes material artifacts that point to runtime-loadable texture paths, model import publishes parse/convert/write/commit progress, stale native mesh caches are invalidated by importer versioning, and the asset browser does not synchronously load texture previews while building the file tree.

- [x] T234 [US2] Add material conversion coverage proving glTF image URIs serialize as runtime resource paths instead of internal `image/N` keys in `Tests/Unit/AssetMaterialConversionTests.cpp`.
- [x] T235 [US2] Resolve imported material texture references relative to the source model folder and write them into `.nmat` uniforms in `Runtime/Rendering/Assets/MaterialConversion.cpp` and `Project/Editor/Assets/ExternalAssetImporter.cpp`.
- [x] T236 [US2] Bump the model-scene importer version so old placeholder `.nmesh` artifact caches are automatically invalidated on refresh in `Runtime/Core/Assets/AssetMeta.cpp` and `Runtime/Core/Assets/SourceAssetDatabase.cpp`.
- [x] T237 [US6] Thread import progress events through `AssetDatabaseFacade` and the external model importer so source parse, conversion, mesh cache build, prefab build, artifact write, and commit are visible in `Project/Editor/Assets/*`.
- [x] T238 [US6] Draw an import progress status bar in the Asset Browser using the shared editor import tracker in `Project/Editor/Panels/AssetBrowser.cpp`.
- [x] T239 [US6] Start copied external asset imports on a background worker after file selection/copy so the main editor frame can keep drawing progress in `Project/Editor/Core/EditorActions.cpp`.
- [x] T240 [US6] Make texture previews lazy-loaded on hover instead of synchronously decoding every texture while the Asset Browser tree is built in `Project/Editor/Panels/AssetBrowser.cpp`.
- [x] T241 [US6] Add a startup asset refresh/import bootstrapper that scans project and built-in read-only roots incrementally, reports progress before panels finish first paint, and defers heavy import work until after the first editor frame.
- [x] T242 [US6] Treat built-in assets as read-only package sources with prebuilt or lazily generated Library artifacts: ship stable `.meta` identities with engine assets, keep imported artifacts under the project/library cache, and never write generated `.prefab` files into built-in asset folders.
- [x] T243 [US6] Add regression coverage proving built-in assets mount as read-only, import lazily or from prebuilt artifacts, and startup progress reports the current scanned/imported file without blocking normal editor commands.

**Phase 25 implementation note**: Built-in model source paths now resolve in `ModelManager` through project `Library/BuiltinArtifacts` first, bundled read-only artifacts second, and lazy single-mesh `.nmesh` generation into the project library as a fallback. The editor restores the startup scene after the first frame through the shared import progress tracker. Full multi-mesh imported model scenes continue to use the generated model/prefab import pipeline rather than the primitive built-in mesh cache.

---

## Phase 26: Central Artifact And Shader Cache Indexes

**Goal**: Add durable central indexes for imported artifact records and shader compile records so editor startup, dependency checks, cache inspection, and future incremental import scheduling do not need to scan every per-asset artifact folder.

**Independent Test**: Artifact database tests prove source/sub-asset/status lookup survives save/load; asset facade import tests prove successful model imports update `Library/ArtifactDB/index.tsv`; shader cache tests prove compile success/failure diagnostics persist to `Library/ShaderCache/ShaderCache.tsv`.

- [x] T244 [US2] Define and test `ArtifactDatabase` central source/sub-asset/status index persistence in `Runtime/Core/Assets/ArtifactDatabase.*` and `Tests/Unit/AssetImportPipelineTests.cpp`.
- [x] T245 [US2] Write successful asset manifests into `Library/ArtifactDB/index.tsv` from the shared `AssetDatabaseFacade::AddArtifactManifest` path, using project-relative artifact paths for stable cache records.
- [x] T246 [US6] Define and test `ShaderCacheDatabase` persistence for successful bytecode artifacts and failed diagnostics in `Runtime/Rendering/ShaderCompiler/ShaderCacheDatabase.*` and `Tests/Unit/ShaderCompilerTests.cpp`.
- [x] T247 [US6] Add configurable shader cache DB persistence to `ShaderCompiler` and route `ShaderLoader` project shader compiles into `Library/ShaderCache/ShaderCache.tsv`.
- [x] T248 [US6] Add same-process concurrent write protection for central artifact and shader cache DB writes so batch imports/compiles preserve every source and stage record.
- [ ] T249 [US6] Use the central artifact index during startup refresh to avoid opening every per-asset manifest before dependency or stale-cache checks need it.
- [ ] T250 [US6] Add editor diagnostics/inspection UI for artifact index status, failed imports, shader compile errors, and cache invalidation reasons.

---

## Phase 27: Asset Pipeline Usability Fixes

**Goal**: Close the editor-facing usability gaps found during large model import: startup work must show global progress, explicit reimport must refresh stale native mesh artifacts, and generated model prefab drops must instantiate renderable mesh/material artifact paths.

**Independent Test**: Focused asset progress, reimport, importer facade, and GameObject drag/drop tests prove startup progress is visible outside the Asset Browser, stale `.nmesh` payloads are replaced by reimport, and dragged generated model prefabs point renderer components at loadable internal artifacts.

- [x] T251 [US6] Add global import progress presentation coverage for startup-scene work in `Tests/Unit/AssetImportProgressTests.cpp`.
- [x] T252 [US6] Draw active import/startup progress in the editor status bar and share the same presentation model with the Asset Browser in `Project/Editor/Panels/EditorStatusBar.cpp`, `Project/Editor/Panels/AssetBrowser.cpp`, and `Project/Editor/Assets/EditorAssetDatabase.*`.
- [x] T253 [US2] Add forced reimport coverage proving stale `.nmesh` artifacts are overwritten when source model content changes in `Tests/Unit/AssetDatabaseFacadeTests.cpp`.
- [x] T254 [US2] Implement `AssetDatabaseFacade::ReimportAsset` and route `AssetImporterFacade::SaveAndReimport` through the real import path so committed artifact folders and central manifests refresh immediately.
- [x] T255 [US3] Resolve generated prefab mesh/material asset references to committed artifact paths before instantiation so dragging model files into the Hierarchy creates renderable `GameObject` instances in `Runtime/Engine/Assets/PrefabAsset.cpp` and `Tests/Unit/GameObjectAssetImportTests.cpp`.
- [x] T256 [US6] Split startup-scene progress across a visible pre-load frame and guarded completion so scene/model/shader startup stalls show the current operation in the global status bar.
- [x] T257 [US2] Make model reimport artifact publishing transactional, with rollback coverage for manifest save failures so stale `.nmesh` refresh never leaves half-published internal assets.
- [x] T258 [US3] Accelerate generated model drop resource realization by separating mesh decode scheduling from main-thread bind work, increasing bounded in-flight native mesh loads, and making validation readback wait until deferred renderer resource jobs finish.

---

## Phase 27A: Shader Source Artifact Pipeline

**Goal**: Treat `.hlsl` files as first-class source assets whose compiled variants are imported before editor hot paths use them.

**Independent Test**: Asset database tests import `.hlsl` files and verify `.nshader` artifacts, `manifest.json`, `ArtifactDB/index.tsv`, startup preimport planning, and artifact-loader resolution without requiring runtime source compilation.

- [ ] T258A [US6] Add failing tests proving `AssetDatabaseFacade::ImportAsset` imports `.hlsl` into GUID-keyed `Library/Artifacts` shader artifacts and central `ArtifactDB`.
- [ ] T258B [US6] Implement shader artifact payload serialization/deserialization for `.nshader` stage records under the rendering shader loader boundary.
- [ ] T258C [US6] Implement `.hlsl` import in `AssetDatabaseFacade::RefreshSingle`, including source/meta/importer/build-target dependencies and atomic artifact manifest commit.
- [ ] T258D [US6] Extend startup/file-change preimport scheduling to include shader source assets and skip unchanged shader manifests.
- [ ] T258E [US6] Extend `ShaderLoader` to load `.nshader` artifacts directly and preserve legacy `.hlsl` source compilation as a fallback.
- [ ] T258F [US6] Add material shader-reference coverage for artifact-handle loading while preserving legacy `:Shaders/*.hlsl` payload compatibility.

---

## Phase 28: Main Model Runtime Cache And Native Model Package

**Goal**: Make imported model drops instantiate from a loaded main model asset instead of resolving dozens or hundreds of mesh sub-assets one-by-one after the hierarchy object is created.

**Independent Test**: Dragging an already-imported large model into the scene creates the connected generated prefab immediately, references a loaded main model cache entry, submits visible drawables within the first validation window, and does not schedule per-mesh disk decode jobs unless the main model package is missing or invalid.

- [x] T259 [US2] Replace placeholder `model.nmodel` metadata with a native model package/index containing mesh chunk records, material binding records, local bounds, artifact content hashes, and offsets or paths for every runtime-loadable mesh payload.
- [x] T260 [US2] Add a model-level runtime loader/cache that loads or memory-maps `model.nmodel`, creates shared `Model`/`Mesh` runtime resources for the source main asset, and registers submesh aliases so existing prefab mesh references can reuse the same loaded objects.
- [x] T261 [US3] Update generated model prefab construction to reference the main model package for drag/runtime realization while preserving stable mesh and material sub-asset identities for inspection, overrides, and dependency tracking.
- [ ] T262 [US6] Prewarm imported model runtime cache opportunistically after import completion, asset selection, or editor idle, with progress/cancellation routed through the global import progress popup. Startup blocking preimport now warms `.nmodel`, mesh aliases, and `.nmat` artifacts before the UI opens; import-completion, selection, and idle-triggered prewarm remain follow-up work.
- [x] T263 [US2] Keep generated model drops immediately visible while native resources stream in: generated prefab instances must bind a non-blocking fallback material first, preserve real material path hints, and replace slots only when real materials are already cached or loaded by the asset-resolution queue.
- [x] T264 [US3] Add Scene View validation/readback and focused performance tests proving a warm imported model drop does not synchronously parse source files or schedule one background load per mesh sub-asset.
- [ ] T265 [US2] Add cache invalidation coverage proving model package changes, mesh artifact hash changes, material changes, and source reimport update or evict loaded main model resources without leaving stale scene instances.

---

## Phase 29: Unity-Style Imported Asset Drag Handles

**Goal**: Align Scene View/Hierarchy model drops with reference editor object-reference drag/drop: Project Browser drags provide imported asset handles, cold assets never import synchronously on drop, and warm model drops instantiate from committed artifacts while renderer resources stay deferred.

**Independent Test**: `build\bin\Debug\NullusUnitTests.exe --gtest_filter=GameObjectAssetImportTests.*EditorAssetHandle*:EditorAssetDragDropTests.*`

- [x] T266 [US6] Add regression coverage proving cold model asset handle drops return pending without mutating scenes or writing artifacts in `Tests/Unit/GameObjectAssetImportTests.cpp`.
- [x] T267 [US6] Define a trivially-copyable editor asset drag payload for Project Browser to Scene View/Hierarchy transfers in `Project/Editor/Assets/EditorAssetDragPayload.h` and `Project/Editor/Assets/EditorAssetDragPayload.cpp`.
- [x] T268 [US6] Add bridge APIs that drop imported asset handles without synchronous `Refresh`, `ImportAsset`, source parsing, or renderer payload decoding in `Project/Editor/Assets/EditorAssetDragDropBridge.h` and `Project/Editor/Assets/EditorAssetDragDropBridge.cpp`.
- [x] T269 [US6] Route Asset Browser file drag sources to publish imported asset handles while preserving legacy raw file payloads for folder/file management in `Project/Editor/Panels/AssetBrowser.cpp`.
- [x] T270 [US6] Route Scene View and Hierarchy drop targets to prefer imported asset handles and fall back to raw file payloads only for legacy scene/file workflows in `Project/Editor/Panels/SceneView.cpp` and `Project/Editor/Panels/Hierarchy.cpp`.
- [x] T271 [US6] Add warm imported model handle coverage proving generated prefab instances are created from committed artifacts without synchronous import fallback in `Tests/Unit/GameObjectAssetImportTests.cpp`.
- [x] T272 [US6] Run focused drag/drop and editor target validation, then record the Unity-style drag handle behavior in `specs/026-asset-management-system/quickstart.md`.

---

## Phase 30: Unity-Style Background Preimport

**Goal**: Make project model/prefab assets hot before drag/drop by scheduling background preimport on editor startup, project asset file changes, and copy/move/import into `Assets`, while keeping drop callbacks free of synchronous import work.

**Independent Test**: `build\bin\Debug\NullusUnitTests.exe --gtest_filter=EditorAssetDatabaseTests.Preimport*:EditorAssetDatabaseTests.FileWatcherPreimport*:AssetFileWatcherTests.ConsumeChangedPathsReturnsCreatedAssetPath`

- [x] T273 [US6] Add preimport planning tests proving cold model/prefab assets are planned, warm artifacts are skipped on startup, watcher changes reimport warm scene assets, and changed-path watcher requests avoid unrelated model/prefab assets in `Tests/Unit/EditorAssetDatabaseTests.cpp`.
- [x] T274 [US6] Extend `AssetFileWatcher` to expose concrete changed paths while preserving existing boolean change consumption in `Project/Editor/Core/AssetFileWatcher.*` and `Tests/Unit/AssetFileWatcherTests.cpp`.
- [x] T275 [US6] Implement `AssetPreimportScheduler` requests with startup cold-only import, watcher/copy forced reimport, and changed-path filtering in `Project/Editor/Assets/EditorAssetDatabase.*`.
- [x] T276 [US6] Persist prefab source import manifests and runtime prefab payloads under project `Library` so refreshed database instances classify imported prefabs as warm in `Project/Editor/Assets/AssetDatabaseFacade.cpp`.
- [x] T277 [US6] Wire Project Browser watcher startup, project file changes, folder copy/move drops, and external import/copy actions to schedule background preimport in `Project/Editor/Panels/AssetBrowser.cpp` and `Project/Editor/Core/EditorActions.cpp`.
- [x] T278 [US6] Run focused preimport/watcher tests plus editor/unit build validation and record the evidence in `specs/026-asset-management-system/quickstart.md`.
- [x] T279 [US2] Remove duplicate Assimp source parsing for FBX/OBJ imports by sharing parsed mesh data between `ImportedScene` construction and `.nmesh` artifact writing in `Runtime/Rendering/Resources/Parsers/*`, `Runtime/Rendering/Assets/SceneImportPipeline.*`, and `Project/Editor/Assets/ExternalAssetImporter.cpp`.
- [x] T280 [US2] Add FBX/OBJ single-parse regression coverage proving FBX import does not run the second native mesh cache phase and still writes loadable `.nmesh` payloads in `Tests/Unit/AssetImportPipelineTests.cpp` and `Tests/Unit/AssetDatabaseFacadeTests.cpp`.

---

## Phase 31: Imported Material And Mesh Artifact Fidelity

**Goal**: Ensure imported model artifacts are self-contained and faithful enough for Unity-style warm drag/drop: converted materials must include parser-exposed texture/factor channels, native mesh artifacts must preserve the source mesh identity used by stable sub-asset keys, and stale white-material caches must be invalidated.

**Independent Test**: Focused material/import tests prove FBX/OBJ parser material channels become `.nmat` uniforms, glTF native mesh artifacts keep UVs and source mesh ordering, and importer descriptors share the current model importer version.

- [x] T281 [US2] Extend the Assimp parser detailed scene provider so FBX/OBJ imports expose diffuse/base color, normal/bump, opacity, shininess, roughness, metallic, occlusion, emissive, specular, and double-sided material channels with texture URIs in `Runtime/Rendering/Resources/Parsers/AssimpParser.*`.
- [x] T282 [US2] Convert parser-exposed FBX/OBJ material channels into `.nmat` texture uniforms and factors, including independent metallic and roughness texture slots with packed metallic-roughness fallback, in `Runtime/Rendering/Assets/MaterialConversion.cpp`.
- [x] T283 [US2] Add explicit source mesh identity to native parser mesh data and use source keys instead of traversal vector indices when writing `.nmesh` artifacts in `Runtime/Rendering/Resources/Parsers/IModelParser.h`, `Runtime/Rendering/Resources/Parsers/AssimpParser.cpp`, and `Project/Editor/Assets/ExternalAssetImporter.cpp`.
- [x] T284 [US2] Write glTF/GLB native mesh artifacts from glTF accessor/buffer data keyed by original mesh index, preserving positions, indices, UVs, normals, tangents, and multi-primitive merges without relying on Assimp traversal order in `Project/Editor/Assets/ExternalAssetImporter.cpp`.
- [x] T285 [US2] Bump the model importer version and align default scene importer descriptors so old placeholder or white-material artifacts are marked stale and regenerated in `Runtime/Core/Assets/AssetMeta.cpp` and `Runtime/Rendering/Assets/SceneImportPipeline.cpp`.
- [x] T286 [US2] Add regression coverage for parser material texture channels, serialized `.nmat` texture uniforms, source mesh key/payload alignment, glTF UV preservation, multi-primitive native mesh merging, and importer descriptor version alignment in `Tests/Unit/AssetImportPipelineTests.cpp` and `Tests/Unit/AssetMaterialConversionTests.cpp`.
- [x] T287 [US2] Store imported texture sub-assets as `.ntex` payload artifacts carrying source or embedded image bytes, URI, MIME, bufferView, embedded flag, and byte length metadata in `Project/Editor/Assets/ExternalAssetImporter.cpp`.
- [x] T288 [US2] Resolve converted `.nmat` sampler uniforms to committed `.ntex` artifact paths instead of source image paths, and teach `TextureLoader` to decode `.ntex` payloads through the normal `TextureManager` path in `Runtime/Rendering/Assets/MaterialConversion.*`, `Runtime/Rendering/Resources/Loaders/TextureLoader.cpp`, and `Runtime/Base/Image.*`.
- [x] T289 [US6] Repair prewarmed generated model materials whose declared sampler uniforms were intentionally left unloaded during startup prewarm, while keeping deferred texture loading free of false failure warnings in `Project/Editor/Core/EditorActions.cpp` and `Runtime/Rendering/Resources/Loaders/MaterialLoader.cpp`.
- [x] T290 [US2] Add regression coverage proving imported texture artifacts carry real bytes, converted materials reference `.ntex` artifacts, `.ntex` loads as a runtime texture, deferred material prewarm does not warn, and loaded material sampler uniforms bind real `Texture2D` resources in `Tests/Unit/AssetImportPipelineTests.cpp` and `Tests/Unit/AssetMaterialConversionTests.cpp`.

## Dependencies & Execution Order

- Phase 1 and Phase 2 must complete before any importer or prefab work.
- User Story 1 is the MVP and blocks stable sub-asset IDs.
- User Story 2 depends on User Story 1.
- User Story 3 depends on User Story 1 and can proceed alongside importer work after `ImportedScene` records exist.
- User Story 4 depends on source identity, imported artifacts, and prefab dependency records.
- User Story 5 depends on dependency and artifact data from earlier phases.
- User Story 6 depends on User Stories 1-3 for asset identity, import records, prefab instancing, and material artifact references; its import progress slice can start once artifact staging exists.
- Generated model prefab playback for skeletal animation and morph targets depends on explicit runtime component capability support.
- Full editor compatibility acceptance depends on Phase 17 through Phase 20 plus all earlier asset pipeline, prefab workflow, importer, material viewport, and runtime manifest tests.
- Phase 26 builds on Phase 24 and Phase 25: native mesh/material artifacts and progress-aware import must exist before the central cache indexes can be trusted as startup acceleration inputs.
- Phase 28 builds on Phase 24, Phase 26, and Phase 27: native mesh payloads, central artifact lookup, and working connected generated prefab drops must exist before the main model package becomes the primary drag/runtime loading path.
- Phase 29 can proceed before the full native model package: it changes the editor drag/drop contract so source import and renderer payload decoding are kept off the drop callback immediately, while Phase 28 continues the deeper warm-cache optimization.
- Phase 30 builds directly on Phase 29: imported asset handles stay non-blocking, and background preimport is what normally converts cold model/prefab source files into warm handles before drag/drop.
- Phase 31 tightens the artifact payload fidelity required by Phases 28-30: startup preimport and warm drag/drop can only skip source parsing safely when `.nmodel`, `.nmesh`, and `.nmat` payloads are keyed to the same source-local identities.
