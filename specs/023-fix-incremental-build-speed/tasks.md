# Tasks: Fix Incremental Build Speed

**Input**: Design documents from `specs/023-fix-incremental-build-speed/`
**Prerequisites**: `plan.md`, `spec.md`

## Phase 1: Foundational Verification

- [X] T001 Capture current repeated MetaParser generation timestamp behavior for `Runtime/Base/Gen/MetaGenerated.cpp`
- [X] T002 Capture current targeted Windows build command behavior for `build_windows.bat`

## Phase 2: User Story 1 - Fast No-Change Developer Build (Priority: P1)

**Goal**: Repeated generation must not touch unchanged generated reflection files.

**Independent Test**: Invoke the existing MetaParser input twice and confirm unchanged outputs keep the same `LastWriteTimeUtc`.

- [X] T003 [US1] Add a content-stable generated file write helper in `Tools/MetaParser/src/MetaParserTool.Generation.cs`
- [X] T004 [US1] Replace direct generated-output `File.WriteAllText` calls in `Tools/MetaParser/src/MetaParserTool.Generation.cs`
- [X] T005 [US1] Remove parallel shared native dependency copy races from generated Windows MetaParser launchers in `Runtime/CMakeLists.txt`
- [X] T006 [US1] Verify repeated MetaParser generation preserves unchanged generated file timestamps
- [X] T007 [US1] Verify changed inputs still update generated file timestamps through the normal build path

## Phase 3: User Story 2 - Configurable Windows Build Parallelism (Priority: P2)

**Goal**: Targeted Windows builds use configurable parallelism instead of hard-coded single-worker execution.

**Independent Test**: Run the script in a dry or inspected mode and confirm `NLS_BUILD_JOBS=1` maps to `/m:1`, while the unset case maps to `/m`.

- [X] T008 [US2] Update `build_windows.bat` to derive MSBuild `/m` arguments from `NLS_BUILD_JOBS`
- [X] T009 [US2] Update Windows local testing guidance in `Docs/Testing.md`

## Phase 4: Validation

- [X] T010 Run targeted build `cmake --build Build --config Debug --target NullusUnitTests ReflectionTest`
- [X] T011 Run the same targeted build again and record no-change elapsed time plus generated timestamp stability
- [X] T012 Run `Build/bin/Debug/NullusUnitTests.exe`
- [X] T013 Run `Build/bin/Debug/ReflectionTest.exe`
- [X] T014 Self-review diff for generated output edits, platform assumptions, and unrelated changes

## Phase 5: User Story 3 - Faster Full Developer Build (Priority: P1)

**Goal**: Clean Windows Debug builds use MSVC target-internal parallelism and avoid compiling unused Assimp formats by default.

**Independent Test**: Configure and build a fresh Debug tree, then compare elapsed time against the 10:42 baseline and run the reflection/unit test smoke checks.

- [X] T015 [US3] Record clean Debug full-build baseline from `BuildFullBaseline` and optimized experiment logs under `BuildLogs/`
- [X] T016 [US3] Add MSVC `/MP` target-internal compile parallelism in `CMakeLists.txt` without replacing existing flags
- [X] T017 [US3] Add `NLS_ASSIMP_BUILD_ALL_FORMATS` option and default Assimp to FBX/OBJ import-only in `ThirdParty/CMakeLists.txt`
- [X] T018 [US3] Document full-build defaults, `NLS_ASSIMP_BUILD_ALL_FORMATS`, and measured commands in `Docs/Testing.md`
- [X] T019 [US3] Verify fresh optimized Debug full build from a new build directory
- [X] T020 [US3] Verify `Build/bin/Debug/ReflectionTest.exe` and targeted `NullusUnitTests` smoke tests after optimized build
- [X] T021 [US3] Self-review full-build changes for platform assumptions and format-support regressions

## Phase 6: User Story 4 - Header Include Hygiene For Faster Full Builds (Priority: P2)

**Goal**: Reduce full-build compile fan-out by moving heavyweight implementation-only dependencies out of public headers and into `.cpp` or narrow detail headers.

**Independent Test**: For each task, rebuild the affected target and confirm public APIs still compile without pulling implementation-only third-party/platform headers into unrelated translation units.

