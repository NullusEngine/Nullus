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

add_subdirectory(Unit)
