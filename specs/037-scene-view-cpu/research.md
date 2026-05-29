# Research: Editor Scene View CPU Frame-Time Optimization

## Decision: Optimize stable-size GBuffer preparation first

**Rationale**: The baseline trace attributes the dominant main-thread cost to `Panel::Draw:Scene View`, especially `AView::RendererBeginFrame`. Inside that window, `DeferredSceneRenderer::EnsureGBufferTargets` runs every frame and wraps GBuffer textures every time, even when dimensions are unchanged. This is a narrow optimization with a clear regression test.

**Alternatives considered**:

- Optimize draw capture first: larger potential gain, but it touches material binding and object-data preparation and carries more regression risk.
- Investigate GPU synchronization first: useful for the 23 ms spike in one slow frame, but not the steady 34 ms Scene View cost.

## Decision: Keep resize behavior explicit

**Rationale**: Stable-size frames should reuse resources, but size changes, zero-size transitions, and failed allocation retries must still refresh resources. Existing framebuffer tests already cover retry and zero-size behavior, so the renderer-level optimization should preserve those semantics.

**Alternatives considered**:

- Cache only wrapped textures and still call resize: less risk but retains much of the observed stable-frame cost.
- Replace `MultiFramebuffer` resize behavior globally: broader impact than needed for this slice.

## Decision: Fix trace export validity as a separate slice

**Rationale**: The supplied trace has negative durations and incomplete non-main-thread events. Exporting only completed events keeps future evidence usable without changing profiler recording behavior.

**Alternatives considered**:

- Ignore trace export problems for now: leaves validation noisy and makes future comparisons weaker.
- Rewrite timeline storage: unnecessary for the immediate issue and too broad for this optimization.

## Decision: Use focused unit tests before implementation

**Rationale**: The codebase already has renderer and profiler unit-test entrypoints. Tests can verify stable-size no-op behavior and trace export filtering without requiring a live editor run.

**Alternatives considered**:

- Manual trace-only validation: useful but insufficient for a repeatable regression gate.
- RenderDoc capture: better for GPU visual/pass correctness, but this first slice is CPU preparation and trace export validity.

## Decision: Replace selected-object shell outline with a Unity-style mask/composite path

**Rationale**: The latest selected-object trace still attributes about 111 ms per frame to `Debug GameObject`, while prior work has already removed the obvious empty-pass, duplicate traversal, copy, and stable-color invalidation costs. Unity 2018.4 Scene View selection outline, in `Editor/Src/Utility/EditorCameraDrawing.cpp`, first skips empty selection in `Camera::DoRenderSelected`, collects selected parent/child renderable component IDs in `RenderSelectionOutline`, filters them against the existing `RenderNodeQueue`, then calls `RenderOutline`. `RenderOutline` renders selected render nodes into a temporary mask using the current scene depth, stores visible/occluded/object-ID channels, runs an ID-edge prepass, performs separable blur, and composites the final outline with a full-screen pass using `SceneView/SceneViewSelected.shader`. `RenderSingleNodeSelected` also checks for a material `SceneSelectionPass` before falling back to the default selection material. That design bounds final outline output by a few full-screen passes instead of drawing an inflated shell for every selected mesh.

**Alternatives considered**:

- Continue optimizing the current stencil plus inflated shell path: low risk, but the trace shows the remaining cost is too large for incremental command-copy and material-binding improvements to solve.
- Use CPU-generated silhouette edges: can reduce pixel work, but requires topology processing, skinned/animated mesh handling, and more CPU work in the path that is already too slow.
- Disable outlines for large selections: fast but breaks expected editor selection feedback and is worse than Unity's established approach.

## Decision: Raise or bypass the profiler depth cap for the outline validation path

**Rationale**: The 2026-05-28 10:15:58 trace records `Debug GameObject` at depth 16, and `Runtime/UI/Profiling/TimelineProfilerLimits.h` currently sets `kTimelineProfilerMaxCpuScopeDepth` to 16. `TimelineProfilerSink::BeginScope` suppresses scopes at or beyond that depth, so the existing `DebugGameObject::*` scopes cannot appear in the exported trace. The implementation needs an explicit validation path: either raise the bounded TimelineProfiler CPU depth enough for editor render child scopes, or emit dedicated aggregate `SelectionOutlineMask::*` scopes from a shallower point.

**Alternatives considered**:

- Rely on the `Debug GameObject` parent scope only: rejected because it cannot prove whether mask capture, edge/blur/composite, or fallback shell work remains expensive.
- Remove higher-level editor UI scopes: rejected because it would damage the profiler's general editor trace structure.

## Decision: Fix outline trace visibility at the profiler depth boundary

**Rationale**: In the 2026-05-28 10:15:58 trace, `Debug GameObject` is recorded at depth 16, which is the current `kTimelineProfilerMaxCpuScopeDepth`. Nested `DebugGameObject::*` scopes are therefore suppressed even though the source markers exist. The next implementation must either raise the exportable depth safely or emit shallower aggregate scopes around the expensive selected-outline phases.

**Alternatives considered**:

- Ignore nested scopes and rely only on the parent scope: insufficient for validating the algorithmic change or catching regressions.
- Remove higher-level UI scopes: would damage other profiling workflows and make traces harder to interpret.
