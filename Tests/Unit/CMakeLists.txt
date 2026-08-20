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
set_tests_properties(NullusUnitTests PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    LABELS behavior
)

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
