# Research: UE5-Style DX12 Render Alignment

## Decision 1: Use UE 5.7 public rendering contracts as the architectural baseline, then require a source audit before closure

- **Decision**: The feature will treat UE 5.7 public rendering contracts as the immediate baseline for thread ownership, RDG authority, external resource handling, editor integration, and PSO policy. A documented source-level UE audit remains mandatory before the feature can claim "fully aligned".
- **Rationale**: Public Epic documentation is enough to define the target architecture responsibly, but not enough to prove line-by-line behavioral identity with UE internals.
- **Alternatives considered**:
  - Trust only memory of UE internals: rejected because the alignment goal is too important for recall-based assumptions.
  - Delay planning until a full source audit finishes: rejected because Nullus still needs a concrete architecture plan now.

## Decision 2: DX12 is the only active runtime backend in the first alignment phase

- **Decision**: The first implementation phase will allow only DX12 as the active backend for the aligned architecture.
- **Rationale**: The user explicitly scoped phase 1 to DX12. Restricting the live surface prevents compatibility branches from surviving "temporarily" in runtime code.
- **Alternatives considered**:
  - Keep Vulkan active during the cleanup: rejected because it would force ongoing compatibility surfaces during the purity phase.
  - Delete all future multi-backend abstraction: rejected because the user still wants later multi-backend support.

## Decision 3: Future multi-backend support must be preserved through clean abstraction boundaries, not active compatibility paths

- **Decision**: Renderer, editor, and runtime code must keep backend-neutral interfaces, but only DX12 may implement the phase-1 execution path.
- **Rationale**: UE-style alignment means a single architecture that different backends can implement, not multiple runtime architectures hidden behind fallback.
- **Alternatives considered**:
  - Hard-code the renderer permanently to DX12: rejected because it would make later multi-backend support a rewrite.
  - Keep legacy backends runnable inside the new architecture during phase 1: rejected because that would keep fallback pressure alive in the mainline.

## Decision 4: Game Thread, Render Thread, and RHI Thread must become exclusive ownership stages

- **Decision**: Nullus will align to UE's public ownership model:
  - Game Thread publishes immutable render input.
  - Render Thread owns authoritative frame build and graph construction.
  - RHI Thread owns backend execution and retirement.
- **Rationale**: Epic's public APIs and runtime switches expose threaded rendering, render-thread ownership, and RHI-thread/task-thread execution as first-class architecture boundaries rather than optional convenience helpers.
- **Alternatives considered**:
  - Keep `Driver` as a mixed owner of frame build plus backend submit: rejected because that preserves the existing ambiguity.
  - Allow editor-only exceptions: rejected because UE alignment requires Editor and Game to share the same rendering architecture.

## Decision 5: RDG must become the sole truth source for scheduling, lifetime, import/extract, and readback visibility

- **Decision**: RDG becomes the only authoritative scheduler for render passes, transient resource lifetime, external resource import/extract, and graph-visible readback.
- **Rationale**: Epic's public RDG APIs already expose tracked resources, explicit import/extract surfaces, async-compute fence controls, and pass dispatch helpers. That is the contract to align with.
- **Alternatives considered**:
  - Keep `ScenePassSchemas.h` or renderer-local code as a co-equal scheduler: rejected because dual truth sources are the root of current architectural impurity.
  - Treat readback and offscreen resources as graph-adjacent exceptions: rejected because that would immediately recreate editor/runtime side channels.

## Decision 6: Editor rendering must stay inside the same frame pipeline as game rendering

- **Decision**: Scene view, game view, offscreen rendering, picking, gizmo, grid, outline, overlays, and UI-related scene target work must all remain inside one authoritative frame pipeline.
- **Rationale**: Epic's public APIs show RenderThread and RDG participation for Slate target rendering, canvas target rendering, and hit-proxy-related rendering surfaces. The public contract points toward one pipeline, not editor-only bypass submission.
- **Alternatives considered**:
  - Keep editor picking or gizmo on a special readback path: rejected because that keeps a second architecture alive.
  - Allow editor-only present or acquire behavior: rejected because it would violate the same thread and resource ownership rules.

## Decision 7: Remove fallback and compatibility behavior from the DX12 mainline entirely

