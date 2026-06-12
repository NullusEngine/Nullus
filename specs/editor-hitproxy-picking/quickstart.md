# Quickstart: Editor HitProxy Picking

## Automated Validation

Configure if needed:

```powershell
cmake -S . -B build/editor-hitproxy-picking-verify -G "Visual Studio 17 2022" -A x64 -DNLS_BUILD_TESTS=ON
```

Build focused unit tests:

```powershell
cmake --build build/editor-hitproxy-picking-verify --config Debug --target NullusUnitTests -- /m:1 /nr:false /p:CL_MPCount=1 /p:UseMultiToolTask=false /v:minimal
```

Run focused tests:

```powershell
build\editor-hitproxy-picking-verify\bin\Debug\NullusUnitTests.exe --gtest_filter=SceneViewPickingPolicyTests.*:EditorRenderPathContractTests.*Picking*:RendererStatsTests.*Picking*
```

## Runtime Smoke

1. Launch the Editor with DX12 and a large prefab scene.
2. Open Scene View and FrameInfo.
3. Keep camera and mouse stationary over the view.
4. Confirm picking reuse increases while heavy picking rebuild scopes do not appear every frame.
5. Move the camera continuously.
6. Confirm hover picking may skip by budget, but the editor remains responsive.
7. Click a visible object after moving.
8. Confirm selection resolves after a fresh readable frame and outline remains stable.
9. Select an object from Hierarchy without moving the camera.
10. Confirm selection outline updates without forcing a picking rebuild solely from selection change.

## Trace Evidence

Capture a moving-camera trace after implementation and inspect:

- `EditorPicking::Rebuild`
- `EditorPicking::Reuse`
- `EditorPicking::SkipHoverBudget`
- `EditorPicking::WaitReadback`
- `EditorPicking::ResolveClick`

Expected result: moving camera no longer spends repeated large blocks in `PickingRenderPass::CapturePickableModelSources` for hover-only frames over budget.
