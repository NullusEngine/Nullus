# Feature Specification: glTF Texture Orientation

**Feature Branch**: `045-gltf-texture-orientation`  
**Created**: 2026-06-07  
**Status**: Draft  
**Input**: User report that a Sponza trim texture is scrambled in Nullus while the same asset imports correctly in Unity. RenderDoc evidence points at `stone_trims_01_BaseColor`, not a decal pass.

## User Scenarios & Testing

### User Story 1 - Preserve glTF Texture Rows (Priority: P1)

Artists importing glTF or GLB models need the model textures to align with the UV coordinates authored in the source asset, matching Unity for the same file.

**Why this priority**: The visible Sponza trim-sheet mismatch makes imported scenes look corrupt even when material binding is correct.

**Independent Test**: Import a minimal glTF that references a 2x2 texture with different top and bottom rows, then inspect the generated texture artifact's first mip row order.

**Acceptance Scenarios**:

1. **Given** a glTF image whose top row is red and bottom row is blue, **When** Nullus imports it as an external model texture, **Then** the generated texture artifact keeps red in row 0 and blue in row 1.
2. **Given** a glTF model with direct `TEXCOORD_0` data and a base color texture, **When** the material samples the generated texture artifact, **Then** trim-sheet bands are not swapped vertically.

### Edge Cases

- Existing non-glTF external model import paths keep their legacy texture orientation until separately verified.
- Reimport must regenerate affected external model texture artifacts instead of reusing old flipped artifacts from the artifact cache.
- The fix must not change the global image decoder, shader UV convention, or standalone texture import behavior.

## Requirements

### Functional Requirements

- **FR-001**: External glTF and GLB model texture import MUST preserve the encoded image's top-to-bottom row order.
- **FR-002**: External non-glTF model texture import behavior MUST remain unchanged by this fix.
- **FR-003**: The generated texture artifact build identity or dependencies MUST change so cached external model textures rebuild after the orientation fix.
- **FR-004**: A focused automated test MUST fail before the production fix and pass after it.
- **FR-005**: Validation MUST document whether runtime/RenderDoc reimport evidence was captured or remains a required manual follow-up.

### Key Entities

- **External Model Source**: The model file being imported, with extension used to identify glTF or GLB orientation rules.
- **Texture Sub-Asset**: A generated texture artifact referenced by the imported material.
- **Texture Build Identity**: The identity/dependency data used to decide whether generated texture artifacts are current.

## Success Criteria

### Measurable Outcomes

- **SC-001**: The new glTF texture orientation unit test passes after the fix.
- **SC-002**: Existing focused external model texture import tests continue to pass.
- **SC-003**: A generated glTF texture artifact records a changed postprocessor version compared with the previous importer behavior.
- **SC-004**: The final report clearly states that existing Sponza artifacts require reimport to observe the visual correction.

## Assumptions

- glTF and GLB texture coordinates are interpreted with `(0,0)` at the top-left of the texture image for this import/rendering path.
- The current mesh UV import path and `StandardPBR.hlsl` sampling path should remain unchanged.
- Unity's correct import of the same asset is valid external evidence that the source texture and UV data are not inherently corrupt.
