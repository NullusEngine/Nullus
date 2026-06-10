# Feature Specification: DX12 Sampler Descriptor Reuse

**Feature Branch**: `dx12-sampler-descriptor-reuse`  
**Created**: 2026-06-10  
**Status**: Draft  
**Input**: User description: "Fix DX12SamplerHeapAllocator out-of-descriptors errors when importing large prefab scenes such as Sponza."

## User Scenarios & Testing

### User Story 1 - Large Imported Prefabs Remain Renderable (Priority: P1)

An editor user imports or commits a generated model prefab with hundreds of renderer material slots, and the DX12 backend keeps renderer resources valid instead of exhausting the sampler descriptor heap.

**Why this priority**: This directly addresses the observed failure where Sponza import reaches `persistentUsed=2048` and `NativeDX12BindingSet` fails to allocate sampler descriptors.

**Independent Test**: Create repeated DX12 material binding sets with identical sampler layouts under a deliberately small sampler heap; the second identical set must succeed by reusing sampler descriptors.

**Acceptance Scenarios**:

1. **Given** a DX12 sampler descriptor heap with capacity for one sampler table, **When** two binding sets with identical sampler descriptors are created, **Then** both binding sets are valid and sampler descriptor usage remains one table.
2. **Given** a large generated model prefab with many materials sharing common sampler state, **When** renderer resources are resolved, **Then** sampler descriptor allocation does not fail solely because identical sampler tables are duplicated per material.

---

### User Story 2 - Distinct Sampler State Stays Correct (Priority: P2)

Render code can bind materials with different sampler states without accidentally sharing the wrong descriptor table.

**Why this priority**: Reuse must preserve correctness; a nearest/clamp sampler must not alias a linear/repeat sampler table.

**Independent Test**: Create binding sets with different sampler descriptors and verify they consume distinct sampler table allocations.

**Acceptance Scenarios**:

1. **Given** two DX12 binding sets with the same layout but different sampler descriptors, **When** both are created, **Then** they receive different sampler table allocations.
2. **Given** a shared sampler table still referenced by at least one binding set, **When** another binding set releases it, **Then** the table remains valid until the final owner is destroyed.

### Edge Cases

- Binding sets with no sampler entries must continue to avoid sampler heap allocation.
- Binding sets with sampler arrays or multiple sampler descriptor ranges must include every table descriptor in the reuse key.
- Binding sets with failed resource descriptor allocation must not leak a sampler table reference.
- The cache must be protected against concurrent binding set creation and destruction.
- Resource descriptors, texture descriptors, and buffer descriptors must keep their current allocation behavior.

## Requirements

### Functional Requirements

- **FR-001**: The DX12 backend MUST reuse identical shader-visible sampler descriptor tables across binding sets created from equivalent sampler table layouts and sampler descriptors.
- **FR-002**: The DX12 backend MUST allocate distinct sampler descriptor tables when any sampler table layout coordinate or sampler descriptor field differs.
- **FR-003**: Reused sampler descriptor tables MUST remain alive until all binding sets using them are destroyed.
- **FR-004**: Releasing the final owner of a sampler table MUST return its descriptor range to the existing DX12 sampler heap allocator.
- **FR-005**: Resource descriptor allocation behavior MUST remain unchanged.
- **FR-006**: Existing diagnostic logging for sampler heap exhaustion MUST remain available for truly unique sampler pressure.
- **FR-007**: The solution MUST be internal to the DX12 backend and MUST NOT require material, shader, prefab, or editor callers to special-case DX12 sampler reuse.

### Key Entities

- **Sampler Descriptor Table Key**: The canonical identity of a DX12 sampler table, including table layout coordinates and ordered sampler descriptor values.
- **Sampler Descriptor Table Cache Entry**: A cached descriptor range with reference ownership and descriptor count.
- **DX12 Binding Set**: The backend binding set that obtains resource descriptors directly and sampler descriptors through the cache.

## Success Criteria

### Measurable Outcomes

- **SC-001**: A focused DX12 unit test proves two identical sampler binding sets can be created with a sampler heap capacity of one table.
- **SC-002**: A focused DX12 unit test proves different sampler descriptors do not share the same table.
- **SC-003**: A focused release test proves descriptor usage returns to zero after the last shared table owner is destroyed.
- **SC-004**: Existing descriptor allocator and renderer binding lifecycle tests continue to pass.
- **SC-005**: The implementation preserves DX12-only scope; no generated files and no non-DX12 backend files are modified for the core fix.

## Assumptions

- Large imported scenes commonly reuse a small set of sampler states, especially linear/repeat and linear/clamp PBR samplers.
- The D3D12 shader-visible sampler heap capacity should remain bounded at the existing 2048 descriptors.
- Static samplers are not introduced in this fix because material sampler metadata can vary per imported texture slot.
- Vulkan and non-explicit backends are out of scope for this specific failure.
