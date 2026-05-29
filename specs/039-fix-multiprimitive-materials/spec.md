# Feature Specification: Fix Multi-Primitive Materials

**Feature Branch**: `[039-fix-multiprimitive-materials]`
**Created**: 2026-05-29
**Status**: Draft
**Input**: User report: "mesh textures still mismatch after model drag into scene"

## User Scenarios & Testing

### User Story 1 - Imported multi-material meshes keep correct textures (Priority: P1)

When a model contains one source mesh with multiple primitives using different materials, dropping the generated model prefab into the scene must display each primitive with its own material and texture.

**Why this priority**: This is the visible failure reported by the user; fixing texture binding alone is insufficient if primitive material assignment is lost during import.

**Independent Test**: Import or synthesize a multi-primitive model and verify generated renderable objects retain a one-to-one primitive mesh to material relationship.

**Acceptance Scenarios**:

1. **Given** a source mesh with two primitives assigned to two materials, **When** the model artifacts and generated prefab are built, **Then** each renderable primitive resolves to the matching material slot.
2. **Given** a single-primitive mesh, **When** artifacts and prefab are built, **Then** existing mesh and material references remain compatible.

### Edge Cases

- Multi-primitive meshes with sparse material indices must preserve empty slots where required by existing `Mesh::GetMaterialIndex()` lookup.
- Missing or invalid material references should continue to fall back to existing editor default material behavior.

## Requirements

### Functional Requirements

- **FR-001**: The importer MUST preserve per-primitive material indices for source meshes containing multiple primitives.
- **FR-002**: Generated prefabs MUST create renderable records that allow each primitive mesh to select the material intended for that primitive.
- **FR-003**: Single-primitive imported meshes MUST keep existing sub-asset keys and prefab shape where practical for compatibility.
- **FR-004**: Deferred texture binding MUST continue to request missing texture artifacts asynchronously without blocking scene insertion.

### Key Entities

- **Imported mesh primitive**: A source mesh primitive with geometry and a material key.
- **Mesh artifact**: A serialized render mesh with one material index used by runtime material resolution.
- **Generated prefab renderer**: The prefab records that bind mesh artifacts to material references.

## Success Criteria

### Measurable Outcomes

- **SC-001**: A regression test demonstrates that a two-primitive imported mesh generates two independently materialized renderable meshes.
- **SC-002**: Existing single-primitive generated model prefab tests continue to pass.
- **SC-003**: Focused import, prefab, and editor render-path contract tests pass in Debug configuration.

## Assumptions

- Nullus runtime meshes currently support one material index per `Mesh`, so multi-material source meshes must be represented as multiple renderable mesh artifacts or equivalent renderable records.
- This change targets generated model import/prefab correctness, not a new runtime submesh rendering API.
