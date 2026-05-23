# Feature Specification: MetaParser Resource Reflection

**Feature Branch**: `030-metaparser-resource-reflection`
**Created**: 2026-05-22
**Status**: Implementation Review
**Input**: User description: "所有反射 class 必须走 MetaParser；改掉 Mesh/Material/Texture/Shader 等手写 StaticMetaTypeName/GetObjectTypeName 字符串写法"

**Scope Note**: Final implementation also includes the asset/runtime/render integration needed to preserve resource references after the reflection migration: runtime asset manifest loading, material artifact prewarming, editor artifact database indexing, object-graph resource `PPtr` resolution, and FrameGraph material-binding safety checks.

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Generated Resource Type Identity (Priority: P1)

Engine developers need Nullus-owned rendering resource classes to receive their reflected object identity from the normal MetaParser pipeline, so resource types behave like other reflected engine classes and do not maintain duplicated handwritten type-name strings.

**Why this priority**: This removes the current source of duplicated string identity and aligns resource objects with the repository rule that owned reflected classes must use generated reflection code.

**Independent Test**: A focused reflection test can scan the rendering resource headers and generated outputs to prove Mesh, Material, Shader, Texture, Texture2D, and TextureCube use `CLASS`/`GENERATED_BODY()` and no longer define handwritten `StaticMetaTypeName()` or `GetObjectTypeName()` bridges.

**Acceptance Scenarios**:

1. **Given** a Nullus-owned reflected rendering resource class, **When** its header is inspected, **Then** the class declaration uses the MetaParser reflection macros and contains `GENERATED_BODY()`.
2. **Given** a Nullus-owned reflected rendering resource class, **When** its header is inspected, **Then** it does not contain a handwritten `StaticMetaTypeName()` implementation or handwritten `GetObjectTypeName()` override.
3. **Given** the normal reflection generation flow runs, **When** generated rendering reflection outputs are inspected, **Then** generated headers and sources contain registrations for Mesh, Material, Shader, Texture, Texture2D, and TextureCube.

---

### User Story 2 - Preserve Existing Resource Reflection Behavior (Priority: P1)

Runtime and editor systems need existing resource reflection behavior to remain stable after migration, including `GetType()`, `PPtr<T>` type names, pointer type registration, object-graph serialization, and editor object-reference classification.

**Why this priority**: The migration is only acceptable if it removes manual code without changing resource identity or breaking existing serialization/editor contracts.

**Independent Test**: Existing PPtr, object graph, reflected drawer, and reflection runtime tests can run unchanged and continue to observe the same stable resource type names.

**Acceptance Scenarios**:

1. **Given** a Mesh, Material, Shader, Texture, Texture2D, or TextureCube instance, **When** `GetType()` is called, **Then** the returned type is valid and has the same stable qualified name as before the migration.
2. **Given** `PPtr<ResourceType>` is used for each migrated resource type, **When** its static meta type name is queried, **Then** the name remains `NLS::Engine::Serialize::PPtr<qualified-resource-type>`.
3. **Given** pointer and const-pointer resource types are resolved through reflection, **When** the reflection database is queried, **Then** each migrated resource pointer type remains registered and decays to the correct resource type where applicable.

---

### User Story 3 - Keep External Reflection For External Types Only (Priority: P2)

Maintainers need the external reflection module to stop manually registering Nullus-owned resource class shells while still registering truly external/value types such as rendering `Bounds`.

**Why this priority**: This makes ownership boundaries explicit and prevents duplicate registrations between generated and external reflection paths.

**Independent Test**: A focused source-contract test can verify `Runtime/Rendering/ExternalReflection.h` no longer contains resource `NLS_META_EXTERNAL_TYPE_NAME` declarations or `RegisterResourceReferenceType` calls for migrated resource classes, while still preserving external `Bounds` reflection.

**Acceptance Scenarios**:

1. **Given** `Runtime/Rendering/ExternalReflection.h`, **When** it is inspected, **Then** it does not manually declare or register Mesh, Material, Shader, Texture, Texture2D, or TextureCube resource class reflection.
2. **Given** `Runtime/Rendering/ExternalReflection.h`, **When** it is inspected, **Then** external/value type reflection that is not owned as a reflected class remains intact.

---

### User Story 4 - Runtime Asset Materialization (Priority: P1)

Packaged game startup must materialize material artifacts from `RuntimeAssetManifest.json` into `MaterialManager` before render synchronization asks components for already-registered material handles.

**Why this priority**: `MeshRenderer` deliberately avoids synchronous cold loads during render-scene sync. Without a runtime materialization owner, valid material references can fall back to default materials until another path happens to load them.

