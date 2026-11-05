cmake_minimum_required(VERSION 3.20)
project(NullusTaskSystemValidation LANGUAGES C CXX)

# Standalone scheduler validation: no Editor, graphics SDK, or generated engine
# metadata is needed. The production sources and existing GoogleTests are used.
get_filename_component(NLS_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Threads REQUIRED)
set(NLS_TASK_SOURCE_BASE "${NLS_ROOT}/Runtime/Base" CACHE PATH
    "Base source directory; override with a preserved snapshot for comparison")
option(NLS_TASK_SANITIZE_ADDRESS "Enable AddressSanitizer" OFF)
option(NLS_TASK_SANITIZE_THREAD "Enable ThreadSanitizer (Clang/GCC on supported hosts)" OFF)
if(NLS_TASK_SANITIZE_ADDRESS AND NLS_TASK_SANITIZE_THREAD)
    message(FATAL_ERROR "AddressSanitizer and ThreadSanitizer require separate builds")
endif()
if(NLS_TASK_SANITIZE_ADDRESS)
    if(MSVC)
        add_compile_options(/fsanitize=address /Z7)
        add_link_options(/INCREMENTAL:NO /DEBUG:FULL)
    else()
        add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address)
    endif()
endif()
if(NLS_TASK_SANITIZE_THREAD)
    if(MSVC OR WIN32)
        message(FATAL_ERROR "ThreadSanitizer requires a supported non-Windows Clang/GCC host")
    endif()
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
endif()

set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
add_subdirectory("${NLS_ROOT}/ThirdParty/assimp/contrib/googletest"
    "${CMAKE_BINARY_DIR}/googletest" EXCLUDE_FROM_ALL)

file(GLOB NLS_TASK_SOURCES CONFIGURE_DEPENDS
    "${NLS_TASK_SOURCE_BASE}/Jobs/*.cpp"
    "${NLS_TASK_SOURCE_BASE}/TaskGraph/*.cpp")
add_library(NullusTaskRuntime STATIC ${NLS_TASK_SOURCES}
    "${NLS_ROOT}/Runtime/Base/Profiling/Profiler.cpp"
    "${NLS_ROOT}/Runtime/Base/Profiling/ProfilerScope.cpp"
    "${NLS_ROOT}/Runtime/Base/Profiling/TracyProfiler.cpp")
target_include_directories(NullusTaskRuntime PUBLIC
    "${NLS_TASK_SOURCE_BASE}" "${NLS_ROOT}/Runtime/Base"
    "${NLS_ROOT}/Runtime" "${NLS_ROOT}")
target_compile_definitions(NullusTaskRuntime PUBLIC
    NLS_ENABLE_TEST_HOOKS=1 NLS_ENABLE_PROFILING=1 NLS_ROOT_DIR="${NLS_ROOT}")
target_link_libraries(NullusTaskRuntime PUBLIC Threads::Threads)
if(MSVC)
    target_compile_options(NullusTaskRuntime PUBLIC /EHsc /utf-8 /Z7)
endif()

add_executable(NullusTaskSystemTests
    "${NLS_ROOT}/Tests/Unit/TaskGraphTests.cpp"
    "${NLS_ROOT}/Tests/Unit/UnifiedTaskSystemTests.cpp"
    "${NLS_ROOT}/Tests/Unit/GlobalTaskGraphLifecycleTests.cpp"
    "${NLS_ROOT}/Tests/Unit/JobsUnifiedSchedulerTests.cpp"
    "${NLS_ROOT}/Tests/Unit/ExecutionThreadBudgetTests.cpp"
    "${NLS_ROOT}/Tests/Unit/JobSystemSchedulerTests.cpp"
    "${NLS_ROOT}/Tests/Unit/JobSystemParallelTests.cpp"
    "${NLS_ROOT}/Tests/Unit/JobSystemBackgroundTests.cpp"
    "${NLS_ROOT}/Tests/Unit/JobSystemDiagnosticsTests.cpp"
    "${NLS_ROOT}/Tests/Unit/JobSystemBindingsTests.cpp"
    "${NLS_ROOT}/Tests/Unit/JobSystemBindingCAbiCompileTest.c")
target_link_libraries(NullusTaskSystemTests PRIVATE NullusTaskRuntime gtest_main)

add_executable(NullusTaskBenchmark TaskBenchmark.cpp)
target_link_libraries(NullusTaskBenchmark PRIVATE NullusTaskRuntime)
# Exercise real named-thread owners without a Driver or graphics SDK. Keep the
# production command pipe, fence and logging implementations in this target.
add_executable(NullusRenderingThreadLifecycleTests
    "${NLS_ROOT}/Tests/Unit/RenderingThreadSystemLifecycleTests.cpp"
    "${NLS_ROOT}/Runtime/Rendering/Context/RenderingThreadSystem.cpp"
    "${NLS_ROOT}/Runtime/Rendering/Context/RenderCommandPipe.cpp"
    "${NLS_ROOT}/Runtime/Rendering/Context/RenderCommandFence.cpp"
    "${NLS_ROOT}/Runtime/Rendering/Settings/RhiThreadMode.cpp"
    "${NLS_ROOT}/Runtime/Base/Debug/Logger.cpp"
    "${NLS_ROOT}/Runtime/Base/Debug/ConsoleHandler.cpp"
    "${NLS_ROOT}/Runtime/Base/Debug/FileHandler.cpp"
    "${NLS_ROOT}/Runtime/Base/Debug/HistoryHandler.cpp"
    "${NLS_ROOT}/Runtime/Base/Debug/StackTrace.cpp"
    "${NLS_ROOT}/Runtime/Base/Time/Date.cpp")
target_include_directories(NullusRenderingThreadLifecycleTests PRIVATE
    "${NLS_ROOT}/Runtime/Rendering")
target_link_libraries(NullusRenderingThreadLifecycleTests PRIVATE NullusTaskRuntime gtest_main)
if(WIN32)
    target_sources(NullusRenderingThreadLifecycleTests PRIVATE
        "${NLS_ROOT}/ThirdParty/NullusDbgHelpLock.cpp")
    target_link_libraries(NullusRenderingThreadLifecycleTests PRIVATE dbghelp)
endif()

enable_testing()
add_test(NAME NullusTaskSystemTests COMMAND NullusTaskSystemTests)
set_tests_properties(NullusTaskSystemTests PROPERTIES TIMEOUT 180
    WORKING_DIRECTORY "${NLS_ROOT}" LABELS "tasks;headless")
add_test(NAME NullusRenderingThreadLifecycleTests COMMAND NullusRenderingThreadLifecycleTests)
set_tests_properties(NullusRenderingThreadLifecycleTests PROPERTIES TIMEOUT 60
    WORKING_DIRECTORY "${NLS_ROOT}" LABELS "tasks;rendering;headless")