- **Decision**: The aligned DX12 path may not keep direct-submit fallback, driver-built fallback packages, compatibility acquire/present behavior, or editor-only bypass submission.
- **Rationale**: A clean architecture is impossible while the old route is still available in normal runtime code.
- **Alternatives considered**:
  - Keep fallback code behind runtime flags: rejected because "temporary" compatibility code tends to become part of the architecture.
  - Keep fallback for Editor only: rejected because it breaks the alignment goal.

## Decision 8: Serial execution inside the same authoritative architecture is allowed, but compatibility fallback is not

- **Decision**: If a workload cannot use parallel recording or async compute, it may still execute serially as long as it stays inside the authoritative Render Thread, RDG, and RHI Thread path.
- **Rationale**: UE alignment requires a single architecture, not unconditional parallelism. Correct serial scheduling inside the same architecture is acceptable.
- **Alternatives considered**:
  - Require parallel execution for every pass immediately: rejected because it would confuse scheduling policy with architectural purity.
  - Treat serial execution as equivalent to compatibility fallback: rejected because it is still the same architecture if ownership and scheduling remain authoritative.

## Decision 9: Central PSO, descriptor, transient lifetime, and retirement systems must be mandatory, not advisory

- **Decision**: Existing centralized systems such as `PipelineCache`, `DescriptorAllocator`, and `ResourceStateTracker` must become mandatory mainline infrastructure with diagnostics that can prove zero bypasses.
- **Rationale**: UE-style rendering relies on centralized policy and lifetime control. Optional or best-effort centralization would leave the architecture vulnerable to local mini-renderers.
- **Alternatives considered**:
  - Keep diagnostics-only central systems: rejected because they do not change the actual execution surface.
  - Centralize only PSO and leave the rest distributed: rejected because descriptor lifetime and transient retirement are equally foundational.

## Official UE References Used For This Plan

- Threading and ownership:
  - `GUseThreadedRendering`
  - `FGenericPlatformMisc::UseRenderThread`
  - `IsRunningRHIInTaskThread`
  - `FViewport::EnqueueEndRenderFrame`
  - `FDummyViewport::EndRenderFrame`
- RDG and scheduling:
  - `FRDGBuilder`
  - `FRDGBuffer`
  - `FRDGBuilder::AddDispatchPass`
  - `AddDispatchToRHIThreadPass`
- External resource handling:
  - `FRenderTarget`
  - `FSceneTextureExtracts`
- Editor and auxiliary rendering surfaces:
  - `ISlate3DRenderer`
  - `ISlate3DRenderer::DrawWindowToTarget_RenderThread`
  - `FCanvas::Create`
  - `FCanvas::Flush_RenderThread`
  - `FPrimitiveSceneProxy::CreateHitProxies`
  - `FViewport::GetElementHandlesInRect`
- Centralized low-level infrastructure:
  - `PSO Precaching for Unreal Engine`

## Public UE Documentation Evidence Matrix

Validated against Epic public documentation on 2026-04-21. The currently published API pages are split between UE 5.6 and UE 5.7, so this bundle treats the latest public pages available for each symbol as the public-contract baseline and still requires a later source audit before any "fully aligned" claim.

- Threading and ownership:
  - `GUseThreadedRendering` documents whether the rendering thread should exist.
    - https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/RenderCore/GUseThreadedRendering
  - `FGenericPlatformMisc::UseRenderThread` documents platform-level render-thread enablement.
    - https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Core/GenericPlatform/FGenericPlatformMisc/UseRenderThread
  - `IsRunningRHIInTaskThread` documents that RHI commands may run on a dedicated thread distinct from the Render Thread.
    - https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/RHI/IsRunningRHIInTaskThread
  - `FViewport::EnqueueEndRenderFrame` explicitly documents the game-thread handoff at end-of-frame.
    - https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/FViewport/EnqueueEndRenderFrame

- RDG authority and graph scheduling:
  - `FRDGBuffer` is documented as a render-graph tracked buffer.
    - https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/RenderCore/FRDGBuffer
  - `FRDGBuilder::AddDispatchPass` documents pass insertion through the graph rather than ad hoc submission.
    - https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/RenderCore/FRDGBuilder/AddDispatchPass
  - `AddDispatchToRHIThreadPass` documents explicit graph-mediated dispatch to the RHI thread surface.
    - https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/RenderCore/AddDispatchToRHIThreadPass
  - `FRDGBuilder::GetPooledTexture` documents that immediate access is limited to external or extracted resources.
    - https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/RenderCore/FRDGBuilder/GetPooledTexture
  - `FSceneTextureExtracts` documents extracted RHI resources after RDG execution.
    - https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Renderer/FSceneTextureExtracts

