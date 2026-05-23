# Quickstart: UE4.27 Render Architecture Alignment

## Read-Only References

- `F:\Epic Games\UE_4.27\Engine\Source\Runtime\RHI\Public\RHICommandList.h`
- `F:\Epic Games\UE_4.27\Engine\Source\Runtime\RenderCore\Public\RenderGraph.h`
- `F:\Epic Games\UE_4.27\Engine\Source\Runtime\RenderCore\Private\RenderGraphBuilder.cpp`
- `F:\Epic Games\UE_4.27\Engine\Source\Runtime\RenderCore\Public\ShaderParameterStruct.h`
- `F:\Epic Games\UE_4.27\Engine\Source\Runtime\Renderer\Private\MeshDrawCommands.h`

## Focused Validation

Run targeted unit tests after each slice:

```powershell
.\Build\Tests\Unit\Debug\NullusUnitTests.exe --gtest_filter=RenderFrameworkContractTests.*UE427*:FrameGraphSceneTargetsTests.*UE427*:ShaderBindingLayoutUtilsTests.*UE427*:ThreadedRenderingLifecycleTests.*UE427*
```

If the binary layout differs, locate the test binary first:

```powershell
Get-ChildItem -Recurse -Filter NullusUnitTests.exe .\Build
```

Run broader renderer contract tests before wrapping the feature:

```powershell
.\Build\Tests\Unit\Debug\NullusUnitTests.exe --gtest_filter=RenderFrameworkContractTests.*:FrameGraphSceneTargetsTests.*:ShaderBindingLayoutUtilsTests.*:ThreadedRenderingLifecycleTests.*
```

Build Editor after each completed implementation phase:

```powershell
cmake --build .\Build --config Debug --target Editor
```

## Architecture Tracking Notes

- US1 RHI command-list alignment is tracked through command-list lifecycle and child submission metadata.
- US2 RDG alignment is tracked through pass/resource declaration contracts, side-effect retention, and undeclared access diagnostics.
- US3 shader parameter alignment is tracked through `BuildShaderParameterGroupContracts()` and `ValidateShaderParameterGroupResources()`.
- US3 keeps `RHIBindingLayoutEntry` compatible by mapping every parameter group back to the existing descriptor set, register space, binding, type, count, and stage-mask fields instead of adding duplicate RHI descriptor metadata.
- US4 parallel draw command alignment remains pending.

## Runtime Evidence

Before claiming DX12 runtime parity, capture or review a DX12 frame with RenderDoc following:

```text
Docs/Rendering/RenderDocDebugging.md
```

Evidence should show:

- Frame graph pass order matches the prepared execution plan.
- Compute-to-graphics dependencies are visible when compute prepasses feed graphics passes.
- Shader binding groups are stable for frame, object, material, and pass resources.
- Command submission telemetry records serial or parallel command path usage.

## Completion Criteria

- Spec, plan, research, data model, quickstart, and tasks stay in sync.
- Targeted unit tests pass.
- Editor Debug build passes.
- DX12 runtime evidence is recorded before any runtime parity claim.
- Known non-parity remains documented in `research.md`.
