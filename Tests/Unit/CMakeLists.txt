file(GLOB_RECURSE NULLUS_UNIT_TEST_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.inl"
)

file(GLOB_RECURSE NULLUS_RENDERING_TEST_FILES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/Tests/Rendering/*.cpp"
    "${CMAKE_SOURCE_DIR}/Tests/Rendering/*.c"
    "${CMAKE_SOURCE_DIR}/Tests/Rendering/*.h"
    "${CMAKE_SOURCE_DIR}/Tests/Rendering/*.hpp"
    "${CMAKE_SOURCE_DIR}/Tests/Rendering/*.inl"
)

# Exclude DX12 tests on non-Windows platforms
if(NOT WIN32)
    list(FILTER NULLUS_UNIT_TEST_FILES EXCLUDE REGEX "[/\\\\]DX12.*\\.cpp$")
    list(FILTER NULLUS_RENDERING_TEST_FILES EXCLUDE REGEX "[/\\\\]DX12.*\\.cpp$")
endif()

set(NULLUS_UNIT_TEST_C_FILES ${NULLUS_UNIT_TEST_FILES} ${NULLUS_RENDERING_TEST_FILES})
list(FILTER NULLUS_UNIT_TEST_C_FILES INCLUDE REGEX "\\.c$")
if(NULLUS_UNIT_TEST_C_FILES)
    set_source_files_properties(${NULLUS_UNIT_TEST_C_FILES} PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
endif()

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${NULLUS_UNIT_TEST_FILES})
source_group(TREE "${CMAKE_SOURCE_DIR}/Tests/Rendering" PREFIX "Rendering Tests" FILES ${NULLUS_RENDERING_TEST_FILES})
source_group("Test Infrastructure" FILES "${CMAKE_SOURCE_DIR}/Tests/NullusTestMain.cpp")

add_executable(NullusUnitTests
    ${NULLUS_UNIT_TEST_FILES}
    ${NULLUS_RENDERING_TEST_FILES}
    "${CMAKE_SOURCE_DIR}/Tests/NullusTestMain.cpp"
)

nls_configure_test_target(NullusUnitTests)

add_test(NAME NullusUnitTests COMMAND NullusUnitTests)

add_executable(NullusAnimationTests AnimationTests.cpp)
target_compile_features(NullusAnimationTests PRIVATE cxx_std_20)
target_link_libraries(NullusAnimationTests PRIVATE gtest_main NLS_AnimationCompiler)
set_target_properties(NullusAnimationTests PROPERTIES FOLDER Tests RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/runtime")
target_compile_options(NullusAnimationTests PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/Z7>)
add_test(NAME NullusAnimationTests COMMAND NullusAnimationTests)
set_tests_properties(NullusAnimationTests PROPERTIES LABELS "animation;behavior" WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
add_executable(NullusAnimationImportTests AnimationImportTests.cpp)
target_compile_features(NullusAnimationImportTests PRIVATE cxx_std_20)
target_compile_definitions(NullusAnimationImportTests PRIVATE NLS_ROOT_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(NullusAnimationImportTests PRIVATE gtest_main NLS_Render)
set_target_properties(NullusAnimationImportTests PROPERTIES FOLDER Tests RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/runtime")
add_test(NAME NullusAnimationImportTests COMMAND NullusAnimationImportTests)
set_tests_properties(NullusAnimationImportTests PROPERTIES LABELS "animation;import" WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
add_executable(NullusAnimationGpuTests AnimationGpuTests.cpp)
target_compile_features(NullusAnimationGpuTests PRIVATE cxx_std_20)
target_compile_definitions(NullusAnimationGpuTests PRIVATE NLS_ROOT_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(NullusAnimationGpuTests PRIVATE gtest_main NLS_Render NLS_MaterialCompiler)
set_target_properties(NullusAnimationGpuTests PROPERTIES FOLDER Tests RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/runtime")
add_test(NAME NullusAnimationGpuTests COMMAND NullusAnimationGpuTests)
set_tests_properties(NullusAnimationGpuTests PROPERTIES LABELS "animation;gpu" WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
add_executable(NullusAnimationEditorTests AnimationEditorTests.cpp "${CMAKE_SOURCE_DIR}/Tests/NullusTestMain.cpp")
nls_configure_test_target(NullusAnimationEditorTests NO_WHOLE_ARCHIVE)
add_test(NAME NullusAnimationEditorTests COMMAND NullusAnimationEditorTests)
set_tests_properties(NullusAnimationEditorTests PROPERTIES LABELS "animation;editor" WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

add_executable(NullusDistanceFieldTests DistanceFieldTests.cpp
    UploadContextTests.cpp DX12TextureUploadUtilsTests.cpp DX12TextureViewUtilsTests.cpp
    ShaderBindingLayoutUtilsTests.cpp)
if(NOT WIN32)
    set_property(TARGET NullusDistanceFieldTests PROPERTY SOURCES
        DistanceFieldTests.cpp UploadContextTests.cpp ShaderBindingLayoutUtilsTests.cpp)
endif()
target_compile_features(NullusDistanceFieldTests PRIVATE cxx_std_20)
target_include_directories(NullusDistanceFieldTests PRIVATE "${CMAKE_SOURCE_DIR}/Runtime")
target_compile_definitions(NullusDistanceFieldTests PRIVATE NLS_ROOT_DIR="${CMAKE_SOURCE_DIR}" NLS_BUILD_DIR="${CMAKE_BINARY_DIR}")
target_link_libraries(NullusDistanceFieldTests PRIVATE gtest_main NLS_Render NLS_StaticMeshBuild)
set_target_properties(NullusDistanceFieldTests PROPERTIES FOLDER Tests RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/distance-field-tests")
nls_copy_shared_runtime_to_target(NullusDistanceFieldTests)
add_test(NAME NullusDistanceFieldTests COMMAND NullusDistanceFieldTests)
add_executable(NullusDistanceFieldIntegrationTests DistanceFieldIntegrationTests.cpp)
target_compile_features(NullusDistanceFieldIntegrationTests PRIVATE cxx_std_20)
target_include_directories(NullusDistanceFieldIntegrationTests PRIVATE "${CMAKE_SOURCE_DIR}/Runtime" "${CMAKE_SOURCE_DIR}/Project/Editor")
target_compile_definitions(NullusDistanceFieldIntegrationTests PRIVATE NLS_ROOT_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(NullusDistanceFieldIntegrationTests PRIVATE gtest_main NLS_Render NLS_StaticMeshBuild NLS_Engine NLS_Cook EditorProject)
set_target_properties(NullusDistanceFieldIntegrationTests PROPERTIES FOLDER Tests RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/distance-field-tests")
nls_copy_shared_runtime_to_target(NullusDistanceFieldIntegrationTests)
add_test(NAME NullusDistanceFieldIntegrationTests COMMAND NullusDistanceFieldIntegrationTests)
set_tests_properties(NullusDistanceFieldTests NullusDistanceFieldIntegrationTests PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" LABELS "distance-field")

# Focused UI Designer suite: model/property/geometry contracts plus a panel and
# canvas smoke test. It intentionally links EditorProject without WHOLEARCHIVE
# so unrelated editor panels cannot block the suite while they are in flux.
add_executable(NullusUIDesignerTests
    UIDocumentDesignerModelTests.cpp
    UIDocumentDesignerCanvasTests.cpp
    UIDocumentDesignerPanelTests.cpp
    UIDocumentDesignerEditTests.cpp
    UIDocumentResourceReferenceTests.cpp
    UIDocumentResourceResolverTests.cpp
    UIFontAtlasFaceTests.cpp
    ReflectedEditHostTests.cpp
    ReflectedValueBackendTests.cpp
    "${CMAKE_SOURCE_DIR}/Tests/NullusTestMain.cpp"
)
nls_configure_test_target(NullusUIDesignerTests NO_WHOLE_ARCHIVE)
add_test(NAME NullusUIDesignerTests COMMAND NullusUIDesignerTests)
add_executable(NullusMaterialTests MaterialRuntimeTests.cpp MaterialAssetTests.cpp MaterialGpuTests.cpp MaterialSnapshotTests.cpp)
target_sources(NullusMaterialTests PRIVATE MaterialGraphTests.cpp)
target_sources(NullusMaterialTests PRIVATE MaterialCookerTests.cpp)
target_sources(NullusMaterialTests PRIVATE MaterialDocumentTests.cpp
    "${CMAKE_SOURCE_DIR}/Project/Editor/Materials/MaterialDocument.cpp"
    "${CMAKE_SOURCE_DIR}/Project/Editor/Undo/EditorUndoService.cpp"
    "${CMAKE_SOURCE_DIR}/Project/Editor/Undo/EditorUndoMutationContext.cpp"
    "${CMAKE_SOURCE_DIR}/Project/Editor/Undo/UndoBlobStore.cpp")
target_compile_features(NullusMaterialTests PRIVATE cxx_std_20)
target_include_directories(NullusMaterialTests PRIVATE "${CMAKE_SOURCE_DIR}/Runtime" "${CMAKE_SOURCE_DIR}/ThirdParty" "${CMAKE_SOURCE_DIR}/Project/Editor")
target_compile_definitions(NullusMaterialTests PRIVATE NLS_ROOT_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(NullusMaterialTests PRIVATE gtest_main NLS_Render NLS_MaterialCompiler)
set_target_properties(NullusMaterialTests PROPERTIES FOLDER Tests RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/material-tests")
if(WIN32)
    foreach(_material_dependency NLS_Render NLS_Core NLS_Base NLS_Math NLS_Platform ImGui glfw assimp)
        add_custom_command(TARGET NullusMaterialTests POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:${_material_dependency}> $<TARGET_FILE_DIR:NullusMaterialTests>)
    endforeach()
endif()
add_test(NAME NullusMaterialTests COMMAND NullusMaterialTests)
add_executable(NullusMaterialEditorTests MaterialEditorTests.cpp "${CMAKE_SOURCE_DIR}/Tests/NullusTestMain.cpp")
target_sources(NullusMaterialEditorTests PRIVATE MaterialVisibilityTests.cpp)
nls_configure_test_target(NullusMaterialEditorTests)
add_test(NAME NullusMaterialEditorTests COMMAND NullusMaterialEditorTests)
set_tests_properties(NullusMaterialEditorTests PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" LABELS "material;editor")
set_tests_properties(NullusMaterialTests PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" LABELS "material;behavior")
set_tests_properties(NullusUnitTests PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    LABELS behavior
)

# Packaging and container tests. These link only the build library plus the
# runtime modules it needs, so CI can run them without the editor or a GPU.
add_executable(NullusBuildTests
    BuildNpakTests.cpp
    BuildProfileTests.cpp
    BuildDependencyCollectorTests.cpp
    BuildPipelineTests.cpp
    PackageInspectionTests.cpp
    AssetCookerTests.cpp
    UnixDesktopPlatformTests.cpp
    "${CMAKE_SOURCE_DIR}/Tests/NullusTestMain.cpp")
target_compile_features(NullusBuildTests PRIVATE cxx_std_20)
target_include_directories(NullusBuildTests PRIVATE
    "${CMAKE_SOURCE_DIR}/Runtime"
    "${CMAKE_SOURCE_DIR}/Runtime/Core"
    "${CMAKE_SOURCE_DIR}/Runtime/Engine"
    "${CMAKE_SOURCE_DIR}/ThirdParty"
    "${CMAKE_SOURCE_DIR}")
target_compile_definitions(NullusBuildTests PRIVATE
    NLS_ROOT_DIR="${CMAKE_SOURCE_DIR}"
    NLS_BUILD_DIR="${CMAKE_BINARY_DIR}"
    NLS_BUILD_CONFIGURATION="$<CONFIG>")
target_link_libraries(NullusBuildTests PRIVATE gtest gtest_main NLS_Build)
set_target_properties(NullusBuildTests PROPERTIES
    FOLDER Tests
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/build-tests")
if(TARGET zstd)
    add_custom_command(TARGET NullusBuildTests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:NLS_Build> $<TARGET_FILE_DIR:NullusBuildTests>)
endif()
nls_copy_shared_runtime_to_target(NullusBuildTests)
add_test(NAME NullusBuildTests COMMAND NullusBuildTests)
set_tests_properties(NullusBuildTests PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    LABELS "build;packaging")

# Keep the scripting contract independently buildable. The aggregate target
# contains a VS2019-sensitive DX12 test; this focused target lets CI and local
# debugger work validate the C#/Lua runtime without compiling unrelated suites.
add_executable(NullusScriptRuntimeTests
    "${CMAKE_CURRENT_SOURCE_DIR}/ScriptRuntimeTests.cpp"
    # Keep the configuration generator independently testable without making
    # the focused scripting test depend on the full Editor executable.
    "${CMAKE_SOURCE_DIR}/Project/Editor/Debug/VisualStudioConfigurationGenerator.cpp"
    "${CMAKE_SOURCE_DIR}/Tests/NullusTestMain.cpp"
)
nls_configure_test_target(NullusScriptRuntimeTests)
add_test(NAME NullusScriptRuntimeTests COMMAND NullusScriptRuntimeTests)
set_tests_properties(NullusScriptRuntimeTests PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    LABELS scripting
)

add_executable(NullusVfxTests VfxRuntimeTests.cpp VfxSystemTests.cpp VfxTimingTests.cpp ImGuiNodeEditorIntegrationTests.cpp PhysicsWorldTests.cpp VfxShaderTests.cpp VfxGpuTests.cpp VfxSpriteTests.cpp VfxMeshTests.cpp VfxTypedTests.cpp)
target_compile_features(NullusVfxTests PRIVATE cxx_std_20)
target_compile_definitions(NullusVfxTests PRIVATE NLS_ROOT_DIR="${CMAKE_SOURCE_DIR}")
target_include_directories(NullusVfxTests PRIVATE
    "${CMAKE_SOURCE_DIR}/Runtime"
    "${CMAKE_SOURCE_DIR}/ThirdParty"
    "${CMAKE_SOURCE_DIR}/Project/Editor"
)
target_link_libraries(NullusVfxTests PRIVATE gtest_main NLS_VFXCompiler NLS_UI NLS_Physics)
set_target_properties(NullusVfxTests PROPERTIES
    FOLDER Tests
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
if(WIN32)
    foreach(_vfx_dependency NLS_VFX NLS_VFXCompiler NLS_Physics NLS_UI NLS_Render NLS_Core NLS_Platform NLS_Math NLS_Base ImGui glfw assimp)
        get_target_property(_vfx_dependency_type ${_vfx_dependency} TYPE)
        if(_vfx_dependency_type STREQUAL "SHARED_LIBRARY")
            add_custom_command(TARGET NullusVfxTests POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_FILE:${_vfx_dependency}> $<TARGET_FILE_DIR:NullusVfxTests>
            )
        endif()
    endforeach()
endif()
add_test(NAME NullusVfxTests COMMAND NullusVfxTests)
set_tests_properties(NullusVfxTests PROPERTIES LABELS "vfx;behavior" WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

add_executable(NullusVfxEditorTests VfxEditorTests.cpp VfxSceneTests.cpp "${CMAKE_SOURCE_DIR}/Tests/NullusTestMain.cpp")
nls_configure_test_target(NullusVfxEditorTests)
add_test(NAME NullusVfxEditorTests COMMAND NullusVfxEditorTests)
set_tests_properties(NullusVfxEditorTests PROPERTIES LABELS "vfx;editor" WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

add_executable(NullusVfxEditorPanelTests VfxEditorPanelTests.cpp VfxSimulationAuthoringDeviceTests.cpp "${CMAKE_SOURCE_DIR}/Tests/NullusTestMain.cpp")
target_sources(NullusVfxEditorPanelTests PRIVATE VfxPreviewLoadingTests.cpp)
target_sources(NullusVfxEditorPanelTests PRIVATE EditorGridRendererTests.cpp)
target_sources(NullusVfxEditorPanelTests PRIVATE
    CameraControllerInputTests.cpp SceneCameraFocusTests.cpp SceneViewCameraInteractionStateMachineTests.cpp
    DebugSceneLifecycleTests.cpp EditorPipelineStatePresetsTests.cpp)
nls_configure_test_target(NullusVfxEditorPanelTests)
set_target_properties(NullusVfxEditorPanelTests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/vfx-tests")
add_test(NAME NullusVfxEditorPanelTests COMMAND NullusVfxEditorPanelTests)
set_tests_properties(NullusVfxEditorPanelTests PROPERTIES LABELS "vfx;editor" WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

add_executable(NullusVfxAuthoringTests VfxAuthoringTests.cpp VfxLibraryTests.cpp VfxSimulationAuthoringTests.cpp VfxInspectorTests.cpp "${CMAKE_SOURCE_DIR}/Tests/NullusTestMain.cpp")
nls_configure_test_target(NullusVfxAuthoringTests)
set_target_properties(NullusVfxAuthoringTests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/vfx-tests")
add_test(NAME NullusVfxAuthoringTests COMMAND NullusVfxAuthoringTests)
set_tests_properties(NullusVfxAuthoringTests PROPERTIES LABELS "vfx;behavior" WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

add_executable(NullusInspectorPropertyTests InspectorCoreTests.cpp SerializedObjectCanonicalTests.cpp SharedPropertyEditorTests.cpp "${CMAKE_SOURCE_DIR}/Tests/NullusTestMain.cpp")
target_sources(NullusInspectorPropertyTests PRIVATE UnifiedPropertyPanelTests.cpp NativeSerializedStreamTests.cpp CollectionPropertyTests.cpp)
nls_configure_test_target(NullusInspectorPropertyTests)
add_test(NAME NullusInspectorPropertyTests COMMAND NullusInspectorPropertyTests)
set_tests_properties(NullusInspectorPropertyTests PROPERTIES LABELS "inspector;serialization" WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

# Independent controls deliberately link no editor, engine, reflection backend
# or Undo service. This also verifies the controls' public DLL boundary.
add_executable(NullusPropertyControlsTests PropertyControlsTests.cpp CollectionControlTests.cpp)
target_link_libraries(NullusPropertyControlsTests PRIVATE NLS_UI GTest::gtest_main)
set_target_properties(NullusPropertyControlsTests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/runtime")
add_test(NAME NullusPropertyControlsTests COMMAND NullusPropertyControlsTests)
set_tests_properties(NullusPropertyControlsTests PROPERTIES LABELS "ui;properties" WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
