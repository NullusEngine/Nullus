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

add_executable(NullusUnitTests
    ${NULLUS_UNIT_TEST_FILES}
    ${NULLUS_RENDERING_TEST_FILES}
)

target_compile_features(NullusUnitTests PRIVATE cxx_std_20)

target_include_directories(NullusUnitTests PRIVATE
    ${CMAKE_SOURCE_DIR}/Runtime
    ${CMAKE_SOURCE_DIR}/Runtime/Base
    ${CMAKE_SOURCE_DIR}/Runtime/Base/Reflection
    ${CMAKE_SOURCE_DIR}/Runtime/Core
    ${CMAKE_SOURCE_DIR}/Runtime/Engine
    ${CMAKE_SOURCE_DIR}/Runtime/Math
    ${CMAKE_SOURCE_DIR}/Runtime/Rendering
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Project/Editor/Gen
    ${CMAKE_SOURCE_DIR}/Project/Editor
    ${CMAKE_SOURCE_DIR}/Project/Launcher
    ${CMAKE_SOURCE_DIR}/Project/Game
)

target_compile_definitions(NullusUnitTests PRIVATE
    NLS_ROOT_DIR="${CMAKE_SOURCE_DIR}"
    NLS_BUILD_DIR="${CMAKE_BINARY_DIR}"
    $<$<BOOL:${NLS_ENABLE_TEST_HOOKS}>:NLS_ENABLE_TEST_HOOKS=1>
    WITH_PROFILING=$<AND:$<BOOL:${WIN32}>,$<BOOL:${NLS_ENABLE_TIMELINE_PROFILER}>>
)

target_link_libraries(NullusUnitTests PRIVATE
    gtest
    NLS_Base
    NLS_Core
    NLS_Math
    NLS_Render
    NLS_UI
    NLS_Engine
    EditorProject
    GameProject
    LauncherProject
    DirectXTex::DirectXTex
)

nls_target_precompile_headers(NullusUnitTests "${CMAKE_SOURCE_DIR}/Tests/NullusTestsPch.h")

if(APPLE)
    target_link_libraries(NullusUnitTests PRIVATE "-framework CoreServices")
endif()

if(WIN32)
    target_link_libraries(NullusUnitTests PRIVATE Comctl32)
endif()

if(TARGET EditorProject_MetaGenerated)
    add_dependencies(NullusUnitTests EditorProject_MetaGenerated)
endif()

set_target_properties(NullusUnitTests PROPERTIES
    FOLDER Tests
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
nls_copy_shared_runtime_to_target(NullusUnitTests)

add_test(NAME NullusUnitTests COMMAND NullusUnitTests)
set_tests_properties(NullusUnitTests PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
