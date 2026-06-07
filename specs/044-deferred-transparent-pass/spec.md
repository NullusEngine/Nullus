# Feature Specification: Deferred Transparent and Decal Passes

**Feature Branch**: `044-deferred-transparent-pass`
**Created**: 2026-06-06
**Status**: Implementation Complete, Quality Review In Progress
**Input**: User description: "Fix individual texture errors visible in the editor RenderDoc capture."

## User Scenarios & Testing

### User Story 1 - Transparent Imported Materials Render In Deferred (Priority: P1)

Artists and developers can view imported scenes in the editor without blendable opacity-map materials being dropped from the deferred renderer.

**Why this priority**: The supplied capture shows a scene-rendering defect where a correctly imported transparent texture does not survive the deferred rendering path.

**Independent Test**: A deferred render scene package containing opaque, lighting, and transparent draw commands produces ordered pass inputs for GBuffer, Lighting, and Transparent, with the transparent draw preserved after lighting.

**Acceptance Scenarios**:

1. **Given** a material imported with blend alpha, **When** the editor renders with the deferred renderer, **Then** the material is drawn after deferred lighting using normal color blending.
2. **Given** a deferred threaded render package has transparent draw commands, **When** the package is compiled, **Then** the transparent commands remain in a Transparent pass after Lighting.

### User Story 2 - Deferred Decals Participate In Lighting (Priority: P1)

Artists can import mesh decals such as Sponza dirt decals without those decals being treated as post-lighting transparent objects when they are intended to modify lit surface albedo.

**Why this priority**: Dirt decal assets should affect GBuffer albedo before deferred lighting; drawing them after lighting makes them visible but does not make them participate in the same deferred lighting solve as the surface they decorate.

**Independent Test**: A deferred render scene package containing opaque, decal, lighting, and transparent draw commands produces ordered pass inputs for GBuffer, Decal, Lighting, and Transparent.

**Acceptance Scenarios**:

1. **Given** a material explicitly marked as decal, **When** the deferred renderer gathers visible commands, **Then** it queues the drawable as a decal instead of a transparent drawable.
2. **Given** a deferred package has decal draw commands, **When** the package is compiled, **Then** the decal commands are assigned to a Decal pass after GBuffer and before Lighting.
3. **Given** a regular blendable transparent material, **When** the package is compiled, **Then** it remains in the post-lighting Transparent pass.

### Edge Cases

- Scenes with no transparent drawables continue producing only GBuffer and Lighting scene passes.
- Scenes with no decals continue producing the previous GBuffer, Lighting, and optional Transparent pass order.
- Editor helper passes still render after scene lighting and keep using the deferred depth attachment.
- External scene output attachments are preserved for Lighting, Transparent, and helper passes; decal passes bind GBuffer color attachments, enable albedo writes through per-target color masks, and use GBuffer depth read-only.

## Requirements

### Functional Requirements

- **FR-001**: Deferred rendering MUST preserve blendable scene drawables instead of dropping them from the scene package.
- **FR-002**: Deferred rendering MUST execute transparent drawables after deferred lighting so they blend against the lit scene color.
- **FR-003**: Deferred rendering MUST continue to write opaque drawables only into the GBuffer.
- **FR-004**: Deferred rendering MUST keep editor helper pass behavior and depth access unchanged.
- **FR-005**: The fix MUST include a regression test that fails before the production change and passes after it.
- **FR-006**: Materials MUST expose an explicit surface/render classification that can distinguish decals from ordinary transparent materials.
- **FR-007**: Deferred decal drawables MUST execute after GBuffer and before Lighting so their albedo GBuffer contribution can affect deferred lighting.
- **FR-008**: Imported or serialized materials that do not declare a decal classification MUST retain their existing opaque/blendable behavior.

### Key Entities

- **Deferred scene package**: The per-frame scene execution package containing draw counts, recorded draw commands, pass inputs, and external target metadata.
- **Transparent pass**: A post-lighting color pass for blendable materials using their original material state.
- **Decal pass**: A pre-lighting pass for explicit decal materials that reads GBuffer depth and currently blends albedo into the GBuffer before lighting. Normal/material-channel decal writes are outside this fix.
- **Material surface mode**: A serialized material property that classifies materials as opaque, transparent, or decal for scene queue selection. Alpha-test metadata may be preserved as imported alpha mode, but `surfaceMode=AlphaTest` is rejected until shader discard and queue behavior are implemented end to end.

