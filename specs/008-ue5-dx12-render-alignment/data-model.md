# Data Model: UE5-Style DX12 Render Alignment

## GameFrameInput

The immutable per-frame publication produced by the Game Thread for both Game and Editor rendering.

### Responsibilities

- Carries frame-owned scene, camera, viewport, and editor request inputs.
- Is the only render-facing artifact the Game Thread is allowed to publish for normal rendering.
- Remains immutable once published.

### Representative fields

- `frameId`
- `sceneRevision`
- `viewRequests`
- `externalResourceRequests`
- `editorAuxiliaryRequests`
- `readbackRequests`

## RenderFrameBuild

The authoritative Render Thread product that owns view preparation, RDG construction inputs, and compiled frame intent before backend execution.

### Responsibilities

- Defines the frame's graph-visible work.
- Owns pass intent for runtime and editor rendering.
- Replaces driver-built compatibility packages as the normal source of execution truth.

### State transitions

- `Building`
- `GraphReady`
- `Submitted`
- `Retired`

## ViewRequest

A frame-owned description of one runtime or editor view participating in the frame.

### Variants

- `GameView`
- `SceneView`
- `OffscreenView`
- `AuxiliaryReadbackView`

### Responsibilities

- Allows Editor and Game to share the same frame pipeline while differing only by view content and graph inputs.

## GraphExternalResource

An explicitly imported or extracted resource that crosses the RDG boundary.

### Examples

- swapchain backbuffer
- editor viewport output
- offscreen target
- extracted scene color
- extracted picking surface

### Responsibilities

- Makes graph boundary crossings explicit.
- Prevents implicit lifetime leaks for external consumers.

## EditorAuxiliaryPassRequest

A frame-owned request for editor-specific rendering work.

### Examples

- picking
- gizmo
- grid
- outline
- editor overlay composition

### Responsibilities

- Keeps editor-only behavior visible inside the same authoritative frame model.
- Prevents editor-only bypass rendering.

## ReadbackRequest

A frame-owned request whose execution and completion must remain inside the authoritative frame lifecycle.

### Responsibilities

- Describes what must be read back.
- Carries completion ownership until frame retirement allows the result to be consumed or released.

### State transitions

- `Queued`
- `Scheduled`
- `Submitted`
- `Completed`
- `Consumed`
- `Retired`

## DX12SubmissionBatch

The backend-facing RHI Thread unit of work for translation, submission, synchronization, and retirement.

### Responsibilities

- Owns DX12 execution details without leaking them back into renderer or editor code.
- Provides the final queue submission and retirement boundary.

### State transitions

- `Translating`
- `Queued`
- `InFlight`
- `Retired`

## FrameRetirementToken

The proof that a frame and all associated resources are safe to reuse or release.

### Responsibilities

- Covers visible output, offscreen output, extracted resources, and readback completion.
- Prevents reuse or destruction before execution truly finishes.

## PipelineCacheEntry

The centralized identity of a reusable graphics or compute pipeline.

### Responsibilities

- Prevents per-pass or per-draw ad hoc pipeline creation.
- Supports diagnostics that prove centralized policy is being used.

## DescriptorAllocationScope

The centrally managed lifetime interval for descriptor-backed bindings.

### Variants

- `FrameTransient`
- `FramePersistent`
- `LongLivedPersistent`

### Responsibilities

- Makes descriptor ownership explicit.
- Allows validation that descriptor policy remains centralized.

## Relationships

- One `GameFrameInput` produces one `RenderFrameBuild`.
- One `RenderFrameBuild` may contain multiple `ViewRequest` values.
- One `RenderFrameBuild` may import or extract multiple `GraphExternalResource` values.
- One `RenderFrameBuild` may contain multiple `EditorAuxiliaryPassRequest` values.
- One `RenderFrameBuild` may generate one or more `DX12SubmissionBatch` values.
- Each `DX12SubmissionBatch` contributes to one `FrameRetirementToken`.
- `ReadbackRequest` completion is governed by the owning `FrameRetirementToken`.
- `PipelineCacheEntry` and `DescriptorAllocationScope` are shared centralized services consumed during `RenderFrameBuild` execution and `DX12SubmissionBatch` submission.
