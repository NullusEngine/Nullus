# Feature Specification: Fix DX12 MultiFramebuffer Barriers

**Feature Branch**: `031-fix-dx12-multiframebuffer-barriers`  
**Created**: 2026-05-23  
**Status**: Draft  
**Input**: User reported DX12 debug-layer message 527 failures for `MultiFramebufferColorTexture0/1/2` and `MultiFramebufferDepthTexture` where transition `Before` states no longer match the previous barrier state.

## User Scenarios & Testing

### User Story 1 - DX12 Deferred Frame Does Not Lose Device (Priority: P1)

Developers can run the editor deferred path on DX12 without the RHI submitting stale render-target/depth-write transitions after the GBuffer has already been made readable.

**Why this priority**: The reported failure removes the DX12 device and prevents the editor frame from presenting.

**Independent Test**: A threaded rendering lifecycle regression test builds a GBuffer write pass followed by visibility/extraction transitions and verifies stale post-pass barriers are not emitted.

**Acceptance Scenarios**:

1. **Given** a threaded deferred frame writes MultiFramebuffer color attachments, **When** those textures are later read or extracted, **Then** the submitted command buffers do not contain a second `RenderTarget -> ShaderRead` transition after the tracker already records `ShaderRead`.
2. **Given** a threaded deferred frame writes a sampled depth attachment, **When** the depth texture is later read or extracted, **Then** the submitted command buffers do not contain a stale `DepthWrite -> ShaderRead` transition after the tracker already records `DepthRead` or `ShaderRead`.

### Edge Cases

- Extraction textures that were not already transitioned by a pass still need a visibility transition to shader read.
- Swapchain output and non-extracted internal resources must keep their existing barrier behavior.
- Parallel command translation may insert resource visibility batches before dependent passes; those batches must remain ordered before the dependent command buffer.

## Requirements

### Functional Requirements

- **FR-001**: The RHI threaded rendering path MUST avoid submitting stale texture transition barriers whose requested `before` state conflicts with the resource state tracker's current state.
- **FR-002**: Post-pass extraction visibility MUST not emit redundant non-UAV texture transitions when the resource tracker already records the requested destination state.
- **FR-003**: Dependency visibility between GBuffer and lighting work units MUST continue to be emitted when a later pass reads a texture written by an earlier pass.
- **FR-004**: The fix MUST preserve existing parallel command translation behavior for command buffer ordering and telemetry.
- **FR-005**: The fix MUST be covered by a targeted unit regression test before production code changes.

## Success Criteria

### Measurable Outcomes

- **SC-001**: The targeted threaded rendering lifecycle regression test fails before the fix and passes after the fix.
- **SC-002**: Existing threaded rendering lifecycle tests covering parallel recording, translation merge, external output, and compute visibility continue to pass.
- **SC-003**: DX12 deferred editor frames no longer trigger debug-layer message 527 for MultiFramebuffer color/depth state mismatch under the reported scenario.

## Assumptions

- The reported failure occurs on Windows DX12 with the explicit RHI resource state tracker enabled.
- Unit validation can exercise the command buffer/barrier planning logic even when a local DX12 runtime capture is unavailable.
- RenderDoc or live DX12 verification should be used later if the local environment can run the editor with the reported scene.
