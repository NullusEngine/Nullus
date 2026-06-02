# Data Model: Optimize Draw-Call Scalability

## DrawStateBucketKey

Represents the compatibility key for cached draw reuse and dynamic instancing.

**Fields**:

- `mesh`: Stable mesh/RHI mesh identity.
- `material`: Stable material identity and shader identity.
- `stateMask`: Render state bits generated from material state.
- `primitiveMode`: Primitive topology.
- `shaderObjectDataSupport`: Whether the shader can consume indexed object data.
- `pipelineOverrideClass`: Pass-specific state class for forward, GBuffer, or transparent drawing.

**Validation Rules**:

- Transparent or order-dependent materials are not eligible for opaque instance grouping.
- Existing material GPU instance counts greater than one are not merged with separate object grouping unless explicitly proven equivalent.
- Keys must not depend on per-frame object transform data.

## CachedSceneDrawCommand

Stable draw state attached to a retained scene primitive.

**Fields**:

- `key`: DrawStateBucketKey.
- `mesh`: Mesh resource pointer/reference.
- `material`: Material resource pointer/reference.
- `stateMask`: Cached state mask.
- `primitiveMode`: Cached primitive mode.
- `buildSerial`: Monotonic serial for rebuild diagnostics.
- `inputStamp`: Mesh/material/render-state revision stamp.

**State Transitions**:

- `Invalid` -> `Valid`: Mesh and material resolve and input stamp is built.
- `Valid` -> `Rebuilt`: Input stamp changes.
- `Valid` -> `Invalid`: Mesh/material disappears or becomes invalid.

## InstanceGroup

Visible compatible objects grouped for instanced submission.

**Fields**:

- `key`: DrawStateBucketKey shared by all members.
- `primitiveIndices`: Retained primitive indices in stable order.
- `visibleMatrices`: Per-visible-object model matrices.
- `objectIndexBegin`: First object-data index assigned for the group.
- `objectCount`: Number of object-data entries used by the group.
- `nearestDistance`: Closest member distance for opaque sorting tie-breaks.

**Validation Rules**:

- `objectCount` must match the submitted instance count.
- `objectIndexBegin + objectCount - 1` must fit within the object-data limit.
- A group may be split into multiple submissions when the object-data limit or configured batch size requires it.

## CommandWorkUnit

An ordered chunk of render work consumed by the threaded RHI path.

**Fields**:

- `passKind`: Opaque, GBuffer, transparent, skybox, helper, lighting, or compute.
- `submissionOrder`: Total ordering for command submission.
- `sourcePassIndex`: Original pass index before slicing.
- `sliceIndex`: Zero-based slice index within the original pass.
- `sliceCount`: Number of slices generated for the original pass.
- `recordedDrawBegin`: First recorded draw command index covered by this slice.
- `recordedDrawCount`: Number of recorded draw commands covered by this slice.
- `requiresOrderedSlicedSubmission`: Whether this work unit may only be consumed by the ordered work-unit path.
- `recordedDrawCommands`: Draw commands owned by this work unit.
- `clearColor`, `clearDepth`, `clearStencil`: Pass clear ownership flags. Attachment-free slices require these to be false. Attachment-backed child slices may carry source pass metadata, but only the parent command buffer owns clear/load/store and render pass begin/end.
- `resourceDependencies`: Incoming visibility and queue dependency edges.
- `parallelEligibility`: Backend/path capability decision.

**Validation Rules**:

- The union of sliced work units must equal the original draw command set.
- Cleared or attachment-backed passes may only use sliced draw ranges when the active backend exposes in-render-pass child command buffer support; otherwise they must remain unsliced.
- Dependencies targeting the original pass must target the first slice; dependencies sourced from the original pass must source from the last slice; adjacent slices from the same pass must preserve submission order through ordered submission and, when required by backend/path semantics, an explicit intra-pass dependency chain.
- Serial fallback paths that cannot prove sliced pass semantics must ignore sliced work units and record the original unsliced `passCommandInputs`.

## DrawCallOptimizationStats

Per-frame and test-facing counters proving the optimization is active.

**Fields**:

- `rawVisibleObjectCount`: Visible objects before grouping.
- `submittedSceneDrawCount`: Scene draw submissions after grouping; existing pass draw counts stay in this submitted/grouped semantic.
- `dynamicInstanceGroupCount`: Number of grouped instance submissions.
- `largestInstanceGroupSize`: Largest submitted instance count.
- `cachedCommandRebuildCount`: Rebuilt cached commands this frame.
- `objectDataOverflowDroppedObjectCount`: Visible objects that could not be submitted because the indexed object-data address space was exhausted. These are counted explicitly instead of emitting draw commands that the binding provider would later reject.

Parallel RHI counters are not owned by this struct; `parallelCommandWorkUnitCount`, `parallelRecordingWorkerCount`, and `parallelFallbackReason` come from threaded frame telemetry and are aggregated only in `FrameInfo`.

**Validation Rules**:

- Counters must be reset per frame.
- Test-facing counters must be deterministic in unit tests.
- Runtime telemetry must not claim parallel execution when the backend chose serial fallback.
- Object-data overflow must be explicit in telemetry; it must not create drawable entries that later fail silently during binding preparation.