- [X] T022 [US4] Capture public-header include baseline with `/showIncludes` or MSBuild binlog sampling for `Runtime`, `Project`, and `Tests`, then record top fan-out headers in `BuildLogs/`
- [X] T023 [US4] Decouple `Runtime/Rendering/Resources/Loaders/ModelLoader.h` from `Runtime/Rendering/Resources/Parsers/AssimpParser.h` by moving the concrete parser dependency into `Runtime/Rendering/Resources/Loaders/ModelLoader.cpp`
- [X] T024 [US4] Move `Runtime/Platform/Windowing/Dialogs/portable-file-dialogs.h` out of `Runtime/Platform/Windowing/Dialogs/MessageBox.h`, `OpenFileDialog.h`, `SaveFileDialog.h`, and `SelectFolderDialog.h`
- [X] T025 [US4] Audit `Runtime/Rendering/Context/DriverAccess.h` and `Runtime/Rendering/Context/ThreadedRenderingLifecycle.h` for forward declarations and move implementation-only includes to matching `.cpp` files
- [X] T026 [US4] Split or narrow `Runtime/Rendering/Settings/GraphicsBackendUtils.h` and `Runtime/Rendering/Settings/DriverSettings.h` so backend parsing/helpers do not force large utility includes into every consumer
- [X] T027 [US4] Reduce component propagation from `Runtime/Engine/GameObject.h`, `Runtime/Engine/Components/MeshRenderer.h`, `Runtime/Engine/Components/TransformComponent.h`, `Runtime/Engine/Components/LightComponent.h`, and `Runtime/Engine/Components/CameraComponent.h` using forward declarations where only pointers or references are required
- [X] T028 [US4] Audit public RHI core headers `Runtime/Rendering/RHI/Core/RHIResource.h`, `Runtime/Rendering/RHI/Core/RHIDevice.h`, `Runtime/Rendering/RHI/Core/RHICommand.h`, `Runtime/Rendering/RHI/Core/RHIPipeline.h`, `Runtime/Rendering/RHI/Core/RHIBinding.h`, and `Runtime/Rendering/RHI/Core/RHISwapchain.h` for unnecessary concrete includes
- [X] T029 [US4] Isolate DX12 platform headers from public backend headers `Runtime/Rendering/RHI/Backends/DX12/DX12Device.h`, `DX12Swapchain.h`, `DX12Command.h`, `DX12Descriptor.h`, `DX12Pipeline.h`, `DX12Resource.h`, and `DX12Synchronization.h`
- [X] T030 [US4] Move `ImGui/imgui.h` out of public UI headers `Runtime/UI/Widgets/AWidget.h`, `Runtime/UI/Modules/Canvas.h`, `Runtime/UI/Internal/Converter.h`, `Runtime/UI/Plugins/DDSource.h`, `Runtime/UI/Plugins/DDTarget.h`, `Runtime/UI/Icons/FontAwesomeIconFont.h`, and `Project/Launcher/Core/LauncherTheme.h` where signatures can use forward declarations or local `.cpp` includes
- [X] T031 [US4] Reduce resource header coupling in `Runtime/Rendering/Resources/Material.h`, `Runtime/Rendering/Resources/Shader.h`, `Runtime/Rendering/Resources/ShaderParameterStruct.h`, `Runtime/Rendering/Resources/Texture2D.h`, `Runtime/Rendering/Resources/TextureCube.h`, and `Runtime/Rendering/Resources/Mesh.h`
- [X] T032 [US4] Audit renderer aggregation headers `Runtime/Rendering/Core/ABaseRenderer.h`, `Runtime/Engine/Rendering/BaseSceneRenderer.h`, `Runtime/Engine/Rendering/LightGridPrepass.h`, `Runtime/Engine/Rendering/DeferredSceneRenderer.h`, and `Runtime/Rendering/Core/CompositeRenderer.h` for move-to-`.cpp` dependencies
- [X] T033 [US4] Split `Runtime/Rendering/FrameGraph/FrameGraphExecutionPlan.h` into smaller declaration/detail headers or move execution-only code into `.cpp` to reduce its 19-include, large-header fan-out
- [X] T034 [US4] Reduce `Json/json.hpp` propagation from `Runtime/Base/Guid.h`, `Runtime/Base/Reflection/JsonConfig.h`, `Runtime/Core/Serialize/IJsonHandler.h`, `Runtime/Engine/Serialize/ObjectGraphReader.h`, and `Runtime/Engine/Serialize/ObjectGraphWriter.h`
- [X] T035 [US4] Move the `Windows.h` dependency out of `Runtime/Rendering/Tooling/RenderDocEnvironment.h` into `Runtime/Rendering/Tooling/RenderDocEnvironment.cpp` or a platform detail header
- [X] T036 [US4] Audit editor context headers `Project/Editor/Core/Context.h`, `Project/Game/Core/Context.h`, `Project/Editor/Panels/AView.h`, `Project/Editor/Panels/Inspector.h`, and `Project/Editor/Panels/AssetProperties.h` for heavy engine/UI includes that can be forward-declared
- [X] T037 [US4] Add include hygiene guidance to `Docs/Testing.md`: public headers should not include Assimp, ImGui, Windows, DX12, or `Json/json.hpp` unless the public signature requires concrete types
- [X] T038 [US4] Verify header hygiene changes with `cmake --build Build --config Debug --target NLS_Render NullusUnitTests ReflectionTest`
- [X] T039 [US4] Re-run include baseline sampling from `BuildLogs/` and compare top fan-out headers against T022 results
- [X] T040 [US4] Self-review header include changes for ABI/API drift, generated-file edits, platform assumptions, and backend-specific regressions

