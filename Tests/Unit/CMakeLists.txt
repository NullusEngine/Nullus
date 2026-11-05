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

# Ordinary suites have one executable. Only tests requiring a different runtime
# environment or a deliberately smaller link boundary are compiled separately.
set(NULLUS_BUILD_TEST_FILES
    BuildNpakTests.cpp BuildProfileTests.cpp BuildDependencyCollectorTests.cpp
    BuildPipelineTests.cpp PackageInspectionTests.cpp AssetCookerTests.cpp
    UnixDesktopPlatformTests.cpp)
set(NULLUS_PROPERTY_CONTROL_TEST_FILES PropertyControlsTests.cpp CollectionControlTests.cpp)
set(NULLUS_GPU_TEST_FILES ${NULLUS_UNIT_TEST_FILES} ${NULLUS_RENDERING_TEST_FILES})
list(FILTER NULLUS_GPU_TEST_FILES INCLUDE REGEX "GpuTests\\.cpp$")

set(_isolated_test_files ${NULLUS_GPU_TEST_FILES})
foreach(_file IN LISTS NULLUS_BUILD_TEST_FILES NULLUS_PROPERTY_CONTROL_TEST_FILES)
    list(APPEND _isolated_test_files "${CMAKE_CURRENT_SOURCE_DIR}/${_file}")
endforeach()
list(REMOVE_ITEM NULLUS_UNIT_TEST_FILES ${_isolated_test_files})
list(REMOVE_ITEM NULLUS_RENDERING_TEST_FILES ${_isolated_test_files})

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${NULLUS_UNIT_TEST_FILES})
source_group(TREE "${CMAKE_SOURCE_DIR}/Tests/Rendering" PREFIX "Rendering Tests" FILES ${NULLUS_RENDERING_TEST_FILES})
source_group("Test Infrastructure" FILES "${CMAKE_SOURCE_DIR}/Tests/NullusTestMain.cpp")

add_executable(NullusUnitTests
    ${NULLUS_UNIT_TEST_FILES}
    ${NULLUS_RENDERING_TEST_FILES}
    "${CMAKE_SOURCE_DIR}/Tests/NullusTestMain.cpp")
nls_configure_test_target(NullusUnitTests)
target_link_libraries(NullusUnitTests PRIVATE
    NLS_AnimationCompiler NLS_Build NLS_Cook NLS_MaterialCompiler NLS_StaticMeshBuild)
add_test(NAME NullusUnitTests COMMAND NullusUnitTests)
set_tests_properties(NullusUnitTests PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" LABELS "unit;behavior")

# Keep GPU fixtures out of the ordinary process so they own their device/executor
# lifecycle. New *GpuTests.cpp files are routed here by the same source inventory.
add_executable(NullusGpuTests
    ${NULLUS_GPU_TEST_FILES}
    "${CMAKE_SOURCE_DIR}/Tests/NullusTestMain.cpp")
nls_configure_test_target(NullusGpuTests)
target_link_libraries(NullusGpuTests PRIVATE NLS_MaterialCompiler)
set_target_properties(NullusGpuTests PROPERTIES FOLDER "Tests/GPU")
add_test(NAME NullusGpuTests COMMAND NullusGpuTests)
set_tests_properties(NullusGpuTests PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" LABELS gpu RUN_SERIAL TRUE)

# Packaging and container tests. These link only the build library plus the
# runtime modules it needs, so CI can run them without the editor or a GPU.
add_executable(NullusBuildTests
    ${NULLUS_BUILD_TEST_FILES}
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
target_link_libraries(NullusBuildTests PRIVATE gtest NLS_Build)
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

# These controls must remain usable without Editor, Engine, reflection or Undo.
# A small executable verifies that public DLL boundary instead of hiding it behind
# the aggregate's transitive dependencies.
add_executable(NullusPropertyControlsTests ${NULLUS_PROPERTY_CONTROL_TEST_FILES})
target_link_libraries(NullusPropertyControlsTests PRIVATE NLS_UI GTest::gtest_main)
set_target_properties(NullusPropertyControlsTests PROPERTIES
    FOLDER "Tests/Boundaries" RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/runtime")
add_test(NAME NullusPropertyControlsTests COMMAND NullusPropertyControlsTests)
set_tests_properties(NullusPropertyControlsTests PROPERTIES
    LABELS "ui;properties;boundary" WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

if(NLS_ENABLE_CSHARP_SCRIPTING)
    if(WIN32)
        set(_play_dotnet_command "${CMAKE_SOURCE_DIR}/Tools/RunRepositoryDotnet.cmd" "${NLS_DOTNET_EXECUTABLE}")
    else()
        set(_play_dotnet_command "${CMAKE_COMMAND}" -E env "DOTNET_ROOT=${NLS_DOTNET_ROOT}" "${NLS_DOTNET_EXECUTABLE}")
    endif()
    add_custom_target(NullusPlayManagedFixture
        COMMAND ${_play_dotnet_command} build "${CMAKE_SOURCE_DIR}/Tests/Fixtures/PlayScripts/EngineFixture.csproj"
            -c $<CONFIG> -o "${CMAKE_BINARY_DIR}/play-tests-managed/$<CONFIG>"
            "-p:NullusManagedOutput=${CMAKE_BINARY_DIR}/Managed/$<CONFIG>"
            "-p:BaseIntermediateOutputPath=${CMAKE_BINARY_DIR}/play-tests-managed/engine-obj/$<CONFIG>/"
            --ignore-failed-sources
        COMMAND ${_play_dotnet_command} build "${CMAKE_SOURCE_DIR}/Tests/Fixtures/PlayScripts/PlayFixture.csproj"
            -c $<CONFIG> -o "${CMAKE_BINARY_DIR}/play-tests-managed/$<CONFIG>"
            "-p:NullusManagedOutput=${CMAKE_BINARY_DIR}/Managed/$<CONFIG>"
            "-p:NullusFixtureOutput=${CMAKE_BINARY_DIR}/play-tests-managed/$<CONFIG>"
            "-p:BaseIntermediateOutputPath=${CMAKE_BINARY_DIR}/play-tests-managed/obj/$<CONFIG>/"
            --ignore-failed-sources
        VERBATIM)
    add_dependencies(NullusPlayManagedFixture NLS_Scripting)
    set_target_properties(NullusPlayManagedFixture PROPERTIES FOLDER "Tests/Fixtures/Managed")
    add_dependencies(NullusUnitTests NullusPlayManagedFixture)
endif()
