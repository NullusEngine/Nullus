# Header Include Baseline

Date: 2026-05-11

This snapshot was collected with `rg` over `Runtime`, `Project`, and `Tests` headers before the first include hygiene implementation task.

## Header Files With Many Direct Includes

| Direct includes | Header |
| ---: | --- |
| 25 | `Runtime/Platform/Windowing/Dialogs/portable-file-dialogs.h` |
| 21 | `Runtime/Rendering/Core/ABaseRenderer.h` |
| 20 | `Project/Editor/Core/Context.h` |
| 19 | `Runtime/Rendering/FrameGraph/FrameGraphExecutionPlan.h` |
| 18 | `Runtime/Rendering/Resources/Material.h` |
| 18 | `Runtime/NullusRuntimePch.h` |
| 17 | `Runtime/Rendering/Context/ThreadedRenderingLifecycle.h` |
| 16 | `Runtime/Engine/Rendering/BaseSceneRenderer.h` |
| 15 | `Runtime/UI/GUIDrawer.h` |
| 15 | `Runtime/Rendering/Context/DriverInternal.h` |
| 14 | `Runtime/UI/ImGuiExtensions/TimelineProfiler/Profiler.h` |
| 13 | `Runtime/Engine/Rendering/LightGridPrepass.h` |
| 12 | `Runtime/Rendering/Settings/GraphicsBackendUtils.h` |
| 11 | `Runtime/Rendering/Context/DriverAccess.h` |
| 10 | `Runtime/Engine/GameObject.h` |
| 10 | `Runtime/Rendering/RHI/Backends/DX12/DX12Command.h` |

## High-Impact Public Header Dependencies

- `Runtime/Rendering/Resources/Loaders/ModelLoader.h` includes `Rendering/Resources/Parsers/AssimpParser.h`.
- Dialog public headers include `Runtime/Platform/Windowing/Dialogs/portable-file-dialogs.h`.
- Several public UI headers include `ImGui/imgui.h`.
- Several DX12 public backend headers include `d3d12.h`, `dxgi1_6.h`, or `wrl/client.h`.
- JSON propagates through `Runtime/Base/Guid.h`, `Runtime/Base/Reflection/JsonConfig.h`, `Runtime/Core/Serialize/IJsonHandler.h`, `Runtime/Engine/Serialize/ObjectGraphReader.h`, and `Runtime/Engine/Serialize/ObjectGraphWriter.h`.
- `Runtime/Rendering/Tooling/RenderDocEnvironment.h` includes `Windows.h`.

## Most Frequent Header Includes

| Count | Include |
| ---: | --- |
| 130 | `string` |
| 101 | `vector` |
| 89 | `cstdint` |
| 85 | `RenderDef.h` |
| 75 | `memory` |
| 34 | `optional` |
| 33 | `BaseDef.h` |
| 32 | `Eventing/Event.h` |
| 29 | `functional` |
| 29 | `EngineDef.h` |
| 29 | `string_view` |
| 29 | `unordered_map` |
| 25 | `Reflection/Type.h` |
| 25 | `type_traits` |
| 20 | `Rendering/RHI/Core/RHIResource.h` |

## First Implementation Result

- `Runtime/Rendering/Resources/Loaders/ModelLoader.h` no longer includes `Rendering/Resources/Parsers/AssimpParser.h`.
- `Runtime/Rendering/Resources/Parsers/EModelParserFlags.h` was made self-contained by including `<cstdint>`.
- Verification build passed: `cmake --build Build --config Debug --target NLS_Render NullusUnitTests ReflectionTest -- /m:4`.
- `Build/bin/Debug/ReflectionTest.exe` passed.
- `Build/bin/Debug/NullusUnitTests.exe` had one full-suite failure in `PanelWindowHookTests.ProfilerPanelDrawDoesNotAdvanceTimelineFrameInsideActiveScope`; the same test passed when rerun alone and when rerunning `PanelWindowHookTests.*`, so it is tracked as an unrelated flaky/environment-sensitive result for this include-hygiene slice.

## Dialog Header Decoupling Result

- `Runtime/Platform/Windowing/Dialogs/MessageBox.h`, `OpenFileDialog.h`, `SaveFileDialog.h`, `SelectFolderDialog.h`, and the dependent `DesktopNotify.h` no longer include `portable-file-dialogs.h`.
- Public dialog headers now forward declare the required `pfd` classes and hold them through `std::unique_ptr`.
- Verification build passed: `cmake --build Build --config Debug --target NLS_Platform Launcher Editor NullusUnitTests -- /m:4`.
- Focused verification passed: `Build/bin/Debug/NullusUnitTests.exe --gtest_filter=PanelWindowHookTests.*:Launcher*:*AssetBrowser*:*EditorLaunchArgs*`.