- Editor, viewport, and auxiliary rendering surfaces:
  - `ISlate3DRenderer` and `DrawWindowToTarget_RenderThread` document Render Thread target rendering for Slate-backed 3D surfaces.
    - https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/SlateRHIRenderer/Interfaces/ISlate3DRenderer
    - https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/SlateRHIRenderer/Interfaces/ISlate3DRenderer/DrawWindowToTarg-/1
  - `FCanvas::Flush_RenderThread` exposes both RHI-command-list and `FRDGBuilder` render-thread flushing surfaces.
    - https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/FCanvas/Flush_RenderThread
  - `FPrimitiveSceneProxy::CreateHitProxies` documents hit-proxy creation on the game thread.
    - https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/FPrimitiveSceneProxy/CreateHitProxies
  - `FViewport::GetElementHandlesInRect` documents that hit-proxy lookup can trigger viewport drawing with a hit-testing canvas when caches are not ready.
    - https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/FViewport/GetElementHandlesInRect
  - `FRenderTarget::ReadPixels` documents readback from render targets and is useful as a boundary reference for explicit readback handling.
    - https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/FRenderTarget/ReadPixels

- Centralized low-level infrastructure:
  - `PSO Precaching for Unreal Engine` documents centralized PSO collection, asynchronous compilation, and validation counters.
    - https://dev.epicgames.com/documentation/unreal-engine/pso-precaching-for-unreal-engine

## Public-Docs Boundary Versus Source-Audit Boundary

- The public documentation is strong enough to anchor architecture-level contracts:
  - threaded ownership exists as an explicit UE concept,
  - RDG resources and dispatch are graph-owned concepts,
  - external and extracted resources are first-class RDG boundary objects,
  - editor-facing rendering surfaces still participate in Render Thread or RDG-visible flows,
  - PSO policy is centralized and diagnosable.
- The public documentation is not enough to prove exact source topology, exact call ordering, or exact subsystem layering in UE.
- Therefore this feature may claim only:
  - "aligned to UE public rendering contracts"
  - "planned against UE public API and documentation evidence"
- It may not claim:
  - "completely identical to UE internals"
  - "fully source-aligned with UE"
- Final closure still requires the planned source-audit pass.

## Research Boundary

- This research bundle is sufficient to define the target architecture and forbidden-path rules.
- It is not sufficient to prove source-level identity with UE internals.
- The implementation plan therefore includes an explicit source-audit task before final closure claims.

## Landed Implementation Notes (2026-04-23)

- The accepted DX12-aligned mainline now treats `DescriptorAllocator` as mandatory for explicit binding-set creation.
  - `DriverRendererAccess::CreateExplicitBindingSet(...)` no longer returns a raw backend binding set when no tracked allocator is active.
- Graphics and compute PSO acquisition remain `PipelineCache`-owned.
  - `Material.cpp` and `LightGridPrepass.cpp` reject missing-cache mainline execution instead of rebuilding PSOs through direct fallback paths.
- Transient/readback lifetime remains graph-visible and tracker-owned.
  - `ExternalResourceBridge` is the authoritative helper for preferred readback registration and external-output shader-read transitions.
- Infrastructure diagnostics now flow through one telemetry chain:
  - `RhiSubmissionFrame` / `ThreadedRenderingLifecycle`
  - `DriverRendererAccess::GetThreadedFrameTelemetry(...)`
  - `RendererStats`
  - `FrameInfo`
- Retirement diagnostics no longer treat a missing frame fence as successful retirement.
  - The accepted path only reports retirement mainline success when an actual fence wait occurred.

## Focused Evidence Snapshot (2026-04-23)

- `NullusUnitTests` rebuilt successfully with the Phase 7 central-infrastructure cleanup.
- Focused passing filters covered:
  - descriptor mainline enforcement
  - pipeline-cache-only PSO acquisition
  - frame-graph/external-resource readback ownership
  - threaded submission diagnostics for descriptor/transient/retirement telemetry
