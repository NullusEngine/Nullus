# Feature Specification: Unity-Aligned Asset Reuse

**Feature Branch**: `052-unity-aligned-asset-reuse`
**Created**: 2026-06-17
**Status**: Draft
**Input**: User description: "Align model import duplicate asset handling with Unity-style asset identity and external texture references: preserve model-owned mesh/material/prefab identity, resolve model texture slots through explicit remaps, source paths, unique name search, and model-local fallback, avoid global content-hash dedup, surface remaps in Asset Properties, and validate migration/error behavior."

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Reuse Shared Texture Assets During Model Import (Priority: P1)

An editor user imports or reimports multiple models that reference the same project texture file. The imported model materials should reference the existing texture asset instead of each model creating its own duplicate texture sub-asset.

**Why this priority**: This directly solves duplicate texture assets after model import while matching Unity's asset identity behavior.

**Independent Test**: Can be tested by importing two models that reference the same texture file and verifying that both materials resolve to the same texture asset, with no duplicate model-owned texture sub-assets for that shared file.

**Acceptance Scenarios**:

1. **Given** two model files reference `Assets/Textures/wood.png`, **When** both models are imported, **Then** both imported materials reference the same texture asset identity and resolved artifact.
2. **Given** a model references an external texture file that exists inside the project and has metadata, **When** the model is imported, **Then** the model manifest does not contain a duplicate model-owned texture sub-asset for that file.
3. **Given** a model references an external texture file inside the project that has not yet been imported, **When** the model is imported with automatic missing texture import enabled, **Then** the texture becomes a normal texture asset and the model references it.

---

### User Story 2 - Override Texture Resolution With Explicit Remaps (Priority: P2)

An editor user can inspect a model's discovered texture references and explicitly bind a source texture reference to a project texture asset. Reimporting the model should use this binding ahead of automatic path or name matching.

**Why this priority**: Unity-style remapping is the user-controlled escape hatch for ambiguous or incorrect automatic matching.

**Independent Test**: Can be tested by setting a remap for one model texture reference, reimporting the model, and verifying that the material uses the selected texture asset even when another path or name candidate exists.

**Acceptance Scenarios**:

1. **Given** a model texture reference has an explicit remap to a texture asset, **When** the model is reimported, **Then** that remap is used before source-path matching or name search.
2. **Given** a user clears a remap for a texture reference, **When** the model is reimported, **Then** the importer falls back to automatic resolution rules.
3. **Given** a remap points to an unavailable texture asset, **When** the model is reimported, **Then** the importer reports a warning and continues with automatic resolution where possible.

---

### User Story 3 - Understand Texture Resolution Results In Asset Properties (Priority: P3)

An editor user can select a model and see how each imported texture reference was resolved, including whether it came from a remap, source path, unique name search, embedded fallback, or remains missing.

**Why this priority**: Users need visibility to trust automatic reuse and to repair ambiguous references without inspecting generated files manually.

**Independent Test**: Can be tested by selecting a model with known texture references and verifying that the properties view reports the expected source name, original URI, resolution kind, resolved target, and warnings.

**Acceptance Scenarios**:

1. **Given** a model has texture references resolved by different strategies, **When** the model is selected in Asset Properties, **Then** each reference shows its current resolution kind and target.
2. **Given** a model texture name matches multiple project textures, **When** the model is selected after import, **Then** the properties view shows the ambiguity warning and allows a manual remap.
3. **Given** a model has embedded or data-URI texture content, **When** the model is imported, **Then** Asset Properties reports it as model-local fallback unless the user explicitly remaps it.
4. **Given** a user changes a model's texture resolution settings, **When** the model is reimported, **Then** the next resolution result reflects those settings.

---

### Edge Cases