## Driver Context Header Audit Result

- `Runtime/Rendering/Context/DriverAccess.h` no longer includes `Rendering/RHI/Core/RHIDevice.h`.
- The header now includes the narrower `Rendering/RHI/Core/RHIEnums.h` and forward declares `RHIReadbackResult`; it still intentionally includes `ThreadedRenderingLifecycle.h` and `DescriptorAllocator.h` because public signatures use `ThreadedFrameTelemetry`, `PreparedRenderSceneBuilder`, `NativeHandle`, and `DescriptorAllocationLifetime` by value or as default-argument types.
- Verification build passed: `cmake --build Build --config Debug --target NLS_Render NullusUnitTests ReflectionTest -- /m:4`.

## Graphics Backend Settings Header Split Result

- Added `Runtime/Rendering/Settings/EngineDiagnosticsSettings.h` for `RenderDocSettings` and `EngineDiagnosticsSettings`.
- `Runtime/Rendering/Settings/GraphicsBackendUtils.h` no longer includes `Runtime/Rendering/Settings/DriverSettings.h`, avoiding propagation of `Rendering/Data/PipelineState.h` through backend parsing/helper utilities.
- `Runtime/Rendering/Settings/DriverSettings.h` now includes the narrower diagnostics settings header for its concrete member fields.
- Verification build passed: `cmake --build Build --config Debug --target NLS_Render NullusUnitTests ReflectionTest -- /m:4`.

## Engine Component Header Decoupling Result

- `Runtime/Engine/Components/MeshRenderer.h` no longer includes `Rendering/Resources/Model.h`; it forward declares `NLS::Render::Resources::Model` and keeps the concrete `BoundingSphere` include because it is stored by value.
- `Runtime/Engine/Components/LightComponent.h` no longer includes `Rendering/Entities/Light.h`; it forward declares the light entity and includes only the lightweight `ELightType`/`Vector3` public signature types.
- `Runtime/Engine/Components/CameraComponent.h` no longer includes `Rendering/Entities/Camera.h`; it forward declares the camera entity and includes only the lightweight `EProjectionMode`/`Vector3` public signature types.
- Consumers that used `Model` implementation details now include `Rendering/Resources/Model.h` directly in their `.cpp` files.
- Verification build passed: `cmake --build Build --config Debug --target NLS_Engine NullusUnitTests ReflectionTest -- /m:4`.

## RHI Core Header Audit Result

- `Runtime/Rendering/RHI/Core/RHIBinding.h` no longer includes `Rendering/RHI/Core/RHIResource.h`; it includes the narrower `RHIEnums.h` and forward declares resource object types used only through `std::shared_ptr`.
- `Runtime/Rendering/RHI/Core/RHICommand.h` now forward declares `RHIVertexBufferView` and `RHIIndexBufferView`, which are reference-only public parameters there.
- `Runtime/Rendering/RHI/Core/RHIDevice.h` still intentionally includes `RHIResource.h` because its public signatures and inline defaults use `RHIBufferDesc`, `RHITextureDesc`, upload descriptors, update results, and resource objects.
- Implementation/backend files that require complete resource definitions now include `RHIResource.h` directly: `Runtime/Rendering/RHI/Utils/UploadContext/UploadContext.cpp` and `Runtime/Rendering/RHI/Backends/DX12/DX12Swapchain.h`.
- Verification build passed: `cmake --build Build --config Debug --target NLS_Render NullusUnitTests ReflectionTest -- /m:4`.

## DX12 Public Backend Header Audit Result

- Added `Runtime/Rendering/RHI/Backends/DX12/DX12Access.h` for the small binding/pipeline access interfaces previously declared in `DX12Command.h`.
- `Runtime/Rendering/RHI/Backends/DX12/DX12Descriptor.h` and `Runtime/Rendering/RHI/Backends/DX12/DX12Pipeline.h` now include `DX12Access.h` instead of the heavier `DX12Command.h`, avoiding propagation of the full command-buffer API and profiler header into descriptor/pipeline consumers.
- `Runtime/Rendering/RHI/Backends/DX12/DX12Resource.h` no longer includes `Rendering/RHI/Utils/UploadContext/UploadContext.h`; it forward declares `UploadBackend`, and `DX12Resource.cpp` includes the concrete upload context header where the implementation needs it.
- Public DX12 headers still intentionally include `d3d12.h`, `dxgi1_6.h`, or `wrl/client.h` where public constructors, return values, base-interface overrides, or stored `Microsoft::WRL::ComPtr` members expose concrete DX12 ABI types. Fully hiding those platform headers would require a larger pImpl/handle-wrapper refactor.
- Verification build passed: `cmake --build Build --config Debug --target NLS_Render NullusUnitTests ReflectionTest -- /m:4`.

