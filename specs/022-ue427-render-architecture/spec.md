# Feature Specification: UE4.27 Render Architecture Alignment

**Feature Branch**: `022-ue427-render-architecture`  
**Created**: 2026-05-10  
**Status**: Draft  
**Input**: User description: "RHI、frame graph、shader binding，多线程渲染也做到和UE4.27源码对齐"; licensed reference path supplied: `F:\Epic Games\UE_4.27\Engine`

## User Scenarios & Testing

### User Story 1 - RHI Command List Submission Contract (Priority: P1)

An engine developer can reason about rendering submission through a UE4.27-style command list lifecycle: command recording is separated from command execution, immediate submission is explicit, and parallel command lists can be queued with known dependencies.

**Why this priority**: RHI command lists are the foundation for RDG execution, shader binding calls, and threaded draw submission. Without this contract, later UE-aligned layers will keep depending on backend-specific command buffer behavior.

**Independent Test**: Can be tested by creating contract command lists that record Begin, pass commands, barriers, End, and submit events without requiring a real GPU backend.

**Acceptance Scenarios**:

1. **Given** a command list with no recorded work, **When** it is submitted, **Then** submission metadata identifies an empty list and does not report visible draw work.
2. **Given** a command list containing graphics and compute work, **When** it is finalized, **Then** it reports queue type, recording state, command count, and debug name before queue submission.
3. **Given** multiple child command lists, **When** they are queued for a parent immediate submission, **Then** their submit order and dependency policy are preserved.

---

### User Story 2 - RDG-Style Frame Graph Pass Ownership (Priority: P1)

An engine developer can define render passes by declaring resources and side effects up front, so the frame graph owns pass ordering, resource visibility, and extraction transitions in a way that matches UE4.27 Render Dependency Graph behavior.

**Why this priority**: Nullus already has a frame graph and recent LightGrid compute passes. Aligning pass/resource contracts prevents hidden barriers and makes future renderer features easier to port from UE-style flows.

**Independent Test**: Can be tested by compiling synthetic graph passes and verifying declared reads, writes, side effects, culling eligibility, queue class, and exported transitions.

**Acceptance Scenarios**:

1. **Given** a pass that writes a transient texture consumed by a later pass, **When** the graph is compiled, **Then** the producer and consumer are connected by a resource visibility dependency.
2. **Given** a pass with side effects but no extracted resource, **When** the graph is compiled, **Then** it is retained.
3. **Given** a pass that uses an undeclared resource, **When** validation runs, **Then** the graph reports a compile error rather than relying on implicit backend state.

---

### User Story 3 - Shader Parameter Binding Contract (Priority: P2)

An engine developer can bind shader parameters through a UE4.27-style parameter metadata contract: frame, object, material, and pass resources are validated as structured parameter groups before pipeline layout creation and command recording.

**Why this priority**: Shader binding errors are currently caught by reflection/layout utilities, but UE alignment requires the higher-level parameter struct concept to be visible in renderer contracts.

**Independent Test**: Can be tested by constructing reflected shader parameter groups and verifying binding set order, register spaces, resource kind validation, and missing-parameter diagnostics.

**Acceptance Scenarios**:

1. **Given** a shader declares frame, object, material, and pass resources, **When** the binding contract is built, **Then** descriptor sets keep stable UE-like group ordering.
2. **Given** two parameters collide in the same register space and binding, **When** validation runs, **Then** the error names both conflicting bindings.
3. **Given** a render pass requires lighting data, **When** LightGrid is disabled, **Then** the pass receives an explicit empty or skipped binding state rather than stale resources.

---

### User Story 4 - Parallel Draw Command Build And Submit (Priority: P2)

An engine developer can build draw command batches independently from command list submission, then submit them through a threaded rendering plan that mirrors UE4.27's parallel mesh draw command flow.

**Why this priority**: Nullus already publishes render scene packages and parallel work units. UE alignment requires the draw-command batch to become the stable handoff between scene preparation and RHI submission.

**Independent Test**: Can be tested by preparing recorded draw commands, splitting them into work units, and verifying dependency edges, eligibility flags, queue class, and submission telemetry.

**Acceptance Scenarios**:

1. **Given** visible opaque and transparent draw commands, **When** a render scene package is prepared, **Then** draw commands are grouped by pass role before RHI submission.
2. **Given** a compute prepass followed by graphics lighting, **When** threaded submission is compiled, **Then** graphics consumers depend on the last compute producer.
3. **Given** a pass is marked serial-only, **When** the threaded plan is applied, **Then** it remains in submission order but is not treated as eligible for parallel recording.

### Edge Cases

