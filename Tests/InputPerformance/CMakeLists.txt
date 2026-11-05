cmake_minimum_required(VERSION 3.22)

# Can also be configured alone: no Editor, graphics SDK, reflection or downloads.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_policy(SET CMP0091 NEW)
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
    project(NullusInputPerformance LANGUAGES CXX)
    include(CTest)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

get_filename_component(NLS_INPUT_BENCH_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(_input_dir "${NLS_INPUT_BENCH_ROOT}/Runtime/Input")
# Compile the production sources into this executable's static dependency. An
# executable's replacement operator new cannot count allocations inside a DLL.
# No copied implementation, allocator changes, or test hooks enter NLS_Input.
add_library(NullusInputBenchCore STATIC
    "${_input_dir}/InputSystem.cpp"
    "${_input_dir}/InputAction.cpp"
    "${_input_dir}/InputUser.cpp"
    "${_input_dir}/InputDomain.cpp"
    "${_input_dir}/InputRebinding.cpp"
    "${_input_dir}/InputReplay.cpp"
    "${NLS_INPUT_BENCH_ROOT}/Runtime/Base/Guid.cpp"
    "${NLS_INPUT_BENCH_ROOT}/Runtime/Math/Vector2.cpp"
    "${NLS_INPUT_BENCH_ROOT}/Runtime/Math/Vector3.cpp"
    "${NLS_INPUT_BENCH_ROOT}/Runtime/Math/Vector4.cpp"
)
target_include_directories(NullusInputBenchCore PUBLIC
    "${NLS_INPUT_BENCH_ROOT}/Runtime"
    "${NLS_INPUT_BENCH_ROOT}/Runtime/Base"
    "${NLS_INPUT_BENCH_ROOT}/ThirdParty")
target_compile_features(NullusInputBenchCore PUBLIC cxx_std_20)
# Also support a shared-library parent build, without importing these symbols
# from a different CRT/DLL and invalidating the allocation counts.
target_compile_definitions(NullusInputBenchCore PUBLIC NLS_INPUT_EXPORT NLS_BASE_EXPORT NLS_MATH_EXPORT)
set_target_properties(NullusInputBenchCore PROPERTIES DISABLE_PRECOMPILE_HEADERS ON FOLDER Tests)

add_executable(NullusInputBenchmark InputBenchmark.cpp HeapAllocationCounter.cpp)
target_link_libraries(NullusInputBenchmark PRIVATE NullusInputBenchCore)
target_compile_definitions(NullusInputBenchmark PRIVATE NLS_INPUT_BENCH_CONFIG="$<CONFIG>")
set_target_properties(NullusInputBenchmark PROPERTIES DISABLE_PRECOMPILE_HEADERS ON FOLDER Tests)
if(MSVC)
    foreach(_target NullusInputBenchCore NullusInputBenchmark)
        target_compile_options(${_target} PRIVATE /utf-8 /bigobj /EHsc /Z7)
    endforeach()
endif()
add_test(NAME NullusInputAllocationChecks COMMAND NullusInputBenchmark --samples 16 --batch 16
    CONFIGURATIONS Release RelWithDebInfo)
set_tests_properties(NullusInputAllocationChecks PROPERTIES LABELS "input;performance;allocation;headless")

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR AND BUILD_TESTING)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    add_subdirectory("${NLS_INPUT_BENCH_ROOT}/ThirdParty/assimp/contrib/googletest"
        "${CMAKE_BINARY_DIR}/googletest" EXCLUDE_FROM_ALL)
    # The existing behavioral tests run against exactly the sources measured
    # above, including serialization, replay, callback safety and fixed ticks.
    add_executable(NullusInputReleaseTests
        "${NLS_INPUT_BENCH_ROOT}/Tests/Unit/InputSystemTests.cpp"
        "${NLS_INPUT_BENCH_ROOT}/Tests/Unit/InputActionTests.cpp"
        "${NLS_INPUT_BENCH_ROOT}/Tests/Unit/InputSerializationTests.cpp"
        "${NLS_INPUT_BENCH_ROOT}/Runtime/Core/Inputs/InputSerialization.cpp")
    target_link_libraries(NullusInputReleaseTests PRIVATE NullusInputBenchCore gtest_main)
    if(MSVC)
        target_compile_options(NullusInputReleaseTests PRIVATE /utf-8 /bigobj /EHsc /Z7)
    endif()
    add_test(NAME NullusInputReleaseTests COMMAND NullusInputReleaseTests)
    set_tests_properties(NullusInputReleaseTests PROPERTIES LABELS "input;headless"
        WORKING_DIRECTORY "${NLS_INPUT_BENCH_ROOT}")
endif()