## UI ImGui Header Decoupling Result

- `Runtime/UI/Widgets/AWidget.h` and `Runtime/UI/Modules/Canvas.h` no longer include `ImGui/imgui.h`; neither header uses ImGui types directly.
- `Runtime/UI/Internal/Converter.h` now forward declares `ImVec2` and `ImVec4`; `Converter.cpp` includes `ImGui/imgui.h` for the concrete definitions.
- `Runtime/UI/Icons/FontAwesomeIconFont.h` now forward declares `ImFont` and uses the `ImWchar` underlying type locally; `FontAwesomeIconFont.cpp` includes `ImGui/imgui.h`.
- Added `Runtime/UI/Plugins/DragDrop.h/.cpp` so `Runtime/UI/Plugins/DDSource.h` and `DDTarget.h` no longer include `ImGui/imgui.h`, `UI/UIManager.h`, or `ServiceLocator.h` for inline drag/drop templates.
- `Runtime/UI/UIManager.h` and scalar input/drag/slider template headers still intentionally include `ImGui/imgui.h` because their public signatures or inline implementations expose ImGui types directly.
- `Project/Launcher/Core/LauncherTheme.h` still intentionally includes `ImGui/imgui.h` because it is an inline theme helper header that calls ImGui APIs and uses ImGui enums/types in function bodies.
- Verification build passed after reconfiguring for the new source file: `cmake -S . -B Build`, then `cmake --build Build --config Debug --target NLS_UI NullusUnitTests ReflectionTest -- /m:4`.

## Resource Header Coupling Audit Result

- `Runtime/Rendering/Resources/Shader.h` no longer includes `Rendering/RHI/Core/RHIDevice.h`; it includes the lighter `Rendering/RHI/RHITypes.h` and forward declares `RHIDevice`/`RHIShaderModule`.
- `Runtime/Rendering/Resources/Shader.cpp` now includes `RHIDevice.h` where it calls `GetAdapter()` and `CreateShaderModule()`.
- `Runtime/Rendering/Resources/Texture.h` no longer includes `Rendering/RHI/Core/RHIResource.h`; it includes `Rendering/RHI/Core/RHIEnums.h` and forward declares `RHITexture`/`RHITextureView`.
- `Runtime/Rendering/Resources/Texture2D.cpp` now includes `RHIResource.h` where it reads concrete `RHITexture` descriptors.
- `Runtime/Rendering/Resources/Material.h`, `ShaderParameterStruct.h`, and `Mesh.h` still intentionally keep their current concrete includes where public value members, inline helper construction, or templated buffer members require complete public types.
- Verification build passed: `cmake --build Build --config Debug --target NLS_Render NullusUnitTests ReflectionTest -- /m:4`.

## Renderer Aggregation Header Audit Result

- `Runtime/Engine/Rendering/BaseSceneRenderer.h` no longer includes `Rendering/Resources/Mesh.h`, `Rendering/Resources/Material.h`, `GameObject.h`, `Components/CameraComponent.h`, or `SceneSystem/Scene.h`; it forward declares `Material`, `Model`, and `Scene` for public pointer/reference signatures.
- `Runtime/Engine/Rendering/BaseSceneRenderer.cpp` now includes the concrete resource and scene headers used by the implementation.
- `Runtime/Engine/Rendering/DeferredSceneRenderer.h` no longer includes `fg/FrameGraph.hpp`; the concrete `FrameGraph` usage remains in `DeferredSceneRenderer.cpp`.
- `Tests/Unit/RendererFrameObjectBindingTests.cpp` now includes `SceneSystem/Scene.h` and `Components/LightComponent.h` directly instead of relying on `BaseSceneRenderer.h` transitive includes.
- `Runtime/Rendering/Core/ABaseRenderer.h`, `Runtime/Engine/Rendering/LightGridPrepass.h`, and `Runtime/Rendering/Core/CompositeRenderer.h` still intentionally keep their heavier includes where public value members, inline templates, or default argument types require complete definitions.
- Verification build passed: `cmake --build Build --config Debug --target NLS_Engine NullusUnitTests ReflectionTest -- /m:4`.