## Success Criteria

### Measurable Outcomes

- **SC-001**: A targeted unit test verifies the deferred package includes exactly one Transparent pass when transparent draw count is nonzero.
- **SC-002**: Existing deferred scenes without transparent drawables keep their previous GBuffer and Lighting pass order.
- **SC-003**: A targeted unit test verifies the deferred package includes Decal before Lighting and Transparent after Lighting when both decal and transparent draw counts are nonzero.
- **SC-004**: The supplied texture error has a documented RenderDoc-based root cause and the fix path matches that evidence.

## RenderDoc Evidence

- **Capture**: `d:\VSProject\Nullus\App\Win64_Release_Runtime_Shared\Build\RenderDocCaptures\Editor_frame553.rdc`
- **Replay command**: `py -3 Tools\RenderDoc\rdc_analyze.py d:\VSProject\Nullus\App\Win64_Release_Runtime_Shared\Build\RenderDocCaptures\Editor_frame553.rdc`
- **Observed frame shape**: The D3D12 editor frame contained `Nullus/DeferredGBuffer` (EID `2637` -> `4106`, `122` draws) followed by `Nullus/DeferredLighting` (EID `4115` -> `4127`, `1` draw), then editor grid/picking/UI work. No scene Transparent or Decal pass appeared after lighting in the captured bad frame.
- **Material/import cross-check**: The affected imported materials preserve alpha data and enter the render scene as blendable/transparent drawables, so the defect is not an importer-side texture loss.
- **Root cause**: Deferred scheduling dropped transparent scene drawables after lighting. Opaque drawables were recorded for GBuffer, lighting was recorded as the single post-GBuffer scene output pass, and later helper passes could run, but transparent draw commands were not assigned to a post-lighting pass.
- **Fix mapping**: Opaque drawables remain GBuffer-only; deferred lighting writes the lit color target; transparent drawables are rendered afterward with their original materials, color blending, GBuffer depth attached read-only, and depth writes disabled.
- **Decal correction**: Blendable mesh decals are not semantically the same as ordinary transparent surfaces. Explicit decal materials must be scheduled before Lighting so the lighting shader consumes their albedo GBuffer contribution.
- **Current decal scope**: This fix implements albedo-only deferred decals. Unity/Unreal-style richer decal channels such as normals, roughness, and material response require shader and GBuffer channel work beyond this texture-error fix; ordinary transparent surfaces remain post-lighting blended draws.
- **Regression capture**: `d:\VSProject\Nullus\App\Win64_Release_Runtime_Shared\Build\RenderDocCaptures\Editor_frame3600.rdc` was replayed with `py -3 Tools\RenderDoc\rdc_analyze.py`. The D3D12 frame contained `Nullus/DeferredGBuffer` (EID `13` -> `1854`, `153` draws), `Nullus/DeferredLighting` (EID `1863` -> `1875`, `1` draw), and `Nullus/DeferredTransparent` (EID `1898` -> `2009`, `8` draws), but no `Nullus/DeferredDecal` pass.
- **Decal material identity root cause**: The Sponza artifact `TestProject\Library\Artifacts\907a615f-bfd4-4d8c-b8e9-6dd60ec33ed0\materials\material%3Amaterial%2F21.nmat` declares `<name>dirt_decal</name>` and `<blendable>true</blendable>` but has no `<surfaceMode>`. Its source glTF material is named `dirt_decal` with `"alphaMode": "BLEND"`, so legacy loading classified it as ordinary Transparent instead of Decal.

## Assumptions

- The imported texture artifact and material alpha data are valid; the bug is in deferred render pass scheduling.
- Correct alpha sorting improvements are outside this fix; existing scene transparent ordering is preserved.
- The fix targets the editor/runtime deferred renderer path shared by the capture, not a one-off asset workaround.
- Existing imported material artifacts that do not declare `surfaceMode` are treated through the legacy `blendable` flag to avoid broad asset churn.
- Legacy decal inference is intentionally conservative and uses explicit material identity tokens such as `dirt_decal`, `DirtDecal`, or `Decal_01`; arbitrary blend materials remain Transparent unless their identity indicates decal semantics.