- Empty scenes must still produce valid frame lifecycle telemetry and must not submit bogus draw work.
- Offscreen frames and swapchain frames must both preserve extraction and presentation boundaries.
- Compute-only graph prefixes must promote their first graphics consumer to depend on the compute producer.
- Missing shader parameters, duplicate binding slots, and incompatible register spaces must fail validation before pipeline layout creation.
- Backend support claims must be limited to validated backends; DX12 is the first target for runtime evidence.
- Generated files under `Runtime/*/Gen/` and `Project/Editor/Gen/` must not be hand-edited.
- UE4.27 systems that Nullus does not yet have, such as full task graph parity or renderer-specific mesh pass processors, must be documented as non-parity instead of hidden behind matching names.

## Requirements

### Functional Requirements

- **FR-001**: The system MUST expose a command-list-level contract that records lifecycle state, queue type, debug name, command counts, child submit ordering, and whether visible draw work was recorded.
- **FR-002**: The system MUST keep immediate command submission explicit and separate from pass declaration and draw command preparation.
- **FR-003**: The system MUST support queued graphics and compute work units with dependency policies equivalent to none, previous pass, last graphics producer, and last compute producer.
- **FR-004**: The frame graph MUST validate declared pass reads, writes, side effects, queue type, and resource visibility transitions before command recording.
- **FR-005**: The frame graph MUST retain side-effect passes even when no extracted resource depends on them.
- **FR-006**: The frame graph MUST produce diagnostics for undeclared or conflicting resource access before a backend command buffer is used.
- **FR-007**: Shader binding MUST be represented as structured parameter groups for frame, object, material, and pass data, with deterministic descriptor set/register-space mapping.
- **FR-008**: Shader binding validation MUST catch missing required resources, duplicate binding slots, invalid array sizes, and stale pass binding use.
- **FR-009**: Parallel draw preparation MUST publish draw command batches independent of immediate command submission.
- **FR-010**: Threaded rendering MUST preserve pass role grouping, queue class, dependency edges, serial/parallel eligibility, and telemetry for submitted work.
- **FR-011**: Existing Editor and Game runtime paths MUST remain runnable while the architecture is migrated.
- **FR-012**: Documentation MUST list every intentional non-parity with the supplied UE4.27 reference and identify the follow-up path.
- **FR-013**: Automated tests MUST cover RHI command list contracts, RDG-style pass/resource contracts, shader parameter binding validation, and threaded draw command planning.

### Key Entities

- **RHI Command List Contract**: The recording and submission lifecycle metadata for graphics, compute, immediate, and child command work.
- **RDG Pass Contract**: A declared frame graph pass with name, queue class, resource reads/writes, side effects, culling eligibility, and execution callback.
- **RDG Resource Access**: A declared texture or buffer read/write access with required state, stages, and visibility transition.
- **Shader Parameter Group**: A structured binding group that maps reflected shader parameters to frame, object, material, or pass descriptor sets.
- **Parallel Draw Command Batch**: A prepared set of draw commands grouped by pass role and eligible for serial or parallel recording.
- **Render Submission Timeline**: The ordered frame handoff from scene preparation, graph compilation, command recording, queue submission, presentation, and resource retirement.

## Success Criteria

### Measurable Outcomes

- **SC-001**: Targeted unit tests verify command list lifecycle metadata for empty, graphics, compute, and child-list submission cases.
- **SC-002**: Targeted unit tests verify graph pass retention, declared resource dependencies, and validation diagnostics for invalid access.
- **SC-003**: Targeted unit tests verify shader parameter group ordering and binding conflict diagnostics.
- **SC-004**: Targeted unit tests verify threaded draw command grouping, compute-to-graphics dependency promotion, and serial versus parallel eligibility.
- **SC-005**: `Editor` Debug build succeeds after each completed implementation phase.
- **SC-006**: DX12 RenderDoc or equivalent runtime evidence is captured before claiming runtime renderer parity for this feature.
- **SC-007**: `research.md` lists all known UE4.27 parity gaps with a reason and follow-up path.

## Assumptions

- The supplied UE4.27 source tree is a licensed local reference for architecture and behavior; Nullus code remains original and adapted to existing engine abstractions.
- "完全对齐" is treated as aligning core lifecycle and data-flow contracts first, then migrating implementation details in reviewable phases.
- DX12 is the first validation backend; other backends require separate evidence.
- Current LightGrid work remains in place and must continue to use the shared frame graph, shader binding, and threaded rendering contracts.
- Full UE task graph, renderer module hierarchy, and all mesh pass processor types are out of scope for the first implementation slice unless a later spec expands scope.