## FrameGraph Execution Plan Header Split Result

- Added `Runtime/Rendering/FrameGraph/FrameGraphExecutionTypes.h` for execution-plan data types and small metadata helpers without pulling `fg/FrameGraph.hpp` or execution algorithm templates into public consumers.
- `Runtime/Rendering/FrameGraph/SceneRenderGraphBuilderLightGrid.h` and `Runtime/Engine/Rendering/LightGridPrepass.h` now include the lighter execution-types header.
- `Runtime/Rendering/FrameGraph/FrameGraphExecutionPlan.h` now includes the execution-types header and retains the heavier compile/apply algorithm templates for consumers that actually build or execute plans.
- `Runtime/Rendering/FrameGraph/SceneRenderGraphBuilderForward.cpp`, `SceneRenderGraphBuilderDeferred.cpp`, and `Runtime/Engine/Rendering/LightGridPrepass.cpp` include the full execution-plan header where implementation code calls the algorithms.
- Added `Runtime/Engine/Rendering/LightGridPrepassExecution.h` as an explicit helper header for the previously inline LightGrid execution wrappers so `LightGridPrepass.h` no longer has to include the full execution-plan algorithms.
- Verification build passed: `cmake --build Build --config Debug --target NLS_Render NLS_Engine NullusUnitTests ReflectionTest -- /m:4`.

## JSON Header Propagation Audit Result

- Added `Runtime/Base/JsonFwd.h` as a local forward declaration shim for the bundled nlohmann 3.11.3 ABI namespace, including the same fwd include guard used by the bundled single-header file.
- `Runtime/Base/Guid.h` no longer includes `Json/json.hpp`; it includes `JsonFwd.h`, while `Runtime/Base/Guid.cpp` includes the full JSON header for `to_json`/`from_json` implementation.
- `Runtime/Core/Serialize/IJsonHandler.h` now includes `JsonFwd.h` because its public API only passes `json` by reference.
- `Runtime/Base/Reflection/JsonConfig.h` still intentionally includes `Json/json.hpp` because the wrapper stores `nlohmann::json` by value and exposes native object/array aliases and inline operations.
- `Runtime/Engine/Serialize/ObjectGraphReader.h` and `ObjectGraphWriter.h` still intentionally include `Json/json.hpp` because both are inline implementation headers that parse, inspect, build, and return concrete JSON values.
- Verification build passed: `cmake --build Build --config Debug --target NLS_Base NLS_Core NLS_Engine NullusUnitTests ReflectionTest -- /m:4`.

## RenderDoc Environment Windows Header Audit Result

- `Runtime/Rendering/Tooling/RenderDocEnvironment.h` no longer includes `Windows.h`; it now only declares `PreloadRenderDocIfAvailable`.
- Added `Runtime/Rendering/Tooling/RenderDocEnvironment.cpp` with the Windows-specific `GetModuleHandleW`/`LoadLibraryW` implementation and logging.
- Reconfigured CMake so the new source file is picked up by the runtime source glob: `cmake -S . -B Build`.
- Verification build passed: `cmake --build Build --config Debug --target NLS_Render Launcher Editor Game NullusUnitTests ReflectionTest -- /m:4`.

## Editor Context Header Audit Result

- `Project/Editor/Panels/Inspector.h` no longer includes texture loading or concrete UI widget headers that are only needed by `Inspector.cpp`; it forward declares `GameObject`, `Component`, and `UI::Widgets::Group`.
- `Project/Editor/Panels/AssetProperties.h` no longer includes concrete resource/widget headers for pointer-only public and private members; it forward declares `Model`, `Material`, `Texture2D`, and widget pointer types.
- `Project/Editor/Panels/AView.h` no longer includes unused `Rendering/Buffers/UniformBuffer.h` or `Rendering/Core/CompositeRenderer.h`.
- `Project/Editor/Panels/AssetProperties.cpp` and `Project/Editor/Panels/GameView.cpp` now include the concrete widget/buffer headers they use directly.
- `Project/Editor/Core/Context.h` and `Project/Game/Core/Context.h` still intentionally keep many concrete includes because their public context objects own engine managers, settings, and platform objects by value or through inline methods.
- Verification build passed: `cmake --build Build --config Debug --target Editor NullusUnitTests ReflectionTest -- /m:4`.