## Phase 7: User Story 5 - Build Acceleration With PCH And C++20 Modules (Priority: P2)

**Goal**: Add opt-in build-system support for precompiled headers first, then evaluate C++20 Modules on a narrow, low-risk slice without breaking existing CMake generators or non-MSVC platforms.

**Independent Test**: Enable each option in a fresh build directory, perform a clean Debug build and a no-change rebuild, then compare elapsed time and verify `NullusUnitTests` plus `ReflectionTest`.

- [X] T041 [US5] Capture current compiler, generator, CMake version, and clean Debug build timing in `BuildLogs/` before adding PCH or Modules
- [X] T042 [US5] Add `NLS_ENABLE_PCH` option in `CMakeLists.txt` defaulting to `ON` for supported native C++ targets and allowing a full opt-out for debugging build issues
- [X] T043 [US5] Create a minimal shared runtime PCH header such as `Runtime/NullusRuntimePch.h` containing stable standard-library and project-wide headers only
- [X] T044 [US5] Apply `target_precompile_headers` to runtime library targets in `Runtime/CMakeLists.txt` while excluding generated files under `Runtime/*/Gen/` and files that are sensitive to include order
- [X] T045 [US5] Create separate narrow PCH headers for editor/game/test targets such as `Project/NullusProjectPch.h` and `Tests/NullusTestsPch.h` instead of forcing editor-only or gtest headers into runtime builds
- [X] T046 [US5] Update `Docs/Testing.md` with PCH enable/disable commands, expected troubleshooting path, and how to compare full-build timings
- [X] T047 [US5] Verify PCH-enabled build with `cmake --build Build --config Debug --target NLS_Render NullusUnitTests ReflectionTest`
- [X] T048 [US5] Verify PCH opt-out build with `-DNLS_ENABLE_PCH=OFF` in a fresh build directory to keep the old include model working
- [X] T049 [US5] Audit PCH contents after include-hygiene tasks so heavyweight headers like Assimp, ImGui, Windows, DX12, and `Json/json.hpp` are not added just to hide include coupling
- [X] T050 [US5] Research required CMake and compiler support for C++20 Modules in `CMakeLists.txt`, noting that the current root `cmake_minimum_required(VERSION 3.18)` is below practical module support levels
- [X] T051 [US5] Add an opt-in `NLS_ENABLE_CXX_MODULES` option in `CMakeLists.txt` defaulting to `OFF` until the supported CMake/generator/compiler matrix is validated
- [X] T052 [US5] Add configurable `NLS_MSVC_MP_JOBS` support in `CMakeLists.txt` and `build_windows.bat` so outer MSBuild `/m` and inner MSVC `/MPn` can be tuned independently
- [X] T053 [US5] Create a tiny module pilot for a low-dependency utility area such as `Runtime/Base` or `Runtime/Math`, using CMake file sets or the selected compiler-specific flow without changing public runtime behavior
- [X] T054 [US5] Keep module adoption separate from reflection-generated code by documenting whether `MetaParser` can parse `.ixx`, `.cppm`, or module interface files before moving any reflected types
- [X] T055 [US5] Verify the module pilot on Windows MSVC first, then document unsupported generator/platform behavior and fallback to normal headers when `NLS_ENABLE_CXX_MODULES=OFF`
- [X] T056 [US5] Compare PCH-only, Modules-only pilot, and PCH-plus-Modules clean build timings in `BuildLogs/` and decide whether broader module migration is worth scheduling
- [X] T057 [US5] Self-review PCH and Modules changes for ODR risk, macro leakage, generated-file interactions, toolchain version assumptions, and cross-platform fallback behavior