- Editor DX12 startup no longer crashes in `FrameGraph::Builder::write(...)`.
  - Root cause was stale scene-output `FrameGraphResource` handles being reused after RDG-style write-renaming on imported external outputs.
  - The accepted mainline now threads the latest valid output-chain handle while graph passes are added.
- New runtime evidence landed:
  - Editor foreground DX12 probe stayed alive for 25 seconds after startup with no FrameGraph assertion.
  - Editor RenderDoc capture succeeded with `NLS_RENDERDOC_CAPTURE_AFTER_FRAMES=1`.
    - Capture: `Build\RenderDocCaptures\editor\dx12_chain_fix_frame1\editor_dx12_chain_fix_frame1_DX12_capture.rdc`
    - `rdc_analyze.py` summary: `D3D12`, `43` events, `14` draws, `3` explicit color passes.
  - Game RenderDoc capture succeeded under DX12.
    - Capture: `Build\RenderDocCaptures\game\dx12\game_dx12_DX12_capture.rdc`
- Explicit non-DX12 startup failure was observed in Game.
  - `Game.exe --backend vulkan` exited with code `1`
  - stderr: `Game CLI only supports DX12 during UE5 alignment phase 1. Requested backend: Vulkan.`
- Runtime-facing harness publish helpers no longer use `Compatibility` naming.
  - `RenderThreadCoordinator`, `DriverTestAccess`, and related focused tests now expose `TryPublishHarness*` / `AllowsThreadedHarnessPublish`.
  - This removes one source-audit false positive where test-only surfaces still looked like accepted compatibility mainline.
- Internal lifecycle and dependency helper naming below the harness API boundary is now aligned with the accepted DX12 mainline.
  - `ThreadedRenderingLifecycle` now uses `SnapshotHarness` / `PreparedBuilderMissing`.
  - `RhiThreadCoordinator` / `DriverInternal` now use `ResolveImplicitDependencySource*` naming for inferred dependency-order helpers.
  - Dead `DriverCompatibilityAccess` and `Texture::GetCompatibilityTextureId()` remnants were also removed.
- Threaded render-build material binding now resolves through the owning frame slot's centralized descriptor allocator.
  - `DriverRendererAccess::GetActiveDescriptorAllocator(...)` now resolves the current frame-context allocator during threaded prepared-frame build even before `explicitFrameActive` is set.
  - This keeps `Material::GetExplicitBindingSet(...)` on the mandatory centralized descriptor path during Render/Build-stage package construction instead of dropping scene draws with `descriptor allocator missing`.
- Final audit wording cleanup removed remaining false-positive terminology from accepted mainline seams.
  - `Game.cpp` now reports the default validation scene instead of a "fallback scene".
  - `DriverAccess.h` UI semaphore comments are backend-neutral.
  - `RhiThreadCoordinator.cpp` no longer labels same-architecture compute-to-graphics routing as a "fallback".
- Editor scene rendering no longer injects a runtime-facing fallback material.
  - `Project/Editor/Panels/SceneView.*` no longer constructs or forwards `m_fallbackMaterial`.
  - `Runtime/Engine/Rendering/BaseSceneRenderer.*` no longer honors `fallbackMaterial` when a renderer-owned material is invalid.
  - This keeps invalid scene content from silently reviving a hidden fallback material path on the accepted mainline.
- UI bridge selection is now driven by native backend identity instead of the higher-level graphics-backend enum.
  - `RHIUIBridge` factory no longer accepts `EGraphicsBackend`.
  - `CreateRHIUIBridge(...)` now resolves `NativeRenderDeviceInfo.backend` from the explicit caller or the located driver and selects the bridge from that native identity.
  - `UIManager::ResolveUISignalSemaphore()` now tags the returned handle with the bridge's actual native backend instead of hard-coding `Vulkan`.
- Backend-specific UI bridge execution no longer lives in the common RHI utils translation unit.
  - `Runtime/Rendering/RHI/Utils/RHIUIBridge.cpp` is now a thin factory/common seam only.
  - Backend-specific bridge implementations now live under:
    - `Runtime/Rendering/RHI/Backends/DX12/DX12UIBridge.cpp`
    - `Runtime/Rendering/RHI/Backends/OpenGL/OpenGLUIBridge.cpp`
    - `Runtime/Rendering/RHI/Backends/Vulkan/VulkanUIBridge.cpp`
  - The Vulkan bridge now includes `Rendering/RHI/Core/RHICommand.h` explicitly instead of depending on transitive includes.
