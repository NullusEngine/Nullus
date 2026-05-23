# Quickstart: LightGrid Performance Toggle

## Automated Validation

1. Configure/build Debug shared runtime as usual.
2. Run targeted tests:

```powershell
cmake --build .\Build --config Debug --target NullusUnitTests
.\Build\bin\Debug\NullusUnitTests.exe --gtest_filter=*LightGrid*:EditorSettings*
```

## Manual Editor Validation

1. Start `Editor.exe` with a scene that contains lights.
2. Open Project Settings.
3. Search for `LightGrid`.
4. Confirm the setting is enabled by default.
5. Disable LightGrid and close/reopen Project Settings.
6. Confirm the setting persists.
7. Render Scene View and capture a short Tracy profile.
8. Confirm `LightGridPrepass::Prepare` disappears when disabled.
9. Re-enable LightGrid and confirm the next frame restores LightGrid preparation without crashing.

## Performance Check

With LightGrid enabled and threaded rendering active, capture a frame in Tracy and verify the same scene frame does not show duplicate `LightGridPrepass::Prepare` work from both BeginFrame and EndFrame/package publication.
