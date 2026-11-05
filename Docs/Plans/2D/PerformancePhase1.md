# Phase 1 Sprite performance evidence — 2026-10-30

The existing 10,000-Sprite scene now has measured CPU/GPU distributions. Two changes reduce component lifecycle snapshot allocations and resolve a shared Sprite once per scene extraction. A matching 30-second capture reduced Editor frame P95 from 247.136 ms to 171.846 ms (30.5%). The workload remains limited by CPU work; this does not establish a 60 FPS performance pass.

## Reproduction

- Project: `tmp/2d-smoke-3a34a421/SpriteValidation.nullus`; no project was created.
- Scene: `Assets/Scenes/SpriteBenchmark.scene`, fixed seed 12345, 10,000 SpriteRenderer components, SHA256 `A6B6129A26270B083F8A39E648B6B343DDE4AAD998A546C5E4FE3F5B9EFCCA78`.
- Frozen validation source: `tmp/2d-src`; `snapshot.json` records source provenance and scoped differences from the concurrently edited checkout.
- Native configuration: RelWithDebInfo, profiling/Tracy enabled, TimelineProfiler disabled, Tracy 0.13.3.
- Backend: DX12, Tasks RHI mode, NVIDIA GeForce RTX 5070 Laptop GPU.
- Editor Play, exclusive Game View, fixed 1280×720; VSync and Scene View Static Cache disabled.
- Warmup: 45 seconds after startup and 20 seconds after entering Play; capture: 30 seconds.
- Repository Tracy workflow: `.agents/skills/nullus-tracy-performance/SKILL.md`. Owned script copies under the snapshot's `.validation` directory select this source tree and close only their own Editor normally. `tmp/2d-run-tracy-benchmark.ps1` applies/restores temporary workload settings.

Main.scene retained SHA256 `300B81B48EF0825C8FDC502A0F9C8C68C214E06F689AD633453930E50C533124` after each capture. Project and user settings were restored by value. All runs reached Play and their owned Editor closed normally. No mouse automation was used for these captures.

## Recorded runs

| Measurement | Initial capture | Instrumented baseline | Optimized |
|---|---:|---:|---:|
| Trace directory under tmp/2d-tracy-current | 20261030-092235 | 20261030-093900 | 20261030-110612 |
| Editor frame mean (ms) | 219.400 | 216.381 | 155.159 |
| Editor frame P50 (ms) | 215.329 | 215.069 | 153.672 |
| Editor frame P95 (ms) | 257.154 | 247.136 | 171.846 |
| Editor frame P99 (ms) | 295.846 | 257.939 | 183.488 |
| Mean FPS | 4.56 | 4.62 | 6.44 |
| GPU transparent pass mean (ms) | 0.020 | 0.020 | 0.019 |
| GPU transparent pass P95 (ms) | 0.025 | 0.026 | 0.025 |
| GPU UI overlay P95 (ms) | 0.274 | 0.273 | 0.271 |

Frame cadence is Application::TickFrame start-to-start time, including Editor work. GPU rows are pass timestamps, not total GPU frame time. Render-thread wait zones describe idle waits and are not evidence of expensive active rendering.

Each dated directory contains `capture.tracy`, `capture-result.json`, `workload.json`, Editor/capture logs and `capture-analysis` exports. CSV exports include CPU total/self distributions, GPU pass distributions and frame cadence. The first run established the bottleneck; the latter two retain the same phase/Sprite attribution markers for comparison.

Working set at capture end was 2,734,788,608 bytes initially and 2,747,244,544 bytes after optimization; optimized private bytes were 3,365,183,488. These are measurements of a Tracy-enabled Editor with scripting hosts and profiling buffers, not standalone Player memory requirements. Peak working set includes startup.

## Changes and regression evidence

`GameObject.cpp` keeps component ID snapshots inline for up to eight components and uses a heap fallback for larger lists. It still resolves IDs at each callback boundary, preserves ordering, and allows callbacks to remove components or add components for a later visit. `GameObjectLifecycleSnapshotTests.cpp` covers self-removal, removal of a later component and deferred additions on both paths.

`SpriteAssetCaptureContext` lives for one scene extraction. Components sharing a persistent Sprite reference reuse its resolution while retaining their own revision/error state. New contexts observe a new import; previous immutable descriptors remain valid. The import regression covers reimport, both old/new descriptors, failed resource resolution, retained usable content and clearing one component's reference.

- Play/lifecycle: 43/43, `tmp/2d-performance-play-tests.log`.
- Sprite import/Inspector/SDK/Prefab: 117/117, `tmp/2d-performance-import-tests.log`.
- Rendering, Sprite and mixed submission GPU: 340/340, `tmp/2d-performance-rendering-complete.log` and XML; normal exit 0 in `tmp/2d-performance-rendering-exit.json`.
- Optimized independent Prefab Player: `tmp/2d-optimized-player-complete-evidence/evidence.json`; actual DX12 readback, 2 submitted/1 visible, one batch/draw and two resource leases. Both scripting hosts and product/Windows module isolation passed. This is a separate functional package check, not a standalone Game.exe Tracy measurement.

An earlier rendering run ended without a completion footer during a tool-session transition; its log is retained. It is not counted as passing. The separate complete run above supplies the result.

## Remaining limits

Lifecycle traversal, Sprite proxy construction and Hierarchy drawing remain material CPU costs. The capture covers one fixed Sprite/texture workload on one Windows GPU. Material/Atlas permutations, other RHI/thread configurations, map/physics/light/animation/deformation stress, GPU memory accounting and multi-platform results remain open in the feature matrix. A single matched pair is evidence for this workload, not a cross-device guarantee.