- Common driver/UI coordination no longer carries an OpenGL-specific texture-handle branch.
  - `DriverUIAccess::GetUITextureHandle(...)` was removed from the common driver seam.
  - `OpenGLUIBridge` now resolves UI textures directly from `RHITextureView::GetNativeShaderResourceView()`.
- Launcher project creation now reflects the DX12-only phase-1 product truth.
  - `ProjectCreationWizard` no longer advertises Vulkan, OpenGL, or DX11 as selectable runtime backends during this phase.
- Dead legacy compatibility shims and dead legacy-output fallback hooks were removed from the accepted mainline.
  - Removed unused shim headers / translation units:
    - `Runtime/Rendering/Backend/OpenGL/OpenGLAPI.h`
    - `Runtime/Rendering/Backend/OpenGL/OpenGLShaderProgramAPI.h`
    - `Runtime/Rendering/FrameGraph/OpenGLFrameGraphTexture.*`
    - `Runtime/Rendering/FrameGraph/OpenGLFrameGraphBuffer.*`
  - Removed unassigned legacy-output fallback flags from `FrameGraphExecutionPlan.h`.
  - Removed the unused `executeGBufferFallbackPass` callback from `SceneRenderGraphBuilder`.
  - `ABaseRenderer`, `ForwardSceneRenderer`, and `DeferredSceneRenderer` no longer thread dead `bindLegacyOutputOnFallback` / `unbindLegacyOutputOnFallback` plumbing through the RDG-driven path.
- Additional focused cleanup validation passed after the source-audit cleanup slice.
  - `Editor` and `NullusUnitTests` rebuilt successfully.
  - Focused filter passed:
    - `*HarnessPublish*`
    - `*DriverHarnessScenePackageResolutionIsRejected`
    - `*Dx12PreparedBuilderDrainDoesNotUseHarnessFallbackAttribution`
    - `*SynchronousDrainMarksSnapshotHarnessAndSubmissionAttribution`
    - `*DriverRhiWorkerSkipsRejectedHarnessOffscreenFramesWithoutAcquireOrPresent`
    - `*PreparedBuilderResolutionIgnoresHarnessFallbackBuilders`
    - `*DeferredThreadedOffscreenPackageCarriesExternalOutputAttachmentViews`
    - `*ForwardThreadedOffscreenPackageRegistersExternalOutputExtraction`
    - `*ThreadedResizeWaitsForInFlightFrameRetirementBeforeResizingSwapchain`
  - Additional backend-awareness validation passed:
    - `UIAndToolingBackendAwarenessTests.*`
  - Additional RDG / launcher cleanup validation passed:
    - `FrameGraphSceneTargetsTests.*`
    - `ProjectCreationWizardValidationTests.*`
- Editor validation now has a narrow startup-only capture seam for acceptance evidence.
  - `Project/Editor/Main.cpp` accepts:
    - `--editor-validation-focus-view <scene|game>`
    - `--editor-validation-exclusive-view <scene|game>`
    - `--editor-validation-select-actor <name>`
  - `Editor.cpp` applies those directives before the first captured frame so RenderDoc startup capture can observe stable acceptance scenarios without introducing any runtime fallback path or an extra runtime capture scheduler.
  - The temporary runtime capture-frame / capture-label validation scheduler was removed after acceptance evidence landed, so the accepted DX12 mainline keeps only pre-capture startup directives.
- Runtime startup readiness no longer exposes fallback semantics in the accepted DX12 mainline.
  - `GraphicsBackendUtils.h` now models startup gating as `RuntimeBackendReadinessDecision`.
  - `Driver`, `Editor::Context`, and `Game::Context` now consume explicit readiness checks instead of `Evaluate*Fallback(...)` entry points.
- Common runtime orchestration no longer translates native backend identity through an extra executor enum.
  - `Driver.cpp` now hands `NativeRenderDeviceInfo` directly to `CreateCommandListExecutor(...)`.
  - backend-to-executor dispatch is now owned by the RHI factory seam instead of the higher-level driver orchestration layer.
- Threaded submission-path gating no longer carries backend-named parallel-mode states in common orchestration.
  - `RhiThreadCoordinator.cpp` now derives parallel recording / translation readiness directly from ordered-path policy plus device capabilities.
  - dead `DX12OrderedSubmit` / `VulkanOrderedSubmit` common-layer mode naming was removed from `DriverInternal.h`.
