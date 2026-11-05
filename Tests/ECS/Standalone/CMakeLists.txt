cmake_minimum_required(VERSION 3.20)
project(NullusECSHeadless LANGUAGES CXX)
include(CTest)
get_filename_component(NLS_ROOT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
option(NLS_ECS_ASAN "Enable AddressSanitizer for the ECS headless validation build" OFF)
if(MSVC)
    # Embed debug information, avoiding dependence on a shared compiler PDB.
    set(CMAKE_CXX_FLAGS_DEBUG "/Od /Ob0 /Z7 /RTC1")
    add_compile_options(/EHsc /utf-8 /W4)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
    if(NLS_ECS_ASAN)
        set(CMAKE_CXX_FLAGS_DEBUG "/Od /Ob0 /Z7 /fsanitize=address")
        add_link_options(/INCREMENTAL:NO)
    endif()
elseif(NLS_ECS_ASAN)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
add_subdirectory("${NLS_ROOT_DIR}/ThirdParty/assimp/contrib/googletest" "${CMAKE_BINARY_DIR}/googletest" EXCLUDE_FROM_ALL)
find_package(Threads REQUIRED)
# These are the real Base implementations, compiled directly to avoid pulling
# reflection, windowing, GPU, editor or managed build dependencies into core QA.
add_library(NLS_ECSBase STATIC
    "${NLS_ROOT_DIR}/Runtime/Base/TaskGraph/TaskGraph.cpp"
    "${NLS_ROOT_DIR}/Runtime/Base/TaskGraph/TaskEvent.cpp"
    "${NLS_ROOT_DIR}/Runtime/Base/Jobs/JobSystem.cpp"
    "${NLS_ROOT_DIR}/Runtime/Base/Jobs/JobQueue.cpp"
    "${NLS_ROOT_DIR}/Runtime/Base/Jobs/BackgroundJobQueue.cpp"
    "${NLS_ROOT_DIR}/Runtime/Base/Jobs/JobBatchDispatcher.cpp"
    "${NLS_ROOT_DIR}/Runtime/Base/Jobs/JobDiagnostics.cpp"
    "${NLS_ROOT_DIR}/Runtime/Base/Jobs/JobRange.cpp"
    "${NLS_ROOT_DIR}/Runtime/Base/Jobs/JobSafety.cpp"
    "${NLS_ROOT_DIR}/Runtime/Base/Profiling/Profiler.cpp"
    "${NLS_ROOT_DIR}/Runtime/Base/Profiling/TracyProfiler.cpp")
target_include_directories(NLS_ECSBase PUBLIC "${NLS_ROOT_DIR}/Runtime" "${NLS_ROOT_DIR}/Runtime/Base" "${NLS_ROOT_DIR}/ThirdParty")
target_link_libraries(NLS_ECSBase PUBLIC Threads::Threads)
file(GLOB_RECURSE ECS_SOURCES CONFIGURE_DEPENDS "${NLS_ROOT_DIR}/Runtime/ECS/*.cpp")
add_library(NLS_ECS STATIC ${ECS_SOURCES})
target_include_directories(NLS_ECS PUBLIC "${NLS_ROOT_DIR}/Runtime")
target_link_libraries(NLS_ECS PUBLIC NLS_ECSBase)
add_executable(NullusECSTests "../CoreTests.cpp" "../QueryTests.cpp" "../CommandTests.cpp" "../SystemTests.cpp" "../ParallelTests.cpp" "../SerializationTests.cpp" "../StreamingTests.cpp")
target_sources(NullusECSTests PRIVATE "${NLS_ROOT_DIR}/Tests/Unit/TaskGraphTests.cpp")
target_link_libraries(NullusECSTests PRIVATE gtest_main NLS_ECS)
add_test(NAME NullusECSTests COMMAND NullusECSTests)
set_tests_properties(NullusECSTests PROPERTIES TIMEOUT 120)
# Test the engine's native ECS hierarchy against the actual Math implementation
# without pulling Scene/GPU/editor dependencies into this headless project.
add_library(NLS_ECSTransformSupport STATIC
    "${NLS_ROOT_DIR}/Runtime/Engine/ECS/TransformHierarchy.cpp"
    "${NLS_ROOT_DIR}/Runtime/Math/Matrix3.cpp"
    "${NLS_ROOT_DIR}/Runtime/Math/Matrix4.cpp"
    "${NLS_ROOT_DIR}/Runtime/Math/Quaternion.cpp"
    "${NLS_ROOT_DIR}/Runtime/Math/Vector2.cpp"
    "${NLS_ROOT_DIR}/Runtime/Math/Vector3.cpp"
    "${NLS_ROOT_DIR}/Runtime/Math/Vector4.cpp"
    "${NLS_ROOT_DIR}/Runtime/Math/Transform.cpp")
target_link_libraries(NLS_ECSTransformSupport PUBLIC NLS_ECS)
add_executable(NullusECSTransformTests "../TransformTests.cpp")
target_link_libraries(NullusECSTransformTests PRIVATE gtest_main NLS_ECSTransformSupport)
add_test(NAME NullusECSTransformTests COMMAND NullusECSTransformTests)
set_tests_properties(NullusECSTransformTests PROPERTIES TIMEOUT 120)
add_executable(NullusECSBasics "${NLS_ROOT_DIR}/Samples/ECS/Basics.cpp")
target_link_libraries(NullusECSBasics PRIVATE NLS_ECS)
add_executable(NullusECSMassSimulation "${NLS_ROOT_DIR}/Samples/ECS/MassSimulation.cpp")
target_link_libraries(NullusECSMassSimulation PRIVATE NLS_ECS)
add_executable(NullusECSStreamingExample "${NLS_ROOT_DIR}/Samples/ECS/SceneStreaming.cpp")
target_link_libraries(NullusECSStreamingExample PRIVATE NLS_ECS)
add_executable(NullusECSBenchmark "../Benchmark.cpp")
target_link_libraries(NullusECSBenchmark PRIVATE NLS_ECS)
add_executable(NullusECSArchiveBenchmark "../ArchiveBenchmark.cpp")
target_link_libraries(NullusECSArchiveBenchmark PRIVATE NLS_ECS)
