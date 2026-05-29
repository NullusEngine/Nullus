# Implementation Plan: Editor Scene View CPU Frame-Time Optimization

**Branch**: `037-scene-view-cpu` | **Date**: 2026-05-27 | **Spec**: [spec.md](spec.md)  
**Input**: Feature specification from `specs/037-scene-view-cpu/spec.md`

## Summary

The provided baseline trace shows editor main-thread frames averaging about 47.5 ms, with most time under Scene View rendering. The first implementation slice reduces redundant stable-size deferred renderer preparation by reusing already-created GBuffer targets and wrapped texture resources until dimensions or attachments change. A second slice makes trace export ignore incomplete events so future performance evidence is not distorted by invalid durations. Follow-up traces identified `Debug GameObject` as the next coarse hotspot, so the selected-object slice adds child profiler scopes, gates invisible debug draw recursion, avoids an empty output render pass in the threaded path, reuses current-frame selected-tree mesh/transform data for bounds and outline capture, reserves predictable outline command storage, skips non-threaded empty outline render passes, removes prepared-helper recorded-command copies including a `const std::move` fallback-to-copy case, avoids stable-color outline material binding invalidation, and preserves shared `Unlit.hlsl` backend compatibility. Review hardening also makes incomplete or mismatched GBuffer resources skip both graph and threaded deferred pass construction.

The 2026-05-28 10:15:58 trace still shows selected-object outline work dominating: `Debug GameObject` averages about 111 ms inside `Panel::Draw:Scene View` at about 171 ms. That trace also proves nested `DebugGameObject::*` scopes are suppressed because `Debug GameObject` is already recorded at timeline depth 16, matching the current maximum CPU scope depth. The next effective optimization therefore changes the selected-outline algorithm to a Unity-inspired screen-space path: render selected items into a mask using existing depth, encode selection group/classification channels, and composite the outline with bounded full-screen work. The legacy inflated-shell path remains as a fallback only for explicit compatible cases.

The Phase 12 implementation should be delivered as an algorithmic replacement, not another micro-optimization of `OutlineRenderer`. The primary threaded path should emit selected-mask capture over the selected items plus a composite pass whose shader performs the ID-edge and soft-outline filter directly from the mask using a cached 13-sample neighborhood. The legacy stencil plus inflated-shell path remains available only for explicitly compatible fallback cases. After the 2026-05-28 recapture and the reported selected-object black frame, the follow-up hardening makes legacy-shell metadata propagate scene color/depth explicitly, requires prepared deferred GBuffer depth before screen-space outline capture, declares RHI access against texture-view subresource ranges, and removes linear grouping scans from the selected-outline capture hot path.

## Technical Context

**Language/Version**: C++20  
**Primary Dependencies**: CMake, GoogleTest, ImGui, Nullus Rendering/RHI runtime  
**Storage**: N/A  
**Testing**: `NullusUnitTests` focused GoogleTest filters, optional editor runtime trace validation  
**Target Platform**: Windows editor runtime first; backend evidence limited to the backend used by each validation run  
**Project Type**: Desktop editor and engine runtime  
**Performance Goals**: Reduce stable-size Scene View CPU preparation cost observed in `AView::RendererBeginFrame`; avoid negative-duration trace export events; split the next `Debug GameObject` hotspot into actionable child scopes; remove provable hidden selected-object debug work, empty render-pass setup, duplicate selected-tree mesh resolution, predictable vector reallocations, prepared-helper recorded-command copies, and redundant selected-outline material binding invalidation before later runtime validation; replace per-selected-mesh inflated shell outline output with a bounded mask plus screen-space composite path
**Constraints**: Preserve Editor and Game runtime viability, do not hand-edit generated output, do not claim unvalidated backend/platform behavior  
**Scale/Scope**: Renderer hot-path optimization, trace export validity, selected-object debug scope instrumentation and CPU gating, deferred GBuffer resource safety guards, and one editor-only selection outline algorithm change; no wholesale frame-graph rewrite

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