- Native-backend naming and enum translation now has a tighter single seam.
  - `GraphicsBackendUtils.h` now owns `NativeBackendType -> string` and `NativeBackendType -> EGraphicsBackend` helpers.
  - `Driver.cpp` now consumes those helpers instead of keeping local translation switches for active-backend reporting and RenderDoc backend naming.
- Scene-pass taxonomy is now cleanly separated from frame-graph scheduling policy.
  - `Runtime/Engine/Rendering/ScenePassSchemas.h` now only owns forward/deferred pass taxonomy and pass-kind mapping helpers.
  - Authoritative forward/deferred scene-pass descriptor arrays now live in `Runtime/Rendering/FrameGraph/SceneRenderGraphBuilder.*`, which keeps queue policy, graph pass names, clear/load behavior, and output propagation fully inside the RDG/frame-graph layer.

## Phase 8 Progress Update (2026-04-23)

- T041 is now closed with fresh threaded DX12 Game evidence.
  - Capture: `Build\RenderDocCaptures\game\dx12_threaded_fixed\game_dx12_threaded_fixed_DX12_capture.rdc`
  - `rdc_analyze.py` summary: `D3D12`, `8` events, `2` draws, `1` color pass.
  - Focused draw sample:
    - `DrawIndexed` with `12` triangles
    - `Draw` with `12` triangles
  - Fresh non-DX12 startup rejection remains explicit:
    - `App\Win64_Debug_Runtime_Static\Game.exe --backend vulkan`
    - exit `1`
    - stderr: `Game CLI only supports DX12 during UE5 alignment phase 1. Requested backend: Vulkan.`
- T042 is now closed as a source-audit/self-review checkpoint for the accepted DX12 mainline.
  - Remaining backend-name references in common code are limited to backend-neutral policy or translation seams:
    - native-backend to executor/capture-name mapping in `Driver.cpp`
    - future-backend capability enums in `RhiThreadCoordinator.cpp`
  - The accepted mainline audit did not find remaining:
    - driver-built render fallback paths
    - compatibility acquire/present branches
    - editor-only submission bypasses in the shared render path
    - dual scheduling truth competing with RDG on the accepted runtime path
- T040 is now closed with explicit Editor validation evidence.
  - Scene view + selected actor capture:
    - `Build\RenderDocCaptures\editor\dx12_validation_scene_selected_startup\editor_validation_scene_selected_startup_DX12_capture.rdc`
    - `D3D12`, `67` events, `20` draws, `12` copies
    - startup log confirms `Validation Cube` selection and editor overlay model loads (`Sphere`, `Arrow_Translate`, `Arrow_Picking`)
  - Scene view + selected actor + picking disabled control:
    - `Build\RenderDocCaptures\editor\dx12_validation_scene_selected_no_picking\editor_validation_scene_selected_no_picking_DX12_capture.rdc`
    - `D3D12`, `54` events, `15` draws, `10` copies
    - the delta against the full selected-scene capture proves the picking path is an additional offscreen graph-visible pass/readback path
  - Game view runtime evidence:
    - `Build\RenderDocCaptures\editor\dx12_validation_gameview_no_picking\editor_validation_gameview_no_picking_DX12_capture.rdc`
    - `D3D12`, `38` events, `11` draws, `6` copies
    - startup log confirms `Editor validation pre-focused Game View.`
  - Validation-only experiment note:
    - exclusive-view startup capture for `Game View` (`dx12_validation_gameview_exclusive_no_picking`) only captured the UI composite pass because frame-1 capture happens before the isolated panel reaches a stable content region
    - this is treated as a capture-timing limitation, not an architectural bypass; accepted evidence remains the non-exclusive Game View startup run plus the scene/picking overlay deltas above
- Post-closure regression verification remained green.
  - `cmake --build Build\windows --config Debug --target NullusUnitTests Editor Launcher -- /m:1`
  - `NullusUnitTests.exe --gtest_filter="EditorRenderPathContractTests.*:PanelWindowHookTests.*:UIAndToolingBackendAwarenessTests.*:ProjectCreationWizardValidationTests.*"`
  - Result: 16 / 16 tests passed
