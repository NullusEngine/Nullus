# Review: Large Scene Optimization Design

**Scope**: `specs/large-scene-optimization/` design package only.
**Branch**: `large-scene-optimization`
**Worktree**: `D:/VSProject/Nullus/.worktrees/large-scene-optimization`
**Date**: 2026-06-03

## Review Process

This design required review because it affects rendering architecture, large-scene visibility, RHI/GPU synchronization, streaming budgets, and editor observability.

The package was reviewed with the repository `/plan-review` quality gate and independent multi-agent checks:

- Architecture and performance review.
- GPU/RHI correctness review.
- Code quality, SSoT, and testing review.
- UE 4.27 / Unity 2018.4 industry-reference verification.

No runtime source files were changed during this design pass. This note describes only the original design-review pass; final implementation evidence and code-change validation live under `specs/large-scene-optimization/validation/` and the final review gates.

## R1 Resolutions

- **Architecture/performance P1 fixed in design**: The design now requires maintained spatial/layer/distance/active candidate metadata, slot-map/free-list/tombstone/generation handles, `RepresentationResidencySnapshot`, CPU/IO/GPU/memory streaming budgets, and clear `RenderScene` ownership of queue finalization.
- **GPU/RHI P1 fixed in design**: The design now requires `RHIDevice::GetCapabilities()` / `RHIDeviceFeature` gates, texture resource access plus visibility transitions, narrow HZB subresource hazards, unsupported-capability fallback reasons, and ordinary-frame no-sync-readback regression coverage. The current implementation declares mip0-only HZB hazards until a real mip-chain shader writes additional mips.
- **SSoT/testing P1 fixed in design**: The tasks now require `LargeSceneSettings` as an SSoT surface, draw-call regression gates, object-index regression, candidate/touched-count assertions, and snapshot-based FrameInfo/editor consumption.
- **Industry-reference P1 fixed in design**: The research no longer claims UE/Unity universally prove a spatial-first candidate stage, and Unity scriptable culling is corrected to `Runtime/Graphics/ScriptableRenderLoop/ScriptableCulling.cpp`.

## R2 Deeper Audit Resolutions

- **GPU/RHI P1 fixed in design**: Occlusion history compatibility now includes view/projection/jitter/depth/resource/backend fields plus primitive generation, bounds generation, representation id, and depth-write eligibility. HZB depth is constrained to eligible opaque depth-writing geometry. Prepared compute must carry resource accesses, transitions, exported transitions, dependency edges, and subresource ranges. Ordinary frames must not wait on `ReadPixelsChecked`, `BeginReadPixels`, fences, or blocking readback maps.
- **Architecture/performance P1 fixed in design**: The design now covers O(changed) `RenderScene::Synchronize`, immutable `ScenePrimitiveSnapshot`, sparse visible primitive/command handles, cached command-offset tables, bounded queue finalization, last-good spatial index plus dirty overlay/rebuild budget, dynamic-heavy query bounds, additive-scene handle scoping, and validation tables for sync/visibility/finalization/streaming touched counts.
- **Streaming P1 fixed in design**: The data model and tasks now include `StreamingResourceDependency` and `ResidencyTicket` for dependency closure, request deduplication, coalescing, priority aging, cancellation, pin counts, and dependency-aware eviction.
- **HLOD/rendering P2 fixed in design**: HLOD clusters now include compatibility flags so transparent/order-dependent children are not suppressed unless explicitly safe.
- **Industry benchmark P1 fixed in design**: UE/Unity streaming references are limited to separation of visibility/residency or asset preparation; Nullus budgets are stated as local requirements with direct validation. A benchmark draft was added at `specs/large-scene-optimization/benchmarks/rendering-large-scene-benchmark.md`, and T104 requires promoting it into the `/plan-review` benchmark registry before implementation sign-off.
- **SSoT/testing P2 fixed in design**: Quickstart Phase 5 now names both `AssetImportPipelineTests.cpp` and `AssetImporterFacadeTests.cpp`, with their respective coverage responsibilities.

## Remaining P2 / Follow-Up Gates

- `.specify/` and `.agents/` are not tracked in this worktree, so the bundle was authored manually in repository spec-kit format. Before implementation sign-off, run the official scripts again from a branch/worktree where those directories are available.
- The large-scene benchmark draft is intentionally local to this design package until implementation evidence exists. T104 makes registry promotion a required implementation-phase gate.
- Runtime evidence, unit tests, and RenderDoc captures are required by the implementation tasks before any phase can claim behavioral completion.

## Final Assessment

### Auto-Fail Check

- **Correctness**: Pass. The design covers async lifetime, stale handles, explicitly reported source-sync fallback cost, dirty/sparse downstream maintenance, bounded finalization, conservative occlusion, editor view-local HLOD, streaming dependency closure, resource pinning, and cross-backend evidence boundaries.
- **Robustness**: Pass. Unsupported features degrade to the existing retained render-scene path, invalid occlusion history is visible, missing residency falls back without synchronous loading, JobSystem failure falls back to serial visibility, and spatial rebuild fallback is reported.
- **Performance**: Pass. The plan now protects the full large-scene frame path: sync, candidate query, dynamic query, visibility, queue finalization, streaming commit, and debug overlays all have touched-count or timing gates.
- **Maintainability**: Pass. `LargeSceneSettings`, primitive handles, snapshots, visibility results, HZB capabilities, streaming dependencies/tickets, and validation schema are named SSoT surfaces with matching tasks.
- **Industry Best Practice**: Pass with implementation follow-up. UE 4.27 and Unity 2018.4 source paths are concrete, claims are scoped to what those references prove, and the benchmark draft must be promoted to the registry before implementation sign-off.

### Score

- **Correctness**: 9/10
- **Robustness**: 9/10
- **Performance**: 9/10
- **Maintainability**: 9/10
- **Industry Best Practice**: 8/10

**Total**: 44/50
**Rating**: Excellent design package, ready for phased implementation planning and code work.
**Highest remaining consequence level**: P2.

The design package has no known unresolved P0/P1 findings after the R2 fixes. Implementation must still run `/plan-review`, the required multi-agent review gate for GPU sync/RHI work, targeted tests, and RenderDoc/RHI evidence before any code phase can be called complete.