**Independent Test**: A runtime startup test can load a manifest containing material and non-material entries and prove only material artifact paths are passed to `MaterialManager::LoadArtifactWithoutTextures`.

**Acceptance Scenarios**:

1. **Given** a runtime manifest with material entries, **When** game context provides resource managers and the runtime asset database, **Then** material artifacts are prewarmed outside the render path.
2. **Given** mesh, shader, texture, or prefab entries in the same manifest, **When** material prewarm runs, **Then** those entries are not loaded through `MaterialManager`.

---

### User Story 5 - Batched Artifact Database Updates (Priority: P1)

Editor import and startup preimport must update the central artifact database without repeated whole-index rebuilds and per-manifest file writes during asset editing batches.

**Why this priority**: Asset import can touch many manifests in one batch. Rebuilding indexes twice and saving `index.tsv` on every manifest creates avoidable startup/import latency and serializes unrelated work.

**Independent Test**: Artifact database tests can prove batch edits defer `index.tsv` until `StopAssetEditing()` while still preserving all records, and `UpsertManifest()` replaces source records with a single index rebuild.

**Acceptance Scenarios**:

1. **Given** multiple manifests added inside `StartAssetEditing()`, **When** the batch is still open, **Then** the central artifact DB file is not written yet.
2. **Given** the same batch, **When** `StopAssetEditing()` runs, **Then** the central artifact DB contains every manifest record.
3. **Given** an existing source manifest, **When** it is upserted, **Then** old source records are removed and the index is rebuilt once for the final record set.

---

### User Story 6 - Cross-Platform MetaParser Contracts (Priority: P1)

MetaParser regression tests and PPtr target parsing must be robust across build configurations, host platforms, and harmless macro formatting changes.

**Why this priority**: Reflection changes are cross-platform code-generation contracts. Windows Debug-only tests and silent PPtr target parse drift can hide breakage until later platforms or assets exercise it.

**Independent Test**: Fixture tests invoke the built MetaParser through a platform/config-aware helper; a wrapped `NLS_ENGINE_SERIALIZE_PPTR_RESOURCE_TARGETS` fixture succeeds, while an unparseable target macro fails loudly.

**Acceptance Scenarios**:

1. **Given** MetaParser was built in Debug, Release, RelWithDebInfo, or MinSizeRel, **When** fixture tests run, **Then** they locate the executable without hard-coded Debug-only paths.
2. **Given** a wrapped but valid PPtr target macro, **When** MetaParser validates a `PPtr<T>` field, **Then** the target is recognized.
3. **Given** the target macro cannot be parsed, **When** MetaParser runs, **Then** it fails with an error naming `NLS_ENGINE_SERIALIZE_PPTR_RESOURCE_TARGETS` instead of silently returning an empty target set.

---

### User Story 7 - FrameGraph Uniform Buffer State Contract (Priority: P1)

FrameGraph-created uniform buffers must not request illegal DX12 upload-heap transitions when explicit resource tracking is enabled.

**Why this priority**: Dynamic uniform buffers are CPU-visible upload resources. Requesting `UniformBuffer` state for them violates the RHI state contract and can hide real synchronization bugs.

**Independent Test**: A FrameGraph contract test creates a dynamic uniform buffer, calls `preRead()` and `preWrite()`, and verifies no illegal uniform-buffer barrier is submitted.

**Acceptance Scenarios**:

1. **Given** a FrameGraph uniform buffer created with CPU-to-GPU memory, **When** `preRead()` runs under explicit resource tracking, **Then** no illegal `UniformBuffer` transition is recorded.
2. **Given** the same buffer, **When** `preWrite()` runs, **Then** it follows the same CPU-visible state contract.

### Edge Cases

