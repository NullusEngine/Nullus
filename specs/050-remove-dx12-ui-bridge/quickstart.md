# Quickstart: Remove DX12 Legacy UI Bridge

1. Build the unit-test target:
   `cmake --build Build --target NullusUnitTests --config Debug -- /m:1 /p:CL_MPCount=1 /v:m`
2. Run the focused UI bridge tests:
   `NullusUnitTests --gtest_filter=RHIUiOverlaySourceGuardTests.*:RHIUiOverlayPassTests.*:UIAndToolingBackendAwarenessTests.*`
3. Verify source guards no longer find legacy DX12 direct-submit selection in `Runtime/Rendering/RHI/Backends/DX12/DX12UIBridge.cpp` and `Runtime/Rendering/RHI/Utils/RHIUIBridge.cpp`.
4. Launch Editor on DX12 and confirm UI renders through the frame-graph path with no legacy bridge submit path in the trace output.
5. Launch Launcher on DX12 and confirm the UI still initializes and responds to input after the bridge removal.
