# Feature Specification: Unity-Ready Model Drop

**Feature Branch**: `unity-ready-model-drop`
**Created**: 2026-05-31
**Status**: Draft
**Input**: User description: "调研unity是怎么做的，和unity对齐，为什么unity可以一拖入场景就能立马看到正确的效果；开始对齐unity方案"

## User Scenarios & Testing

### User Story 1 - Already Imported Models Start Preview Quickly (Priority: P1)

An editor user starts dragging an already imported large model into Scene View and gets a preview quickly enough that the interaction feels immediate rather than blocked by avoidable main-thread preparation work.

**Why this priority**: The reported workflow is specifically about already imported large models taking too long before anything useful appears. This is the primary speed expectation for the drag/drop interaction.

**Independent Test**: Drag an already imported large model whose generated prefab and dependency artifacts are current. The preview appears quickly without rebuilding mesh, material, texture, and prefab relationships on the drag path.

**Acceptance Scenarios**:

1. **Given** an already imported generated model prefab with current mesh, material, and texture relationships, **When** the user begins dragging it over Scene View, **Then** the preview appears quickly enough to support interactive placement.
2. **Given** the same already imported large model is dragged repeatedly, **When** the drag path runs, **Then** it reuses stabilized import results instead of reconstructing mesh, material, texture, and prefab relationships during drag.

---

### User Story 2 - Drag Preview Follows The Mouse Before Drop (Priority: P1)

An editor user drags an imported model asset over the Scene View and sees a temporary preview object follow the mouse before the button is released.

**Why this priority**: This is the missing interaction the user explicitly asked to align with Unity. Without preview feedback, large-model drag/drop feels delayed and uncertain even if final placement succeeds.

**Independent Test**: Start dragging an imported model over Scene View. A preview object appears before drop, follows the current placement point, and disappears or converts only when the drag ends.

**Acceptance Scenarios**:

1. **Given** an imported model asset is dragged over Scene View, **When** the cursor moves across valid placement space before mouse release, **Then** a preview object remains visible and updates to the latest placement point.
2. **Given** a preview object is active during drag, **When** the drag is still in progress, **Then** the preview does not become a committed scene instance and does not appear as a normal Hierarchy item.

---

### User Story 3 - Drop Commits Only Renderer-Ready Instances (Priority: P1)

An editor user releases the mouse after dragging an imported model, and the first committed scene instance is already ready to render with the intended mesh and material state.

**Why this priority**: The current failure mode is that large models can take a long time to appear because committed instances depend on delayed post-drop resource completion. The Unity-aligned target is that the committed result is either ready immediately or not committed yet.

**Independent Test**: Drop a generated model prefab whose manifest and cached renderer dependencies are current. The preview ends and one committed scene instance appears at the preview location without a delayed hidden-instance phase.

**Acceptance Scenarios**:

1. **Given** a generated model prefab with current prefab, mesh, and material dependencies, **When** the user releases the drag in Scene View or Hierarchy, **Then** the drop commits exactly one visible scene instance at the resolved drop target.
2. **Given** the final preview position is valid, **When** the committed instance is created, **Then** its placement matches the final preview placement rather than jumping to an unrelated fallback location.

---

### User Story 4 - Not-Ready Assets Stay Pending Instead Of Creating Delayed Hidden Instances (Priority: P2)

An editor user drops a model whose imported prefab or renderer dependencies are still stale, missing, or otherwise not ready. The editor reports a pending or not-ready result instead of creating a formal scene object that stays hidden while long asynchronous fix-up work runs.

**Why this priority**: This is the root-cause correction from the investigation. Large-model drag/drop should no longer rely on "commit first, hide root, wait for conservative async completion, then reveal" as the main path.

**Independent Test**: Simulate a generated model prefab with missing or stale required dependencies. The preview can end, but no committed scene object is left behind and the result indicates import or readiness is pending.

**Acceptance Scenarios**:

1. **Given** a generated model prefab whose required committed dependencies are stale or missing, **When** the user releases the drag, **Then** the editor does not create a committed scene instance and returns a pending or not-ready result.
2. **Given** a background import or readiness refresh is triggered for a not-ready asset, **When** that work completes, **Then** a later drag may commit normally through the ready path.

---

### User Story 5 - Legacy Async Resolution Cannot Regress The Main Drag/Drop Experience (Priority: P3)

