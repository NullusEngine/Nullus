# Feature Specification: Multi-Backend RHI Mainline

**Feature Branch**: `multi-backend-rhi-mainline`  
**Created**: 2026-03-27  
**Status**: Draft  
**Input**: User description: "Complete the formal RHI so DX12 and Vulkan are full explicit backends, DX11 and OpenGL are supported through the formal RHI entry, renderer/framegraph/material mainline no longer depends on legacy immediate interfaces, and Editor/Game remain runnable with correct rendering."

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Run The Main Rendering Path Only Through Formal RHI (Priority: P1)

As a rendering developer, I need the renderer, frame graph, material system, wrappers, and driver mainline to consume the formal RHI contract instead of the legacy immediate device API so the engine has one real rendering architecture instead of two overlapping ones.

**Why this priority**: This is the architectural core of the change. If the mainline still depends on `IRenderDevice`, backend work remains a partial refactor rather than a complete RHI.

**Independent Test**: Build and run the main renderer on a Tier A backend, inspect the driver and renderer code paths, and confirm the frame lifecycle executes through `RHIDevice`, `RHICommand*`, `RHIPipeline*`, `RHIBinding*`, and `RHIResource*` objects without requiring the renderer mainline to call legacy `IRenderDevice` drawing or binding APIs.

**Acceptance Scenarios**:

1. **Given** a renderer frame running on the formal RHI path, **When** the driver creates the device, frame context, swapchain, and command submission objects, **Then** the renderer mainline consumes formal RHI objects rather than legacy immediate draw state.
2. **Given** material and frame graph work for a draw pass, **When** pipeline, binding, and resource objects are produced, **Then** the mainline uses `RHIGraphicsPipelineDesc`, `RHIPipelineLayout`, `RHIBindingSet`, `RHIBuffer`, `RHITexture`, and `RHITextureView` instead of `GraphicsPipelineDesc` and `BindingSetInstance`.
3. **Given** the compatibility layer still exists for limited backends, **When** the formal RHI mainline executes on a Tier A backend, **Then** it does not route through `CreateCompatibilityExplicitDevice`.

---

### User Story 2 - Support Multiple Backends Under One RHI Contract (Priority: P2)

As an engine maintainer, I need `DX12`, `Vulkan`, `DX11`, and `OpenGL` to all enter through the same formal RHI API surface so backend differences are explicit capability differences instead of hidden architecture forks.

**Why this priority**: A complete RHI must support multiple rendering backends through one contract. Without this, the engine still has backend-specific architecture drift.

**Independent Test**: Create the formal RHI device for each supported backend, launch a focused smoke workload, and confirm all four backends are created from the same `Driver` and `RHIDevice` entry points while exposing capability differences through documented backend tiers rather than renderer-side branching.

**Acceptance Scenarios**:

1. **Given** `DX12` or `Vulkan` is selected, **When** `ExplicitDeviceFactory` creates the backend device, **Then** it returns a backend-owned formal `RHIDevice` implementation instead of a native-named wrapper around `CreateCompatibilityExplicitDevice`.
2. **Given** `DX11` or `OpenGL` is selected, **When** the engine creates the backend through the same driver entry, **Then** the renderer still receives formal RHI objects and does not need to reintroduce legacy mainline paths.
3. **Given** backend capabilities differ, **When** a backend cannot match Tier A behavior, **Then** the engine records the limitation in a capability matrix instead of silently assuming equivalence.

---

### User Story 3 - Keep Editor And Game Runnable With Correct Rendering (Priority: P3)

As a contributor validating the RHI transition, I need both the Editor and Game to run on the new formal RHI paths with correct rendering output so the work is proven in the real products, not only in synthetic demos.

**Why this priority**: The change is only complete if real application paths work. Demos alone do not prove the engine or editor remain usable.

**Independent Test**: Launch both Editor and Game on each targeted backend tier, verify swapchain creation, default resource loading, frame execution, and present, then gather RenderDoc or focused runtime evidence that key passes and outputs are correct.

**Acceptance Scenarios**:

1. **Given** the Editor launches on a supported backend, **When** it initializes the driver and scene renderer, **Then** it reaches a stable render loop with correct swapchain creation, resource loading, and present.
2. **Given** the Game launches on a supported backend, **When** it renders representative content, **Then** the scene output matches expected geometry, material, depth, and texture behavior without requiring legacy fallback rendering paths.
3. **Given** a Tier A backend validation run, **When** the frame is captured, **Then** the recorded evidence confirms expected pass order, resource transitions, pipeline binding, and final presentation.

---

### User Story 4 - Ship A Minimum Delivery Surface For Ongoing Backend Work (Priority: P4)

As a maintainer, I need demos, correctness tests, backend documentation, and a capability matrix so the multi-backend RHI can be validated, extended, and reviewed without rediscovering backend limits by trial and error.

**Why this priority**: Once the architecture lands, the project still needs a repeatable delivery and validation surface to keep the backends healthy.

**Independent Test**: Run the smoke demos and targeted correctness checks, then read the backend documentation and capability matrix to confirm supported paths, known gaps, and validation evidence are all explicit.

**Acceptance Scenarios**:

1. **Given** the minimum demo set is available, **When** a contributor runs `Triangle`, `TexturedQuad`, `OffscreenRender`, and `ComputeClear`, **Then** each demo states which backends are expected to pass and what limitations apply.
2. **Given** targeted correctness tests exist, **When** upload, sync, barrier, acquire-present, and readback paths are exercised, **Then** failures can be localized to a backend or subsystem instead of being discovered only through product regressions.
3. **Given** the documentation bundle is reviewed, **When** a maintainer checks backend tiers and smoke coverage, **Then** they can tell what was validated, what remains limited, and which evidence supports each claim.

### Edge Cases

- What happens when a Tier B backend receives a formal RHI request for a capability it cannot faithfully represent, such as a barrier or binding model that does not map one-to-one?
- What happens when `Editor` or `Game` starts successfully on a backend but renders incorrectly because default resource loading, offscreen passes, or pipeline state translation diverge from Tier A behavior?
- What happens when `DX12` and `Vulkan` both use the formal RHI mainline but only one backend passes swapchain resize, synchronization, or resource transition validation?
- How does the engine report unsupported or degraded behavior on `DX11` and `OpenGL` without forcing renderer mainline code to branch back into legacy immediate APIs?
- What validation evidence is required when RenderDoc is unavailable or less useful for a backend, and how is that lower-confidence verification recorded?

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The engine MUST define one formal rendering mainline built on `RHI/Core` contracts and use that mainline for renderer, frame graph, material, wrappers, and driver integration.
- **FR-002**: `Driver` MUST remain the unified entry point for backend selection, device creation, swapchain management, frame lifecycle management, and formal RHI frame context ownership.
- **FR-003**: `DX12` and `Vulkan` MUST be implemented as Tier A backends that directly own formal `RHIDevice`, `RHIQueue`, `RHISwapchain`, `RHIFence`, `RHISemaphore`, `RHICommandPool`, and `RHICommandBuffer` behavior without routing Tier A execution through `CreateCompatibilityExplicitDevice`.
- **FR-004**: Tier A backends MUST provide formal RHI implementations for pipeline, binding, resource, barrier, copy, render-pass, and upload flows needed by the renderer mainline.
- **FR-005**: `DX11` and `OpenGL` MUST be supported as Tier B backends through the same formal RHI entry surface used by Tier A backends.
- **FR-006**: Tier B backends MAY translate formal RHI objects through compatibility-style implementations internally, but the renderer, frame graph, material system, and driver mainline MUST NOT require separate legacy rendering paths to use them.
- **FR-007**: The renderer mainline MUST stop depending on `GraphicsPipelineDesc` and `BindingSetInstance` as its primary pipeline and binding abstractions.
- **FR-008**: `Material` MUST produce formal RHI pipeline and binding objects suitable for the renderer mainline without requiring legacy binding creators for Tier A execution.
- **FR-009**: `FrameGraph` and rendering wrappers MUST operate on formal RHI resource, view, barrier, and render-pass abstractions as their main path.
- **FR-010**: `IRenderDevice` MUST be downgraded from renderer main interface to backend-internal compatibility infrastructure used only where transition support still requires it.
- **FR-011**: The change MUST define explicit backend tiers and a capability matrix that records supported features, degraded features, and unsupported features for `DX12`, `Vulkan`, `DX11`, and `OpenGL`.
- **FR-012**: The project MUST provide a minimum demo set containing `Triangle`, `TexturedQuad`, `OffscreenRender`, and `ComputeClear` to exercise the formal RHI surface.
- **FR-013**: The project MUST provide targeted correctness validation for upload, synchronization, barriers, acquire-present, readback, and pipeline binding behavior.
- **FR-014**: `Editor` and `Game` MUST both run through the formal RHI mainline on supported backends, including successful startup, swapchain creation, default resource loading, frame execution, and present.
- **FR-015**: Validation MUST include rendering correctness evidence for representative geometry, materials, textures, depth or stencil behavior, offscreen passes, and final presentation.
- **FR-016**: Tier A rendering validation MUST prefer RenderDoc or equivalent frame-level evidence and MUST record which backend was captured.
- **FR-017**: Tier B validation MUST at least include focused runtime verification and MUST document when evidence is weaker than Tier A frame capture evidence.
- **FR-018**: Documentation for this feature MUST state the tested backend, evidence gathered, known unverified areas, and backend-specific caveats instead of implying one successful backend proves all others.

