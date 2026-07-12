cmake_minimum_required(VERSION 3.18)

project(NullusTests LANGUAGES CXX)

include(CTest)
enable_testing()

if(NOT TARGET gtest)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(BUILD_GMOCK ON CACHE BOOL "" FORCE)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    add_subdirectory(
        "${CMAKE_SOURCE_DIR}/ThirdParty/assimp/contrib/googletest"
        "${CMAKE_BINARY_DIR}/ThirdParty/googletest"
        EXCLUDE_FROM_ALL
    )

    set_target_properties(gtest PROPERTIES FOLDER ThirdParty/googletest)
    set_target_properties(gtest_main PROPERTIES FOLDER ThirdParty/googletest)
    if(TARGET gmock)
        set_target_properties(gmock PROPERTIES FOLDER ThirdParty/googletest)
    endif()
    if(TARGET gmock_main)
        set_target_properties(gmock_main PROPERTIES FOLDER ThirdParty/googletest)
    endif()

    foreach(_gtest_target gtest gtest_main gmock gmock_main)
        if(TARGET ${_gtest_target})
            get_target_property(_gtest_compile_flags ${_gtest_target} COMPILE_FLAGS)
            if(_gtest_compile_flags)
                string(REPLACE "-utf-8" "/source-charset:utf-8 /execution-charset:utf-8" _gtest_compile_flags "${_gtest_compile_flags}")
                set_target_properties(${_gtest_target} PROPERTIES COMPILE_FLAGS "${_gtest_compile_flags}")
            endif()
        endif()
    endforeach()
endif()

function(nls_configure_test_target target_name)
    target_compile_features(${target_name} PRIVATE cxx_std_20)

    target_include_directories(${target_name} PRIVATE
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

    target_compile_definitions(${target_name} PRIVATE
        NLS_ROOT_DIR="${CMAKE_SOURCE_DIR}"
        NLS_BUILD_DIR="${CMAKE_BINARY_DIR}"
        $<$<BOOL:${NLS_ENABLE_TEST_HOOKS}>:NLS_ENABLE_TEST_HOOKS=1>
        $<$<BOOL:${NLS_ENABLE_TIMELINE_PROFILER}>:NLS_ENABLE_TIMELINE_PROFILER=1>
        WITH_PROFILING=$<AND:$<BOOL:${WIN32}>,$<BOOL:${NLS_ENABLE_TIMELINE_PROFILER}>>
    )

    target_link_libraries(${target_name} PRIVATE
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

    nls_target_precompile_headers(${target_name} "${CMAKE_SOURCE_DIR}/Tests/NullusTestsPch.h")

    if(APPLE)
        target_link_libraries(${target_name} PRIVATE "-framework CoreServices")
    endif()

    if(WIN32)
        target_link_libraries(${target_name} PRIVATE Comctl32)
    endif()

    if(TARGET EditorProject_MetaGenerated)
        add_dependencies(${target_name} EditorProject_MetaGenerated)
    endif()

    set_target_properties(${target_name} PROPERTIES
        FOLDER Tests
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
    nls_copy_shared_runtime_to_target(${target_name})
    nls_copy_editor_resources_to_target(${target_name})
endfunction()

if(NLS_BUILD_TESTS)
    add_test(
        NAME DeployRuntimeTests
        COMMAND ${CMAKE_COMMAND}
            "-DNLS_ROOT_DIR=${CMAKE_SOURCE_DIR}"
            -P "${CMAKE_SOURCE_DIR}/Tests/CMake/DeployRuntimeTests.cmake")
    add_subdirectory(Unit)
endif()

if(NLS_BUILD_PERFORMANCE_TESTS)
    add_subdirectory(Performance)
endif()