- Explicit remap target no longer exists or no longer has a usable texture artifact.
- Source URI is relative, absolute, empty, normalized differently, or points outside the project.
- Source path finds a file but the file has no metadata yet.
- Name search finds zero candidates, exactly one candidate, or multiple candidates.
- Texture is embedded in the model, stored as a data URI, or stored in a model container buffer.
- Texture encoding is unsupported or cannot be decoded safely.
- Existing projects have previously imported model-owned texture sub-assets and are reimported under the new behavior.
- External texture is reimported after a model already references it.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The system MUST preserve model-owned identities for model prefabs, meshes, materials, skeletons, skins, animations, and other non-texture model sub-assets during import.
- **FR-002**: The system MUST resolve model texture references using this priority order: explicit remap, source file path, unique project-wide name match, model-local fallback.
- **FR-003**: The system MUST treat explicit texture remaps as stable user-authored metadata for the model source asset.
- **FR-004**: The system MUST allow a texture reference to be remapped only to a valid project texture asset.
- **FR-005**: The system MUST allow users to clear an explicit remap and return that texture reference to automatic resolution on the next import.
- **FR-006**: The system MUST avoid creating model-owned texture sub-assets when a texture reference resolves to an existing or newly imported project texture asset.
- **FR-007**: The system MUST create model-owned texture sub-assets only when external resolution is unavailable and model-local texture payload exists.
- **FR-008**: The system MUST NOT merge assets globally solely because two artifacts or source files have identical content.
- **FR-009**: The system MUST automatically import an unimported project texture file referenced by a model when automatic missing texture import is enabled.
- **FR-010**: The system MUST report a warning and require user remap when name search finds more than one viable texture candidate.
- **FR-011**: The system MUST report diagnostics for missing textures, invalid remaps, unsupported texture encodings, and stale or unavailable texture artifacts.
- **FR-012**: The system MUST record enough dependency information for a model to become stale when a referenced texture asset, texture artifact, or path-to-asset mapping changes.
- **FR-013**: The system MUST show discovered model texture references, current resolution targets, resolution kinds, and warnings in the model's asset properties.
- **FR-014**: The system MUST keep existing imported models unchanged until they are reimported.
- **FR-015**: The system MUST ensure that reimporting a legacy model removes duplicate model-owned texture sub-assets from the current manifest only when those texture references resolve to project texture assets.
- **FR-016**: The system MUST keep model import usable when external texture resolution fails by preserving existing fallback behavior for embedded or otherwise available model-local texture payloads.
- **FR-017**: The system MUST let users configure model-level texture resolution behavior for external texture use, unique-name search, automatic import of missing project texture files, and embedded texture fallback mode.

### Key Entities *(include if feature involves data)*

- **Texture Source Reference**: A texture reference discovered from a model source. It includes a stable source identifier, source name, original URI when available, and any embedded or container-backed payload status.
- **External Object Remap**: User-authored model metadata that maps a texture source reference to a target project texture asset.
- **Resolved Texture Reference**: The import result for a texture source reference. It records the target asset identity, target sub-asset identity where relevant, resolved artifact, resolution kind, and diagnostics.
- **Model Import Manifest**: The current imported output for a model source asset. It lists model-owned sub-assets and dependencies on any reused texture assets.
- **Texture Resolution Diagnostic**: A warning or error explaining a missing texture, invalid remap, ambiguous match, unsupported encoding, or unavailable artifact.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: In a test project where two models reference the same existing project texture, each imported model produces zero model-owned texture sub-assets for that shared texture.
- **SC-002**: In the same shared-texture scenario, both imported materials resolve to the same texture asset identity and artifact after import.
- **SC-003**: Explicit remaps take precedence over automatic matching in 100% of tested remap scenarios.
- **SC-004**: Ambiguous name matches produce a visible warning and no automatic binding in 100% of tested multi-candidate scenarios.
- **SC-005**: Embedded or data-backed textures without an external match continue to import successfully as model-owned texture sub-assets in 100% of tested fallback scenarios.
- **SC-006**: Legacy models are not changed until reimported, and reimported legacy models remain previewable and instantiable after duplicate external texture sub-assets are removed from the current manifest.
- **SC-007**: Asset Properties displays the resolution kind and target for every discovered texture source reference in the model test fixtures.
- **SC-008**: Changing each model-level texture resolution setting changes the next import result or diagnostic in the expected way in the model test fixtures.

## Assumptions

- First-phase behavior aligns with Unity's asset identity and external object remap model; it intentionally does not implement global content-addressed deduplication.
- Texture reuse is scoped to texture references used by imported model materials. Mesh content deduplication across separate model assets is out of scope for this feature.
- Existing projects may keep old generated artifact files on disk until a normal cleanup mechanism removes them; the current manifest is the source of truth for what the model owns after reimport.
- Material payloads may continue to store resolved texture resource paths in the first phase, provided the model manifest and remap metadata preserve asset identity and dependency information.
- Asset Properties is the first UI surface for viewing and editing model texture remaps; Asset View remains focused on previewing resources.
