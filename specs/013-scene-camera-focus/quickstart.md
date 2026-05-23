# Quickstart: Scene Camera Focus Validation

## Automated Validation

Run focused unit tests:

```powershell
cmake --build Build --config Debug --target NullusUnitTests -- /m:1
Build\bin\Debug\NullusUnitTests.exe --gtest_filter=SceneCameraFocusTests.*:ImGuizmoTransformAdapterTests.*ViewGizmo*
```

Run the full unit test target:

```powershell
ctest --test-dir Build -C Debug --output-on-failure -R "NullusUnitTests"
```

Build the Editor:

```powershell
cmake --build Build --config Debug --target Editor -- /m:1
```

## Manual Scene View Checks

1. Select or frame an object, then click ViewGizmo side faces. The object or scene area under focus should stay centered while the camera rotates around it.
2. Click an opposite ViewGizmo face from a high or low view. The camera should route around world Y, not flip over the top or bottom.
3. Start a ViewGizmo click rotation, then press right mouse and rotate the camera. The right-mouse result should remain; the click rotation must not resume after releasing right mouse.
4. Middle-mouse pan near and far from the focus. The focused point should visually track the mouse consistently, with farther focus distances moving a larger world distance for the same screen drag.
5. Scroll zoom toward the focus, then ViewGizmo rotate. Rotation should continue around the same focus point at the updated distance.
