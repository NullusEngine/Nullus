file(GLOB_RECURSE NULLUS_PERFORMANCE_TEST_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.inl"
)

# Exclude DX12 tests on non-Windows platforms
if(NOT WIN32)
    list(FILTER NULLUS_PERFORMANCE_TEST_FILES EXCLUDE REGEX "[/\\\\]DX12.*\\.cpp$")
endif()

set(NULLUS_PERFORMANCE_TEST_C_FILES ${NULLUS_PERFORMANCE_TEST_FILES})
list(FILTER NULLUS_PERFORMANCE_TEST_C_FILES INCLUDE REGEX "\\.c$")
if(NULLUS_PERFORMANCE_TEST_C_FILES)
    set_source_files_properties(${NULLUS_PERFORMANCE_TEST_C_FILES} PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
endif()

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${NULLUS_PERFORMANCE_TEST_FILES})
source_group("Test Infrastructure" FILES "${CMAKE_SOURCE_DIR}/Tests/NullusTestMain.cpp")

add_executable(NullusPerformanceTests
    ${NULLUS_PERFORMANCE_TEST_FILES}
    "${CMAKE_SOURCE_DIR}/Tests/NullusTestMain.cpp"
)

nls_configure_test_target(NullusPerformanceTests)

add_test(NAME NullusPerformanceTests COMMAND NullusPerformanceTests)
set_tests_properties(NullusPerformanceTests PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    LABELS performance
)
