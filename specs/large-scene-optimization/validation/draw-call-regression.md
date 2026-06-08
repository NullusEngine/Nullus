# Draw-Call Regression Tests

Report: Phase 9 draw-call and threaded lifecycle regression validation
Date: 2026-06-05
Commit: worktree local changes, not committed
Branch: large-scene-optimization
Machine: local Windows development workstation
Build configuration: Debug
Raw output log: `draw_call_regression_large_scene.local.log`

## Command

```powershell
build\windows\bin\Debug\NullusUnitTests.exe --gtest_filter=*RenderSceneCache*:*RendererFrameObjectBinding*:*ThreadedRenderingLifecycle*:*RendererStats*
```

## Result

Exit code: 0

```text
Note: Google Test filter = *RenderSceneCache*:*RendererFrameObjectBinding*:*ThreadedRenderingLifecycle*:*RendererStats*
[==========] Running 351 tests from 5 test suites.
[==========] 351 tests from 5 test suites ran. (143276 ms total)
[  PASSED  ] 351 tests.
```

## Coverage Notes

- Covers retained draw-command cache behavior, dynamic instancing, object binding allocation, threaded frame/package lifecycle, renderer stats, and regression coverage for the 1,000-compatible-opaque-object reduction path.
- The full exact stdout/stderr stream is preserved in `draw_call_regression_large_scene.local.log`.
- Some `ThreadedRenderingLifecycleTests` intentionally emit simulated RHI failure/quarantine logs; those messages are expected for those failure-path contracts and did not fail the run.