- **Spec-first major change**: PASS. Work is under `specs/037-scene-view-cpu/` before code changes.
- **Validation matches subsystem**: PASS. Automated renderer/profiler unit tests are required; editor runtime trace is planned where practical. RenderDoc is deferred because the first slice is CPU preparation, not visual GPU correctness.
- **Generated code/backend boundaries**: PASS. No files under `Runtime/*/Gen/` will be edited. RHI behavior remains behind existing renderer/RHI boundaries.
- **Incremental verified delivery**: PASS. Work is split into stable-size GBuffer reuse and trace export validity, each with focused tests.
- **Rendering safety**: PASS. Deferred graph/threaded pass construction must reject incomplete or mismatched GBuffer resources before RHI pass inputs are built.
- **Algorithmic correction**: PASS. The latest trace shows micro-optimizations did not remove the selected-outline bottleneck, so the next phase is scoped to an editor-only Unity-style mask/composite path with a legacy fallback.
- **Product runtime preservation**: PASS. Editor/Game entrypoints remain unchanged; backend claims are limited to validated runs.

## Project Structure

### Documentation (this feature)

```text
specs/037-scene-view-cpu/
├── spec.md
├── plan.md
├── research.md
├── data-model.md
├── quickstart.md
├── checklists/
│   └── requirements.md
└── tasks.md
```

### Source Code (repository root)

```text
Runtime/
├── Engine/Rendering/
│   ├── DeferredSceneRenderer.h
│   └── DeferredSceneRenderer.cpp
├── Rendering/FrameGraph/
│   └── SceneRenderGraphBuilderDeferred.cpp
└── UI/ImGuiExtensions/TimelineProfiler/
    ├── ProfilerTraceCursor.h
    └── ProfilerWindow.cpp

Project/
└── Editor/Rendering/
    ├── DebugSceneRenderer.cpp
    ├── DebugGameObjectSelectionCollector.h
    ├── SelectionOutlineMaskRenderer.cpp
    ├── SelectionOutlineMaskRenderer.h
    ├── OutlineRenderer.cpp
    ├── GridRenderPass.*
    ├── PickingRenderPass.*
    └── OutlineRenderer.h

App/
└── Assets/Editor/Shaders/
    ├── SelectionOutlineMask.hlsl
    ├── SelectionOutlineComposite.hlsl
    └── SelectionOutlineCompositeCore.hlsli

Tests/
└── Unit/
    ├── DeferredSceneRendererMaterialCacheTests.cpp
    ├── EditorRenderPathContractTests.cpp
    ├── FrameGraphSceneTargetsTests.cpp
    ├── RendererFrameObjectBindingTests.cpp
    └── ProfilerDestinationTests.cpp
```

**Structure Decision**: Keep changes in the existing runtime modules and their established unit-test files. Add an editor-only selection outline mask renderer and shader rather than changing generated files, shared `Unlit.hlsl`, or game runtime rendering.

## Phase 12 Design Notes

