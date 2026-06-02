# Baseline Diagnostics: Optimize Draw-Call Scalability

## Deterministic Stress Baselines

- Compatible opaque objects: before dynamic grouping, the `RenderSceneCacheTests.DynamicInstancingReducesOneThousandCompatibleOpaqueObjectsToOneSubmittedDraw` scenario represents 1,000 visible mesh renderers sharing one mesh/material/render state submitting 1,000 scene draws.
- Stable second frame: before this work, cached command rebuild telemetry did not expose the stable-frame draw-call reduction proof needed to distinguish cache rebuild cost from draw submission cost.
- Large non-groupable recorded pass: before ordered slicing, 2,000 recorded draw commands stay in one pass work unit. The MVP now distinguishes attachment-free passes, which may be sliced into ordered serial work units, from attachment-backed scene passes, which remain unsliced until renderpass-internal recording exists.
- Overflow behavior: before the overflow fix, exhausting object-data slots could leave invalid-index draw commands for the binding provider to reject downstream instead of reporting dropped objects at grouping time.

## Expected Improvement Targets

- Reduce 1,000 compatible opaque objects to one submitted scene draw on the stable path.
- Preserve existing material GPU instance behavior and transparent ordering.
- Split large attachment-free recorded draw passes into multiple ordered work units only on safe paths.
- Keep attachment-backed scene passes unsliced until backend renderpass-internal recording and RenderDoc validation exist.
- Keep unsliced `passCommandInputs` as the authoritative serial fallback when ordered sliced submission is unsafe.