### Key Entities *(include if feature involves data)*

- **Formal RHI Mainline**: The engine path from `Driver` through renderer, frame graph, material, and wrapper code that consumes only the formal `RHI/Core` contracts.
- **Backend Tier**: A declared support level for a backend, where Tier A backends are full explicit implementations and Tier B backends are formal-entry implementations with documented capability limits.
- **Compatibility Infrastructure**: Backend-internal bridging code used to adapt a formal RHI object model onto legacy or constrained backend behavior without re-exposing the legacy API to the renderer mainline.
- **Capability Matrix**: The maintained record of supported, degraded, and unsupported formal RHI capabilities for each backend.
- **Validation Evidence**: Concrete proof for the feature, including RenderDoc captures, focused runtime observations, smoke demo results, and correctness test output tied to a backend.
- **Product Runtime Validation**: Evidence that `Editor` and `Game` both run correctly on the formal RHI mainline rather than on synthetic or legacy-only paths.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: The renderer, frame graph, material system, wrappers, and driver mainline no longer require legacy renderer-side calls to `IRenderDevice` draw and binding APIs for Tier A execution.
- **SC-002**: `DX12` and `Vulkan` formal device creation no longer depends on `CreateCompatibilityExplicitDevice` in their primary execution path.
- **SC-003**: `DX11` and `OpenGL` can each create a formal RHI device through the same driver entry used by Tier A backends and run the engine without reintroducing a separate legacy renderer mainline.
- **SC-004**: `Editor` and `Game` both complete at least one documented smoke run on each supported backend with recorded startup, render-loop, and present outcomes.
- **SC-005**: Representative rendering correctness evidence exists for every supported backend tier, and Tier A evidence includes at least one backend-specific frame capture showing expected pass order and resource usage.
- **SC-006**: The repository contains a backend capability matrix and a smoke matrix covering the minimum demo set and product runtime validation status.
- **SC-007**: A reviewer can read the feature bundle and determine, in under ten minutes, which backends are Tier A versus Tier B, what is fully supported, what remains degraded, and what evidence was gathered.

## Assumptions

- `Metal` is outside the scope of this feature and will not block completion of this multi-backend RHI milestone.
- The existing CMake-based build and test responsibilities remain in place; this feature extends them instead of inventing a parallel workflow.
- `Runtime/*/Gen/` remains generated output and will not be hand-edited as part of this work.
- `DX12` and `Vulkan` are the only backends required to reach full explicit Tier A behavior in this milestone.
- `DX11` and `OpenGL` are allowed to retain backend-internal translation or compatibility strategies as long as the renderer mainline continues to consume only the formal RHI contracts.
- RenderDoc remains the preferred rendering evidence path where supported, but some Tier B verification may rely on focused runtime checks with clearly documented confidence limits.
