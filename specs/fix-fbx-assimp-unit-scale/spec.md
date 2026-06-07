# Feature Specification: Fix FBX Assimp Unit Scale

**Feature Branch**: `fix-fbx-assimp-unit-scale`  
**Created**: 2026-06-07  
**Status**: Draft  
**Input**: User report that FBX imports appear 100x larger than equivalent glTF imports.

## User Scenarios & Testing

### User Story 1 - Import FBX Prefabs At glTF Scale (Priority: P1)

Artists importing an FBX through the Assimp reader need the generated prefab to instantiate at the same meter-scale convention as an equivalent glTF asset.

**Why this priority**: Imported FBX assets currently appear 100x too large when FBX centimeter-style node scale is combined with unscaled mesh payloads.

**Independent Test**: Import the engine cube FBX with the explicit Assimp FBX reader, then verify the mesh artifact and generated prefab transform combine to a meter-scale instantiated extent.

**Acceptance Scenarios**:

1. **Given** an FBX whose geometry uses centimeter-style authored node scale, **When** Nullus builds the generated prefab, **Then** the mesh payload and transform combine to meter-scale bounds instead of appearing 100x larger.
2. **Given** the same FBX import flow, **When** mesh artifacts are serialized, **Then** their vertex positions are unit-scaled consistently with FBX metadata.

### Edge Cases

- Authored scale on real FBX scene nodes must remain intact when paired with unit-scaled mesh payloads.
- Non-FBX Assimp imports must keep their existing hierarchy and scale behavior.
- Mesh nodes must continue to reference the same generated mesh sub-assets after the root unit wrapper is normalized.

## Requirements

### Functional Requirements

- **FR-001**: Assimp FBX imported scenes MUST NOT instantiate 100x larger than equivalent meter-scale glTF assets.
- **FR-002**: Assimp FBX imported scenes MUST preserve authored child node transforms, including local scale on nodes that represent real scene content.
- **FR-003**: Assimp OBJ and glTF imported scenes MUST remain unaffected by the FBX-specific unit wrapper handling.
- **FR-004**: A focused automated regression test MUST fail before the production fix and pass after it.

### Key Entities

- **Assimp FBX Synthetic Root**: The importer-created root node that carries FBX axis and unit metadata rather than authored scene content.
- **Generated Prefab Transform**: The serialized transform component emitted from imported scene nodes.
- **Mesh Artifact**: The generated native mesh payload referenced by imported scene nodes.

## Success Criteria

### Measurable Outcomes

- **SC-001**: The focused explicit-Assimp FBX import test passes with maximum mesh position under `2.0`.
- **SC-002**: The same test passes with maximum estimated instantiated extent under `2.0`.
- **SC-003**: Nearby Assimp parser/import tests continue to pass.
- **SC-004**: Final validation notes identify the root cause and the exact commands used for verification.

## Assumptions

- The reported 100x size difference is caused by FBX unit scaling not being applied to external FBX mesh payloads, not by runtime renderer scaling.
- Assimp's FBX converter creates a synthetic root named `RootNode` for FBX scene metadata.
- Existing Autodesk FBX SDK parser behavior is out of scope for this Assimp-specific bug fix.