- Generated files under `Runtime/*/Gen/` must not be hand-edited; they must be updated only by the normal build/MetaParser flow.
- Resource classes may contain GPU/RHI implementation details that are not safe or useful as reflected fields; adding `GENERATED_BODY()` must not imply exposing every member as `PROPERTY()`.
- Existing tests and local workspace changes may already touch reflection and rendering files; this migration must avoid reverting unrelated changes.
- Duplicate registration between generated resource types and external resource registrations must not occur.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: Nullus-owned reflected rendering resource classes Mesh, Material, Shader, Texture, Texture2D, and TextureCube MUST use the MetaParser class declaration path with `GENERATED_BODY()`.
- **FR-002**: These resource class headers MUST NOT contain handwritten `StaticMetaTypeName()` functions returning qualified-name string literals.
- **FR-003**: These resource class headers MUST NOT contain handwritten `GetObjectTypeName()` overrides that return handwritten type-name strings or call the removed handwritten static functions.
- **FR-004**: The normal MetaParser generation flow MUST produce stable type names and object bridges for the migrated resource classes.
- **FR-005**: The migration MUST preserve the existing stable qualified names for all migrated resource classes.
- **FR-006**: The migration MUST preserve `PPtr<T>` stable type names for all migrated resource classes.
- **FR-007**: The migration MUST preserve reflection registration for each migrated resource class, pointer type, and const-pointer type.
- **FR-008**: `Runtime/Rendering/ExternalReflection.h` MUST stop manually declaring or registering owned resource class reflection for the migrated resource classes.
- **FR-009**: External/value reflection in `Runtime/Rendering/ExternalReflection.h`, including rendering `Bounds`, MUST remain available.
- **FR-010**: No generated file under `Runtime/*/Gen/` or `Project/*/Gen/` may be hand-edited as part of the source migration.
- **FR-011**: A regression test MUST fail when an owned reflected resource class reintroduces a handwritten type-name/object bridge.
- **FR-012**: Runtime startup MUST prewarm material artifact entries from `RuntimeAssetDatabase` with `MaterialManager::LoadArtifactWithoutTextures`.
- **FR-013**: Material prewarming MUST skip non-material runtime manifest entries.
- **FR-014**: Editor artifact database upserts MUST avoid redundant source-removal index rebuilds.
- **FR-015**: Editor artifact database writes MUST be batch-flushed at `StopAssetEditing()` when asset editing is active.
- **FR-016**: MetaParser fixture tests MUST resolve the MetaParser executable without hard-coded Windows Debug-only paths.
- **FR-017**: MetaParser MUST fail loudly when `NLS_ENGINE_SERIALIZE_PPTR_RESOURCE_TARGETS` exists but cannot be parsed into target entries.
- **FR-018**: Reflection docs MUST describe the implemented `vector<T>`/`Array<T>` shorthand normalization contract.
- **FR-019**: FrameGraph uniform buffers using CPU-visible memory MUST not emit illegal `UniformBuffer` barriers.

### Key Entities

- **Owned Reflected Resource Class**: A Nullus-owned class derived from `NLS::Object` or `NLS::NamedObject` that appears in the reflection database and represents a rendering resource.
- **Generated Type Identity**: The stable type name, type key, and object bridge emitted by MetaParser from `CLASS`/`GENERATED_BODY()`.
- **External Reflection Entry**: A reflection declaration for types that are external to the normal class generation path or cannot be modified directly.
- **Runtime Asset Manifest Entry**: A packaged artifact record used by game startup to resolve runtime resources without editor APIs.
- **Artifact Database Record**: A central editor import index row mapping source assets and sub-assets to imported artifact paths.
- **FrameGraph Buffer State**: The RHI resource state/stage/access tuple requested by FrameGraph before formal pass reads or writes.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Six targeted rendering resource headers are migrated to `CLASS`/`GENERATED_BODY()` with zero handwritten resource `StaticMetaTypeName()` or handwritten resource `GetObjectTypeName()` implementations remaining.
- **SC-002**: Focused reflection/PPtr/resource tests pass after a normal build regenerates reflection outputs.
- **SC-003**: The reflection database reports valid stable names for all six migrated resource classes and their pointer forms.
- **SC-004**: A source-contract regression test detects any future handwritten resource object type bridge in these headers.
- **SC-005**: External reflection for non-owned/value types remains registered after removing the manual resource class registrations.
- **SC-006**: Packaged material artifact paths from runtime manifests are loaded into `MaterialManager` before render-scene synchronization relies on cached material lookup.
- **SC-007**: Batched artifact manifest updates preserve all central index records while reducing per-manifest disk writes during asset editing.
- **SC-008**: MetaParser fixture tests are build-configuration aware and PPtr target macro drift produces an explicit failure.
- **SC-009**: FrameGraph dynamic uniform buffers do not request illegal CPU-visible buffer barriers.

## Assumptions

- The scope is limited to the currently identified production resource classes with handwritten object type bridges: Mesh, Material, Shader, Texture, Texture2D, and TextureCube.
- `PPtr<T>` itself may keep its own generated string composition API because it is a template wrapper rather than an owned reflected resource class.
- Test fixtures may keep local handwritten type names when they intentionally model small fixture-only reflected objects.
- The existing MetaParser supports classes without reflected fields; `GENERATED_BODY()` can be used only for type identity/object bridge generation when no `PROPERTY()` members are present.
