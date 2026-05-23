# Data Model: UE4.27 Render Architecture Alignment

## RHI Command List Contract

**Purpose**: Describes a recordable command list before it is submitted to a queue or merged into an immediate submission.

**Fields**:

- `debugName`: Human-readable name for diagnostics and profiling.
- `queueType`: Graphics or compute queue class.
- `lifecycleState`: Available, Recording, Closed, Submitted, or Retired.
- `commandCount`: Number of recorded command operations.
- `visibleDrawCount`: Number of visible draw commands represented by this list.
- `childCommandLists`: Ordered child lists queued into an immediate submission.
- `dependencyPolicy`: None, previous pass, last graphics, or last compute.

**Validation Rules**:

- A list must be closed before submission.
- A child list cannot be submitted twice in the same parent.
- Empty lists are allowed but must report no visible work.

## RDG Pass Contract

**Purpose**: Describes a frame graph pass before graph compilation.

**Fields**:

- `name`: Stable pass name for diagnostics.
- `queueType`: Graphics or compute.
- `reads`: Declared resource reads.
- `writes`: Declared resource writes.
- `sideEffect`: Whether pass must be retained without extracted outputs.
- `cullingEligible`: Whether unused pass culling can remove the pass.
- `executionMode`: Output render pass, recorded render pass, compute dispatch, or callback.

**Validation Rules**:

- A pass with no reads, writes, or side effects is culling-eligible.
- A pass with side effects is retained.
- Queue type must match command kind.
- Resource access conflicts must produce compile diagnostics.

## RDG Resource Access

**Purpose**: Describes one declared texture or buffer access.

**Fields**:

- `resourceKind`: Texture or buffer.
- `mode`: Read or write.
- `state`: Required resource state.
- `stages`: Pipeline stages.
- `access`: Memory access mask.
- `subresourceRange`: Texture subresource range where applicable.

**Validation Rules**:

- Null resources are invalid in required accesses.
- Conflicting writes in the same pass are invalid unless explicitly modeled.
- Exported resources must receive visibility transitions before extraction.

## Shader Parameter Group

**Purpose**: Groups shader-visible resources in a UE4.27-like parameter contract.

**Fields**:

- `groupKind`: Frame, object, material, or pass.
- `descriptorSet`: Stable descriptor set index.
- `registerSpace`: Register space used by the backend layout.
- `parameters`: Reflected constants, textures, samplers, buffers, and UAVs.
- `required`: Whether pass recording may proceed without the group.

**Validation Rules**:

- Duplicate register/binding pairs are invalid.
- Required parameters must have backing resources before recording.
- Empty descriptor set slots must be preserved when higher set indices exist.

## Parallel Draw Command Batch

**Purpose**: Stores prepared draw work independent from immediate queue submission.

**Fields**:

- `passRole`: Opaque, transparent, skybox, helper, or auxiliary.
- `drawCommands`: Prepared draw commands for the pass.
- `queueType`: Graphics or compute.
- `eligibleForParallelRecording`: Whether work may be recorded in parallel.
- `eligibleForParallelTranslation`: Whether command translation may be parallelized.
- `dependencyEdges`: Incoming resource or queue dependencies.

**Validation Rules**:

- Serial-only batches retain submission order.
- Compute producers must create dependency edges for graphics consumers.
- Empty helper-only batches do not count as visible scene draw work.

## Render Submission Timeline

**Purpose**: Captures the frame lifecycle from scene preparation to resource retirement.

**Stages**:

1. Frame snapshot published.
2. Render scene package prepared.
3. Frame graph compiled.
4. Command lists recorded.
5. Queues submitted.
6. Swapchain or offscreen output presented/extracted.
7. Transient resources retired.

**Validation Rules**:

- Each submitted frame records whether it used serial, parallel, or async compute paths.
- Resource retirement happens after the owning frame is retired.
- Readback/extraction resources remain alive until consumers can safely use them.
