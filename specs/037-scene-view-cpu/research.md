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