If an older or non-primary path still instantiates generated model prefabs before all renderer references are fully resolved, it must remain a compatibility fallback only and must not reintroduce the long hidden-root waiting behavior as the normal large-model drop experience.

**Why this priority**: Existing async resource-resolution code still matters for compatibility, but it should no longer define the primary user-visible interaction for large model placement.

**Independent Test**: Exercise legacy generated-model resolution paths and verify they either complete as explicit fallback handling or fail cleanly without becoming the default large-model drag/drop path.

**Acceptance Scenarios**:

1. **Given** the primary drag/drop readiness gate succeeds, **When** the user drops a large model, **Then** the committed result does not depend on a long hidden-root async resolution sequence to become visible.
2. **Given** a fallback async resolution path is still used elsewhere, **When** it cannot resolve required renderer inputs, **Then** it fails or cancels without leaving a permanently hidden live object behind.

---

### User Story 6 - Committed Generated Model Instances Survive Save And Reload (Priority: P1)

An editor user drops a generated model prefab into a scene, saves the scene, closes or reloads it, and finds the model instance restored as a prefab-backed scene instance instead of missing.

**Why this priority**: This is a confirmed data-loss bug. A successful drop is not usable if the scene cannot persist and restore the generated model instance. The current failure is not only a UI issue: scene save/load still behaves like ordinary object-graph serialization and does not yet reliably persist generated-model prefab instance identity together with enough reconstructible hierarchy state.

**Independent Test**: Commit a generated model instance into a scene, save the scene, reload it through the normal scene-loading path, and verify the model still exists and restores with prefab-instance identity rather than disappearing.

**Acceptance Scenarios**:

1. **Given** a generated model instance was committed to a scene, **When** the scene is saved and reloaded, **Then** the instance is restored instead of disappearing from the scene.
2. **Given** a restored generated model instance comes from a generated model prefab asset, **When** the scene reload completes, **Then** the instance retains prefab-backed identity rather than being reloaded only as an untracked plain object copy.

### Edge Cases

- The cursor is over empty Scene View space and no scene geometry is hit; preview placement still needs a stable fallback plane.
- The asset was already imported successfully, but the drag path still encounters a cold or stale cache path; the drag experience should consume stabilized import results rather than rebuild relationships on the main path.
- The user drags over Scene View, then leaves the view or cancels the drag before release; the preview object must be cleaned up.
- The user drops onto Hierarchy instead of Scene View; no mouse-follow preview is required there, but committed-instance readiness rules still apply.
- The dragged asset handle points to an old asset id after reimport.
- A prefab artifact exists but one required renderer artifact file was deleted from `Library/Artifacts`.
- A legacy fallback path begins resolution work and the scene changes or the target object is destroyed during that work.
- A generated model instance is the only scene object added before save; scene reload must still restore it even if no other prefab instances exist.
- A saved scene references a generated model prefab sub-asset key; reload must resolve the same sub-asset rather than dropping the object when registry state starts empty.
- A generated model instance may carry most of its visible mesh hierarchy under one prefab root; scene persistence must not rely on only a flat root entry if that would drop the reconstructible model subtree.

## Requirements

### Functional Requirements

