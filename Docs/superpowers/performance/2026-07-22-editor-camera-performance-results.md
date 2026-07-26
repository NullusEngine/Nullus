# Editor Scene View Camera Performance Results

| Configuration | Metric | Before | After | Change |
| --- | --- | ---: | ---: | ---: |
| Debug | Mean frame time | 38.378 ms | 34.613 ms | 9.81% |
| Debug | Mean FPS | 26.06 | 28.89 | 10.88% |
| Debug | P95 frame time | 68.881 ms | 64.920 ms | 5.75% |
| Debug | P99 frame time | 85.158 ms | 84.107 ms | 1.23% |
| Debug | Max frame time | 119.847 ms | 128.421 ms | -7.15% |
| Debug | Publication ratio | 76.33% | 59.67% | -16.67 pp |
| Release | Mean frame time | 11.087 ms | 11.067 ms | 0.18% |
| Release | Mean FPS | 90.19 | 90.36 | 0.18% |
| Release | P95 frame time | 24.790 ms | 23.296 ms | 6.03% |
| Release | P99 frame time | 30.651 ms | 26.856 ms | 12.38% |
| Release | Max frame time | 38.870 ms | 27.653 ms | 28.86% |
| Release | Publication ratio | 67.67% | 65.67% | -2 pp |

## Measurement Conditions

- Windows DX12, NVIDIA GeForce RTX 5070 Laptop GPU, driver `32.0.15.9282`.
- Viewport `1276x815`, VSync off, Scene View exclusive, fixed camera `-10,3,10;0,135,0`.
- Each configuration used three sequential trials, with 30 warm-up frames and 300 measured frames. The median trial by mean frame time is reported.
- Baseline artifacts: `build-editor-camera-perf/perf/editor-camera/before/` (captured at commit `8079c613`).
- Optimized artifacts: `build-editor-camera-perf-final-20260725e/perf/editor-camera/after/`.

## Interpretation

- Debug mean frame time improves by 9.81%, but maximum frame time increases from 119.847 ms to 128.421 ms and publication ratio decreases by 16.67 percentage points.
- Release mean frame time is effectively unchanged; P95, P99, and maximum frame time improve by 6.03%, 12.38%, and 28.86% respectively.
- The benchmark does not meet the original stretch goals of 15% Debug mean improvement or 30% Release P99 improvement. The measured result should therefore be treated as a conservative improvement, not a guaranteed frame-rate floor.
- Device loss, unsafe GPU quarantine, descriptor allocation failure, and object-data overflow remained zero in all reported trials.

## Verification

- Debug and Release `Editor` and `NullusUnitTests` targets rebuilt successfully after the final source state.
- Debug focused rendering regression: `286/286` passed.
- Release focused rendering regression: `286/286` passed.
- The attempted per-frame assignment-planning optimization was benchmarked separately and reverted after regressing Debug and Release mean frame time; its temporary artifacts are not used in this table.
