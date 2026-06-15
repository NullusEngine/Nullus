
## Final Diagnostics

**Feature**: Remove DX12 Legacy UI Bridge  
**Branch**: `050-remove-dx12-ui-bridge`

### Build

- `cmake --build Build --target NullusUnitTests --config Debug -- /m:1 /p:CL_MPCount=1 /v:m`
- Result: PASS

### Focused Tests

- `NullusUnitTests --gtest_filter=ProfilerDestinationTest.TimelineTraceExporterCanFilterEditorUiNoise:ProfilerDestinationTest.DX12UiBridgeDirectSubmitSourceIsRemoved:RHIUiOverlaySourceGuardTests.*:RHIUiOverlayPassTests.*:UIAndToolingBackendAwarenessTests.*`
- Result: PASS

### Source Grep

- Legacy DX12 bridge selection and direct-submit references were removed from runtime code paths.
- `Runtime/Rendering/RHI/Backends/DX12/DX12UIBridge.cpp` is deleted.
- `CreateDX12RHIUIBridge` no longer appears in runtime selection code.

### Notes

- Platform backend setup remains intact through `ImGui_ImplWin32` and `ImGui_ImplGlfw`.
- A full DX12 interactive smoke run was not captured in this turn. The targeted unit and source-guard evidence validates source-selection/static removal of the legacy bridge only; Editor/Launcher DX12 interactive render-and-present smoke remains pending in T020/T021.
- Unsupported backend behavior is intentionally fail-closed for the current phase-1 product runtime, which only permits DX12. No non-DX12 renderer bridge is restored by this removal task.