- **Trace root cause**: The latest trace has 24 main-thread frames on `tid=2` with `Debug GameObject` averaging 111.012 ms. `Picking` averages 0.022 ms, `Grid` 1.296 ms, `Debug Draw` 1.485 ms, and `Debug Lights` 2.361 ms, so the actionable remaining bottleneck is selected-object rendering.
- **Profiler visibility**: `Debug GameObject` sits under 16 containing scopes, and `kTimelineProfilerMaxCpuScopeDepth` is 16. Phase 12 should prefer shallow `SelectionOutlineMask::*` aggregate scopes to avoid global profiler overhead. Raising the global cap is acceptable only with tests or measurements that bound trace size and recording overhead.
- **Unity comparison**: Unity 2018.4 uses `Camera::DoRenderSelected` -> `RenderSelectionOutline` -> `RenderOutline` in `EditorCameraDrawing.cpp`: collect selected parent/child render nodes, filter the current `RenderNodeQueue`, render selected nodes into a temporary mask using the current depth buffer, run ID-edge detection, blur horizontally and vertically, then full-screen composite with `SceneViewSelected.shader`. `RenderSingleNodeSelected` also prefers a material `SceneSelectionPass` and otherwise uses the default selection material with copied alpha-cutout inputs.
- **Nullus adaptation**: Use `DebugGameObjectSelectionCollector` as the source of selected mesh/transform/classification data. Do not introduce a scene-wide render-node queue rewrite in this phase. Add `SelectionOutlineMaskRenderer` as an editor-only helper that owns stable-size mask resources and emits prepared helper pass inputs. Nullus fuses Unity's edge/blur topology into the composite shader to reduce helper pass pressure; this is an approximation, not a one-to-one Unity pass graph.
- **Multi-pass integration**: Screen-space outline preparation returns an ordered collection of helper pass inputs, not a single selection helper pass. DebugSceneRenderer appended-helper assembly, metadata counts, graph pass names, duplicate-name consumption, and prepared-package execution must all handle the mask and composite passes.
- **Resource contract**: Stable reuse is keyed by width, height, the actual mip-adjusted output/depth texture extents, color format, output/depth sample counts, scene depth identity, output color identity, and view descriptors. Same-size frames with new depth/output attachments or stale attachments whose texture extents do not match the current frame are rejected before intermediate allocation. The current mask path is single-sample only; MSAA or output/depth sample-count mismatches produce `UnsupportedSampleCount` before intermediate resources are allocated.
- **Safety contract**: Mask capture reads scene depth read-only, and mask PSOs disable depth and stencil writes. The pass matrix is mask write -> composite mask read/output write. Invalid resources must trigger an observable fallback decision; sample-count incompatibility skips the current outline frame because both the mask path and the legacy recorded shell path are single-sample today.
- **Large-selection bound**: Nullus keeps one selected-coverage mask capture for every valid selection, but gates the extra visible-depth refinement pass to tiny selections so complex selected trees do not double command preparation. This is a deliberate approximation of Unity's visible/occluded refinement, not proof of final FPS improvement until a comparable trace is captured.
- **Stable-selection CPU cache**: Large unchanged selections reuse a prepared capture-group cache before the expensive unordered grouping and full mask signature rebuild. The cache is keyed by selected mesh/material identity, mesh content revisions, transforms, group/classification values, selected camera helper transforms, and camera helper mesh presence/revision; per-frame recorded draw bindings are still rebuilt only on actual mask recapture and are never cached.
- **Fused composite shader bound**: The two-pass adaptation must not turn Unity's edge+blur chain into an unchecked per-pixel sample multiplier. The composite shader loads the 13 unique center/cardinal/diagonal/two-texel mask samples once and reuses them for the center/right/left/down/up soft-outline weights, preserving the current fused semantics while avoiding five nested 5-tap edge-filter calls.
- **Unsupported material semantics**: Unity's `SceneSelectionPass`/alpha-cutout material path is documented as a future compatibility gap. Until Nullus implements source-material selection pass binding, alpha-mask materials produce `UnsupportedMaterialMask` and skip the current outline frame instead of drawing an opaque wrong mask or falling back to the expensive shell path.
- **Threaded publication backpressure**: Large selected-tree creation can queue hundreds of renderer resource tasks while selection-outline helper capture needs prepared object-data buffers. The Editor uses one extra threaded frame slot beyond swapchain frames-in-flight plus a short prepared-publication retirement wait so the main deferred Scene View capture can reuse a freshly retired slot instead of skipping the scene snapshot.
- **Cross-language SSoT**: Mask channel swizzles and constants use one shared source of truth, such as a `.def` table consumed by C++ and HLSL or a generated/checked pair. Comments alone are not sufficient.
- **Validation contract**: Unit/source contracts lock pass construction and fallback behavior; RenderDoc or equivalent RHI event verification checks runtime pass order, attachments, and resource-state/barrier transitions; a comparable post-change trace must preserve selection scenario invariants before claiming the FPS issue is fixed.
- **RHI failure contract**: Any helper command recording failure or required visibility/post-pass transition recording failure must poison the submission frame and advance failed-retirement telemetry instead of validating cached mask reuse. External Scene View output paths are serial while they share the frame resource-state tracker, so serial zero-draw/zero-dispatch paths must be covered explicitly.

## Phase 0: Research

See [research.md](research.md).

## Phase 1: Design

See [data-model.md](data-model.md) and [quickstart.md](quickstart.md). No external contracts are introduced.

## Constitution Check (Post-Design)

- **Spec scope**: PASS. Design remains in one focused spec bundle.
- **Generated-file boundary**: PASS. No generated files are in scope.
- **Backend/platform validation**: PASS. Tests are backend-light where possible; runtime evidence will name the validated backend.
- **Runtime viability**: PASS. The optimization preserves current editor/game entrypoints and existing fallback paths.
- **Evidence path**: PASS. Failing tests before implementation, passing targeted tests after implementation, and optional trace comparison are defined. `Debug GameObject` child-scope attribution, empty-pass avoidance, selected-tree data reuse, prepared-helper recorded-command copy avoidance, stable-color material binding invalidation avoidance, screen-space outline pass construction, and profiler depth-cap mitigation are testable; total hotspot improvement is not claimed until a post-change runtime trace is exported.

## Complexity Tracking

No constitution violations require justification.
