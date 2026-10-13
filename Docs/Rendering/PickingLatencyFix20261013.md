# Picking latency repair, 2026-10-13

## Problem and implementation

Picking captured legacy object constants from a reusable frame-slot ring. Those
bindings correctly report `objectBindingImmutable=false`, so
`CanCacheEditorDrawCommands` rejected the entire picking draw-command cache.
Every click then captured all visible models again on the Game Thread.

Picking now explicitly requests immutable object bindings for models and camera
and light helpers. The provider refuses a mutable allocation fallback when an
immutable binding was requested. Normal scene draws continue using frame-slot
rings. Cache hits refresh the current frame binding; a failed frame-binding
capture causes a cache miss instead of retaining stale camera constants.

The first immutable capture was still expensive. Phase instrumentation measured
295 model draws taking 405.56 ms: material binding creation accounted for
194.87 ms, object binding capture for 181.72 ms, and object preparation for
0.76 ms. Hundreds of individual small upload-buffer allocations dominated.

`ScopedUniformBufferSnapshotBatch`, enabled around a picking pass build, packs
snapshots into 64 KiB pages with 256-byte aligned offsets. Pages are append-only,
are never reset or recycled by a frame slot, and are held by the captured
binding sets. A later batch allocates new pages. Unsupported page updates or a
page allocation failure fall back to individual immutable snapshots. Both the
material and object binding paths preserve the snapshot offset and range.

Changing a material value now invalidates resource bindings while retaining
shader/device/generation-keyed pipeline layouts and their validation diagnostics.
Shader changes and explicit layout rebuilds still invalidate layouts. This
avoids creating the same root signature for every picking color.

## Matched performance evidence

- Git HEAD: `73a584ad5f45a6a355ca5dece7b1799d42155533`, with existing migration and
  editor changes preserved; no commit or staging was performed.
- Configuration: Release, Tracy 0.13.3, DX12 Tasks, NVIDIA RTX 5070 Laptop GPU.
- Project: `D:/Code/Nullus/TestProject/TestProject.nullus`, Sponza scene, edit mode.
- Camera: `-10,3,10;0,135,0`; outer window 1440x900, client 1425x862.
- VSync and Scene View Static Cache disabled. Same warm shader cache, 15 seconds
  after `EditorWindowShown`, 22-second capture, 16 directed mouse clicks.
- Directed events target only the trial's Editor HWND; settings and cursor are
  restored and only trial-owned processes are closed. This is automated input
  validation, not an assessment of a human user's subjective experience.
- Before/after start screenshots were inspected and show the same view. End
  screenshots confirm selection outlines and the transform gizmo.

| Capture | Pass builds | Model recaptures | Build median, ms | Build P95, ms | Picking GPU mean / P95, ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| before-directed-1 | 17 | 17 | 281.30 | 338.65 | 0.55 / 0.57 |
| before-directed-2 | 17 | 17 | 302.17 | 352.66 | 0.58 / 0.81 |
| after-final-1 | 17 | 1 | 0.09 | 29.41 | 0.58 / 0.78 |
| after-final-2 | 17 | 1 | 0.10 | 35.48 | 0.56 / 0.71 |

The final runs each contain one first build (29.41 / 35.48 ms) and 16 cache
hits. With only 17 samples, the P95 convention used here includes the first
build. Cache-hit zone medians were 0.08 / 0.09 ms. These are CPU command-build
times, not total mouse-to-visible-selection latency or overall frame-time gains.

A separate final run with 16 distinct coordinate messages recorded 13 builds,
one model recapture and 12 click resolutions. Some input messages did not yield
a click-resolution event, so this run is not part of the matched 16-click A/B.
It confirms the command cache remains effective across coordinates. Its
`ReadPixelsChecked` median was 17.16 ms, P95 29.74 ms (14 calls): uncached pixel
readback still waits for GPU completion and is not eliminated by this repair.

Intermediate captures (`after-directed-*`, `cold-profile-1`, `after-batch-1`)
diagnose the implementation stages and are not the final performance result.
Earlier `before-clean-*` captures with mismatched camera/input are excluded.

## Verification

- Release Editor and RelWithDebInfo Editor/NullusUnitTests built successfully.
- Final regression: 335 tests, zero failures, errors, disabled or skipped cases:
  RendererFrameObjectBindingTests (168), RenderSceneCacheTests (117),
  DeferredSceneRendererMaterialCacheTests (25), SceneViewPickingPolicyTests (13),
  PickingReadbackLifecycleTests (7), CompositeRendererExplicitDrawOrderTests (5).
- New regression coverage checks page boundaries, immutable matrix preservation
  after frame-slot reuse and later batches, nested batches and page release,
  allocation/update failure fallbacks, independent material-color offsets,
  layout retention, device separation and incompatible-shader diagnostics.
- DX12 Debug Layer was confirmed enabled via ID3D12Debug5. The final distinct
  coordinate smoke run under cdb exited normally; its native log contains no
  D3D12 ERROR/CORRUPTION, assertion or configured native-exception marker.
- That smoke run still logged 1,059 clear-value mismatch warnings (#820).
  GPU-based validation was disabled. This is a smoke check, not exhaustive
  GPU validation or a long-duration stress acceptance run.
- Vulkan was not exercised for this repair. Metal verification remains skipped
  at the user's request. The shared CPU paths have focused tests; there is no
  claim of new interactive validation for either backend.
- The broader DebugDrawPass suite is outside the 335-test passing set. A final
  isolated rerun of `CompositeRendererExecutePassDrawsDebugLinesDuringActiveFrame`
  still fails: it pauses the render workers, then expects the removed inline
  standalone draw path to execute. See `debug-draw-existing.log` and XML. This
  repair does not claim a passing result for it or the entire test suite.
- Changed-file `git diff --check` passed. An unrelated existing DX12Resource.cpp
  trailing-whitespace issue was left untouched.

## Artifacts

Artifacts are in `Build/validation/picking-cache-20261013/`, including traces,
CSV/JSON exports, screenshots, test XML/logs and the reusable trial script.
Build logs are `Build/picking-layout-final-build-release-20261013.log` and
`Build/picking-layout-final-build-tests-20261013.log`.

Final deployed Release hashes (matched to the corresponding Build outputs):

```text
App/Win64_Release_Runtime_Shared/Editor.exe
01BA410C0C09468FDB45107A979E011B5BC2A27C700778197640E5682E999011
App/Win64_Release_Runtime_Shared/NLS_Render.dll
DB07413C9A005924735231AA307EAEAE7B909D44A7268EF344572E97442E5749
App/Win64_Release_Runtime_Shared/NLS_Engine.dll
170F2AEC522E0571CA1B0BE9779FFD41A2E89A233FAA53E10389929C736A8654
```