## Post-Hygiene Include Baseline

Re-sampled direct includes in `Runtime`, `Project`, and `Tests` headers with:

```powershell
$headers = rg --files Runtime Project Tests -g '*.h' -g '*.hpp' -g '*.inl'
$rows = foreach ($h in $headers) { $count = (rg -n '^\s*#\s*include\b' $h | Measure-Object).Count; [pscustomobject]@{Count=$count; Header=$h} }
$rows | Sort-Object Count -Descending | Select-Object -First 24
```

| Direct includes | Header |
| ---: | --- |
| 25 | `Runtime/Platform/Windowing/Dialogs/portable-file-dialogs.h` |
| 21 | `Runtime/Rendering/Core/ABaseRenderer.h` |
| 20 | `Project/Editor/Core/Context.h` |
| 18 | `Runtime/Rendering/FrameGraph/FrameGraphExecutionPlan.h` |
| 18 | `Runtime/NullusRuntimePch.h` |
| 18 | `Runtime/Rendering/Resources/Material.h` |
| 17 | `Runtime/Rendering/Context/ThreadedRenderingLifecycle.h` |
| 15 | `Runtime/UI/GUIDrawer.h` |
| 15 | `Runtime/Rendering/Context/DriverInternal.h` |
| 14 | `Runtime/Base/Reflection/TypeData.h` |
| 14 | `Runtime/UI/ImGuiExtensions/TimelineProfiler/Profiler.h` |
| 14 | `Project/Editor/Rendering/PickingRenderPass.h` |
| 14 | `Tests/Unit/ReflectionTestUtils.h` |
| 14 | `Project/Game/Core/Context.h` |
| 13 | `Runtime/Engine/Rendering/LightGridPrepass.h` |
| 13 | `Runtime/Rendering/FrameGraph/FrameGraphExecutionTypes.h` |
| 13 | `Runtime/Base/Reflection/Meta.h` |
| 12 | `Runtime/Rendering/Settings/GraphicsBackendUtils.h` |
| 12 | `Runtime/Rendering/Data/PipelineState.h` |
| 12 | `Runtime/Engine/Serialize/ObjectGraphInstantiator.h` |
| 12 | `Project/Editor/Rendering/GridRenderPass.h` |
| 12 | `Runtime/Rendering/RHI/Backends/DX12/DX12Descriptor.h` |
| 12 | `Runtime/Base/Reflection/Type.h` |
| 11 | `Runtime/Rendering/Resources/ShaderParameterStruct.h` |

Most frequent includes after hygiene:

| Count | Include |
| ---: | --- |
| 134 | `<string>` |
| 106 | `<vector>` |
| 96 | `<cstdint>` |
| 86 | `"RenderDef.h"` |
| 82 | `<memory>` |
| 35 | `<optional>` |
| 33 | `"BaseDef.h"` |
| 30 | `<functional>` |
| 30 | `<string_view>` |
| 29 | `"EngineDef.h"` |
| 29 | `<unordered_map>` |
| 29 | `<Eventing/Event.h>` |
| 25 | `<type_traits>` |
| 22 | `<algorithm>` |
| 21 | `"Reflection/Type.h"` |
| 20 | `"Rendering/RHI/Core/RHIResource.h"` |

High-cost public header scan after hygiene:

- `Windows.h`: no matches in `Runtime`, `Project`, or `Tests` public headers.
- Assimp headers: no matches in `Runtime`, `Project`, or `Tests` public headers.
- `Json/json.hpp`: remains only in `Runtime/Base/Reflection/JsonConfig.h`, `Runtime/Engine/Serialize/ObjectGraphReader.h`, and `Runtime/Engine/Serialize/ObjectGraphWriter.h`, where inline code or by-value storage requires the complete type.
- ImGui headers: remain only in headers whose public signatures or inline implementation use ImGui directly: `Project/Launcher/Core/LauncherTheme.h`, `Runtime/UI/UIManager.h`, scalar widget templates, and the timeline profiler compatibility header.

Self-review checks:

- `git diff --name-only -- Runtime/*/Gen Project/Editor/Gen`: no generated-file diffs.
- `git -C ThirdParty\ImGui status --short`: clean; no submodule edits.
- `git diff --check`: only CRLF conversion warnings, no whitespace errors.
- Header-hygiene verification builds passed through the targeted commands listed in each audit section above.
