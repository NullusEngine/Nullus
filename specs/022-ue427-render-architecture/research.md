# Research: UE4.27 Render Architecture Alignment

## Decision: Cache stable-frame LightGrid prepared resources before full RDG rewrite

**Reference files**:

- `F:\Epic Games\UE_4.27\Engine\Source\Runtime\Renderer\Private\LightGridInjection.cpp`
- `Runtime/Engine/Rendering/LightGridPrepass.cpp`

**Findings**:

- Tracy evidence showed the expensive path on the main thread under `BaseSceneRenderer::BuildLightGridCompileContext -> LightGridPrepass::BuildPreparedComputeDispatchSource -> LightGridPrepass::Prepare`.
- Nullus was rebuilding CPU vectors, GPU buffers, and binding sets even when the camera, lights, skybox state, and grid settings were unchanged.
- UE4.27 keeps forward lighting grid resources sized/cached and only reallocates selected buffers when byte sizes change; temporary linked-list resources are owned by RDG or view-state resources instead of rebuilding a full resource package every static frame.

**Decision**: Cache `LightGridPrepass` prepared resources for identical frame inputs. On a cache hit, reuse the previous graphics LightGrid binding set and skip LightGrid compute dispatch recording for that frame. Cached LightGrid descriptor sets use persistent allocations so cross-frame reuse does not retain transient frame descriptors.

**Updated 2026-05-10**: Nullus now follows the next UE-style step for dynamic frames: linked-list scratch/output resources are kept in a size-based `PreparedBufferCache`, and a `LightGridReset` compute pass clears `StartOffsetGrid`, `CulledLightLinks`, compact counters, records, and compact indices before injection/compact. This mirrors the UE4.27 pattern of reusing sized grid buffers and clearing GPU resources before `LightGridInject:LinkedList`/compact rather than CPU-filling large arrays every frame.

**Updated 2026-05-10 Option A**: The graphics-facing LightGrid data contract now uses UE4.27-style names: `ForwardLightData`, `ForwardLocalLightBuffer`, `NumCulledLightsGrid`, and `CulledLightDataGrid`. Nullus still preserves its local packing layout, but C++ binding debug names, binding layout entries, HLSL resource names, and shader helper parameters now describe the same forward-light data flow shape as UE4.27 `FForwardLightData`.

**Updated 2026-05-10 Option A follow-up**: The UE4.27 fixed LightGrid build constants are now exposed as contracts instead of duplicated magic numbers: injection group size `4`, `NumCulledLightsGridStride = 2`, and `LightLinkStride = 2`. `LightGridCompileContext` also carries a lightweight `ForwardLightingResources` descriptor with UE-style resource names so the frame graph layer can reason about the forward-light resource bundle without owning the actual RHI buffers.

**Remaining gap**: This is still not full UE4.27 LightGrid parity. Nullus uses an explicit reset compute shader rather than UE4.27 RDG `AddClearUAVPass`/transient RDG resource ownership, and runtime Tracy/RenderDoc evidence has not yet proven the hot path cost on DX12 after this change.

## Decision: Align lifecycle contracts before replacing internals

**Rationale**: UE4.27 spreads rendering architecture across RHI command lists, RDG, shader parameter metadata, renderer mesh draw commands, and task graph scheduling. Nullus already has corresponding concepts, but their names and boundaries differ. A staged compatibility contract lets tests lock UE-like data flow while preserving Editor and Game runtime viability.

**Alternatives considered**:

- Directly rename Nullus systems to UE names: rejected because it would hide behavioral gaps and create noisy architecture churn.
- Replace RHI, frame graph, and threading in one pass: rejected because it would violate incremental validation and make regressions hard to isolate.
- Keep only current Nullus abstractions: rejected because the user explicitly wants UE4.27-aligned workflows.

## Decision: Use UE4.27 `FRHICommandList*` as command-list reference

**Reference files**:

- `F:\Epic Games\UE_4.27\Engine\Source\Runtime\RHI\Public\RHICommandList.h`

**Findings**:

- UE separates command recording objects from immediate command list execution.
- `FRHICommandListBase`, `FRHICommandList`, `FRHICommandListImmediate`, and `FRHICommandListExecutor` define parent/child command list flow, immediate submit, and queued command list execution.
- Nullus currently exposes backend command buffers through `Runtime/Rendering/RHI/Core/RHICommand.h` and frame submission telemetry through `Runtime/Rendering/Context/ThreadedRenderingLifecycle.h`.

**Nullus gap**:

- Nullus command buffers record commands but do not expose an engine-level command list contract that captures child-list ordering and UE-like submission metadata independent of backend objects.

## Decision: Use UE4.27 RDG pass/resource declaration as frame graph reference

**Reference files**:

- `F:\Epic Games\UE_4.27\Engine\Source\Runtime\RenderCore\Public\RenderGraph.h`
- `F:\Epic Games\UE_4.27\Engine\Source\Runtime\RenderCore\Private\RenderGraphBuilder.cpp`

**Findings**:

- UE RDG requires resources used by a pass to be declared in pass parameters before execution.
- `FRDGBuilder::Compile()` connects dependencies, culls unused passes, and creates barriers before `Execute()`.
- Side-effect passes and extracted resources prevent culling.
- Resource transitions are owned by the graph rather than hidden inside pass bodies.

**Nullus gap**:

- Nullus has frame graph pass metadata and explicit access vectors, but it still needs a clearer validation layer for undeclared resource use, side effects, culling eligibility, and exported transition ownership.

## Decision: Use UE4.27 shader parameter struct behavior as shader binding reference

**Reference files**:

- `F:\Epic Games\UE_4.27\Engine\Source\Runtime\RenderCore\Public\ShaderParameterStruct.h`
- `F:\Epic Games\UE_4.27\Engine\Source\Runtime\RenderCore\Private\ShaderParameterStruct.cpp`

**Findings**:

- UE routes shader binding through parameter metadata and `SetShaderParameters`.
- Shader resources are grouped into structured parameter layouts, and validation happens before command list binding.
- Nullus currently maps reflection to binding layouts through `Runtime/Rendering/Resources/ShaderBindingLayoutUtils.*` and binding objects in `Runtime/Rendering/RHI/Core/RHIBinding.h`.

**Nullus gap**:

- Nullus validates reflection binding conflicts, but it does not yet expose a renderer-level shader parameter group contract that corresponds to frame/object/material/pass parameter structs.

## Decision: Use UE4.27 parallel mesh draw command flow as threaded rendering reference

**Reference files**:

- `F:\Epic Games\UE_4.27\Engine\Source\Runtime\Renderer\Private\MeshDrawCommands.h`
- `F:\Epic Games\UE_4.27\Engine\Source\Runtime\Renderer\Private\SceneRendering.h`

**Findings**:

- `FParallelMeshDrawCommandPass` stores draw command setup and dispatches through command lists.
- `FMeshDrawCommand` is the handoff unit between scene preparation and RHI submission.
- Nullus has `RecordedDrawCommandInput`, `RenderScenePackage`, `ParallelCommandWorkUnit`, and `ThreadedRenderingLifecycle` for a similar handoff.

**Nullus gap**:

- Nullus draw commands are prepared, but the explicit batch abstraction and eligibility metadata need to become a stable contract before deeper UE-style parallel translation work.

## Decision: DX12 is the first runtime evidence target

**Rationale**: The current renderer and LightGrid work have strongest existing coverage on DX12. The constitution requires backend claims to match evidence. Other backend claims must wait for backend-specific verification.

**Alternatives considered**:

- Claim backend-neutral parity from contract tests: rejected because backend execution may differ.
- Delay all work until RenderDoc evidence exists: rejected because contract tests can safely drive the first architecture slice.

## Known Non-Parity

- UE4.27 task graph and Nullus threading are not identical; this feature aligns lifecycle handoff and dependency contracts first.
- UE4.27 renderer mesh pass processors are not directly present in Nullus; Nullus will align around recorded draw command batches.
- UE4.27 RDG parameter structs are macro-heavy; Nullus will use original metadata and reflection utilities to expose equivalent grouping behavior.
- Full cross-backend parity is not claimed until each backend is validated.

## Validation Notes

- 2026-05-10 LightGrid reset/cache validation passed with:
  `cmake --build .\Build --config Debug --target NullusUnitTests`.
- 2026-05-10 LightGrid reset/cache contract validation passed with:
  `NullusUnitTests.exe --gtest_filter=RenderFrameworkContractTests.*LightGrid*:RenderFrameworkContractTests.*UE427*:FrameGraphSceneTargetsTests.*LightGrid*:FrameGraphSceneTargetsTests.*UE427*:ShaderBindingLayoutUtilsTests.*:ThreadedRenderingLifecycleTests.*LightGrid*:ThreadedRenderingLifecycleTests.*UE427*`.
- 2026-05-10 Editor Debug build passed with:
  `cmake --build .\Build --config Debug --target Editor`.
- 2026-05-10 DXC validation passed for:
  `LightGridReset.hlsl`, `LightGridInjection.hlsl`, and `LightGridCompact.hlsl` using `CSMain`/`cs_6_0`.
- Contract validation passed for the staged UE4.27 architecture slice with:
  `NullusUnitTests.exe --gtest_filter=RenderFrameworkContractTests.*UE427*:FrameGraphSceneTargetsTests.*UE427*:ShaderBindingLayoutUtilsTests.*UE427*:ThreadedRenderingLifecycleTests.*UE427*`.
- Editor Debug build passed with `cmake --build .\Build --config Debug --target Editor`.
- DX12 RenderDoc evidence is still blocked: `py -3 Tools\RenderDoc\renderdoc_runner.py --target editor --backend dx12 --capture --capture-after-frames 120 --timeout 90` launched `Editor.exe` and found `C:\Program Files\RenderDoc\renderdoccmd.exe`, but no `.rdc` was written under `Build\RenderDocCaptures\editor\dx12` before timeout.
- Because no DX12 `.rdc` capture was produced, runtime parity and backend parity remain unproven beyond the tested architecture contracts.