- **FR-001**: The import phase MUST generate and stabilize the relationship between mesh, material, texture, and generated model prefab artifacts before Scene View drag/drop consumes that asset.
- **FR-002**: The already-imported large-model drag/drop main path MUST reuse those stabilized import results instead of rebuilding mesh, material, texture, and prefab relationships on the drag path.
- **FR-003**: Dragging an imported model over Scene View MUST create a temporary drag preview object before mouse release.
- **FR-004**: The drag preview object MUST follow the current resolved placement point while the drag remains active in Scene View.
- **FR-005**: The drag preview object MUST NOT be treated as a committed scene instance, MUST NOT appear as a normal Hierarchy item, and MUST NOT become the final persisted result automatically.
- **FR-006**: Ending a Scene View drag MUST destroy or replace the preview object as part of final drop handling.
- **FR-007**: Releasing a ready imported model drop MUST commit exactly one scene instance using the final preview placement or resolved drop target.
- **FR-008**: Generated model prefab drag/drop MUST treat renderer dependency readiness as part of drop-time asset readiness.
- **FR-009**: The primary generated-model drop path MUST NOT commit a formal scene instance that depends on a long hidden-root async resolution phase before first visibility.
- **FR-010**: If required committed renderer dependencies are not ready at drop time, the editor MUST return a pending, importing, or not-ready result instead of committing the scene instance.
- **FR-011**: Fast prefab loading MUST verify current manifest dependencies and required committed renderer artifact files, not only the prefab artifact file.
- **FR-012**: The already-imported large-model drag/drop main path MUST avoid heavy synchronous work whose responsibility belongs to import-time asset stabilization.
- **FR-013**: Scene View preview behavior MAY use controlled visual degradation for partially cached assets, but committed scene instances MUST follow the ready-or-pending rule.
- **FR-014**: Hierarchy and Scene View drops MUST share the same committed-instance readiness gate even if only Scene View provides mouse-follow preview.
- **FR-015**: Existing async renderer resolution for generated models MUST be treated as a compatibility fallback path rather than the primary large-model Scene View drop experience.
- **FR-016**: Existing async renderer resolution MUST still fail or cancel cleanly when required assets cannot be resolved and MUST NOT leave a still-live generated model root permanently hidden.
- **FR-017**: A committed generated model scene instance MUST be serialized in scene data as a prefab-backed instance relationship that can be restored after scene reload.
- **FR-018**: Scene reload MUST restore committed generated model instances from their prefab asset id and prefab sub-asset identity instead of dropping them or keeping them only as transient runtime registry state.
- **FR-019**: Loading a scene that contains generated model prefab instances MUST rebuild the corresponding prefab-instance tracking state needed by editor prefab presentation and later prefab-aware operations.
- **FR-020**: The generated-model save and reload path MUST preserve normal transform, hierarchy parentage, and prefab-sub-asset targeting for restored instances.
- **FR-021**: Scene persistence for generated model instances MUST NOT depend solely on transient editor-only prefab instance registry state.
- **FR-022**: Scene persistence for generated model instances MUST preserve enough data to reconstruct the full committed model instance, including prefab-backed hierarchy recovery rather than only a flat root scene entry.

### Key Entities

- **Generated Model Prefab**: Imported model prefab artifact produced from an external model source.
- **Drag Preview Object**: Temporary, non-committed Scene View object used only while the user is actively dragging before mouse release.
- **Committed Scene Instance**: The formal scene object created only after a drop succeeds.
- **Renderer Dependency Readiness Gate**: Validation step that decides whether a generated model can be committed immediately or must remain pending.
- **Legacy Async Resolution Path**: Older compatibility flow that resolves deferred renderer references after instantiation.
- **Prefab-Backed Scene Instance Record**: The persisted scene representation of a committed generated model instance, including its prefab asset identity and prefab sub-asset identity.

## Success Criteria

### Measurable Outcomes

- **SC-001**: During Scene View drag, a visible preview object appears before mouse release and tracks the cursor placement continuously until the drag ends.
- **SC-002**: For already imported large models with current generated prefab relationships, preview appearance is fast enough that drag placement remains interactive instead of feeling blocked by main-thread stabilization work.
- **SC-003**: A ready generated model drop commits exactly one visible scene instance without a user-visible long hidden-instance waiting phase.
- **SC-004**: A not-ready generated model drop commits zero scene instances and returns a pending or not-ready result instead of leaving a delayed hidden formal instance in the scene.
- **SC-005**: Focused tests cover Scene View preview lifecycle, ready commit, not-ready pending behavior, save-and-reload persistence of generated model instances, and legacy async cancellation or failure fallback handling.
- **SC-006**: For the reported already-imported large-model workflow, the drag/drop path no longer requires the user to wait through a long post-drop invisibility period before the first committed object appears.
- **SC-007**: Saving and reloading a scene containing a committed generated model instance restores that instance without manual repair or re-drop.

## Assumptions

- This phase targets the editor/DX12 workflow reported by the user; Vulkan/macOS/Linux claims require separate validation.
- The current asset pipeline continues to generate and stabilize model-prefab, mesh, material, and texture relationships before scene placement.
- The current known save/reload failure is caused by missing or incomplete persistence of generated-model prefab instance identity and reconstructible hierarchy state; this phase is expected to close that gap rather than work around it manually.
- Scene View preview and committed-instance placement may reuse existing editor picking and spawn-point logic where possible.
- Preview interaction is in scope for Scene View drag; Hierarchy drop continues to focus on committed result behavior rather than mouse-follow preview.
- Full Unity parity for advanced placement behaviors such as surface-normal alignment or complex multi-scene drag semantics is out of scope for this phase.
- Persisting generated model instances should preserve prefab-instance semantics rather than silently converting them into unrelated unpacked scene copies.
