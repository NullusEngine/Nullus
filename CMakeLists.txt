# CMake 最低版本号要求
# ##############################################################################
cmake_minimum_required(VERSION 3.18)

# 兼容第三方库
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

# 设置CMake安装目录
# ##############################################################################
set(CMAKE_INSTALL_PREFIX ${CMAKE_CURRENT_SOURCE_DIR}/Install CACHE PATH "set install path!" FORCE)

# 在生成的项目文件中使用文件夹。开启文件夹组织项
# ##############################################################################
set_property(GLOBAL PROPERTY USE_FOLDERS ON)
set(CMAKE_SUPPRESS_REGENERATION ON)

# 设置项目名称
# ##############################################################################
project(Nullus VERSION 1.0.0 LANGUAGES C CXX)

if(APPLE)
    enable_language(OBJC)
    enable_language(OBJCXX)
endif()

include(CTest)
enable_testing()

set(NLS_VULKAN_SDK_HINTS "")
if(DEFINED ENV{VULKAN_SDK} AND NOT "$ENV{VULKAN_SDK}" STREQUAL "")
    list(APPEND NLS_VULKAN_SDK_HINTS "$ENV{VULKAN_SDK}")
endif()
if(DEFINED ENV{VK_SDK_PATH} AND NOT "$ENV{VK_SDK_PATH}" STREQUAL "")
    list(APPEND NLS_VULKAN_SDK_HINTS "$ENV{VK_SDK_PATH}")
endif()
if(WIN32 AND EXISTS "C:/VulkanSDK/latest")
    list(APPEND NLS_VULKAN_SDK_HINTS "C:/VulkanSDK/latest")
endif()
list(REMOVE_DUPLICATES NLS_VULKAN_SDK_HINTS)
foreach(_nls_vulkan_sdk IN LISTS NLS_VULKAN_SDK_HINTS)
    if(EXISTS "${_nls_vulkan_sdk}")
        set(Vulkan_ROOT "${_nls_vulkan_sdk}" CACHE PATH "Path to the Vulkan SDK root" FORCE)
        if(EXISTS "${_nls_vulkan_sdk}/Include/vulkan/vulkan.h")
            set(Vulkan_INCLUDE_DIR "${_nls_vulkan_sdk}/Include" CACHE PATH "Vulkan include directory" FORCE)
        endif()
        if(EXISTS "${_nls_vulkan_sdk}/Lib/vulkan-1.lib")
            set(Vulkan_LIBRARY "${_nls_vulkan_sdk}/Lib/vulkan-1.lib" CACHE FILEPATH "Vulkan loader library" FORCE)
        endif()
        if(EXISTS "${_nls_vulkan_sdk}/Bin/glslc.exe")
            set(Vulkan_GLSLC_EXECUTABLE "${_nls_vulkan_sdk}/Bin/glslc.exe" CACHE FILEPATH "Vulkan GLSLC executable" FORCE)
        endif()
        if(EXISTS "${_nls_vulkan_sdk}/Bin/glslangValidator.exe")
            set(Vulkan_GLSLANG_VALIDATOR_EXECUTABLE "${_nls_vulkan_sdk}/Bin/glslangValidator.exe" CACHE FILEPATH "Vulkan glslang validator executable" FORCE)
        endif()
        list(PREPEND CMAKE_PREFIX_PATH "${_nls_vulkan_sdk}")
        break()
    endif()
endforeach()
find_package(Vulkan QUIET)
set(NLS_HAS_VULKAN OFF)
if(Vulkan_FOUND)
    set(NLS_HAS_VULKAN ON)
endif()

# 设置c++版本要求
# ##############################################################################
option(NLS_ENABLE_MSVC_MULTIPROCESSOR_COMPILATION "Enable MSVC /MP target-internal parallel compilation" ON)
set(NLS_MSVC_MP_JOBS "" CACHE STRING "Optional MSVC /MP process count; empty uses compiler default")

set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
if(MSVC)
    set(_nls_msvc_compile_options /FS /bigobj /source-charset:utf-8 /execution-charset:utf-8)
    if(NLS_ENABLE_MSVC_MULTIPROCESSOR_COMPILATION)
        if(NLS_MSVC_MP_JOBS)
            if(NOT NLS_MSVC_MP_JOBS MATCHES "^[1-9][0-9]*$")
                message(FATAL_ERROR "NLS_MSVC_MP_JOBS must be empty or a positive integer.")
            endif()
            list(APPEND _nls_msvc_compile_options "/MP${NLS_MSVC_MP_JOBS}")
        else()
            list(APPEND _nls_msvc_compile_options /MP)
        endif()
    endif()
    add_compile_options(${_nls_msvc_compile_options})
endif()

# 确定编译目标为32位还是64位
# ##############################################################################
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(NLS_PLATFORM_X64 true)
else()
    set(NLS_PLATFORM_X64 false)
endif()

# Check for other platforms if needed
if(APPLE)
    set(NLS_PLATFORM_APPLE true)
    set(TARGET_PLATFORM_NAME "MacOS")
elseif(UNIX)
    set(NLS_PLATFORM_UNIX true)
    set(TARGET_PLATFORM_NAME "Linux")
elseif(WIN32)
    set(NLS_PLATFORM_WIN32 true)
endif()

if(NLS_PLATFORM_X64)
    if(WIN32)
        set(TARGET_PLATFORM_NAME "Win64")
    endif()
else()
    if(WIN32)
        set(TARGET_PLATFORM_NAME "Win32")
    endif()
endif()

# 可选的配置项
# ##############################################################################
option(NLS_USE_SHARED_LIBRARY "Use Shared Library : OFF|ON (default is ON)" ON)
option(NLS_ENABLE_PROFILING "Enable Nullus shared profiling instrumentation" ON)
option(NLS_ENABLE_TRACY "Enable Tracy profiler destination" OFF)
option(NLS_ENABLE_TIMELINE_PROFILER "Enable TimelineProfiler editor destination" OFF)
option(NLS_BUILD_TESTS "Build Nullus test targets" OFF)
option(NLS_ENABLE_TEST_HOOKS "Expose test-only runtime hooks for NullusUnitTests" ${NLS_BUILD_TESTS})
option(NLS_BUILD_TRACY_PROFILER "Build Tracy GUI profiler tool" ${NLS_ENABLE_TRACY})
option(NLS_ENABLE_PCH "Enable precompiled headers for supported C++ targets" ON)
option(NLS_ENABLE_CXX_MODULES "Enable experimental C++20 modules pilot targets" OFF)

set(NLS_RENDER_API_LIST "OpenGL" "Vulkan" "DX12" "Metal")
if(WIN32)
    set(_nls_default_render_api "DX12")
elseif(APPLE)
    set(_nls_default_render_api "Metal")
elseif(UNIX)
    set(_nls_default_render_api "Vulkan")
else()
    set(_nls_default_render_api "OpenGL")
endif()
set(NLS_RENDER_API "${_nls_default_render_api}" CACHE STRING "Choose render api")
set_property(CACHE NLS_RENDER_API PROPERTY STRINGS ${NLS_RENDER_API_LIST})

if(NLS_USE_SHARED_LIBRARY)
    add_definitions(-DNLS_SHARED_LIB)
    set(NLS_LIBRARY_TYPE "Shared")
else()
    set(NLS_LIBRARY_TYPE "Static")
endif()

# 设置目录
# ##############################################################################
set(NLS_ROOT_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(NLS_ThirdParty_DIR "${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty")

# App Path
if(MSVC)
    set(NLS_APP_PATH "${NLS_ROOT_DIR}/App")
    if(NLS_USE_SHARED_LIBRARY)
    set(NLS_APP_OUTPUT_PATH "${NLS_APP_PATH}/${TARGET_PLATFORM_NAME}_$<CONFIGURATION>_Runtime_Shared")
    else()
    set(NLS_APP_OUTPUT_PATH "${NLS_APP_PATH}/${TARGET_PLATFORM_NAME}_$<CONFIGURATION>_Runtime_Static")
    endif()
else()
    set(NLS_APP_PATH "${NLS_ROOT_DIR}/App")
    if(NLS_USE_SHARED_LIBRARY)
        set(NLS_APP_OUTPUT_PATH "${NLS_APP_PATH}/${TARGET_PLATFORM_NAME}_${CMAKE_BUILD_TYPE}_Shared")
    else()
        set(NLS_APP_OUTPUT_PATH "${NLS_APP_PATH}/${TARGET_PLATFORM_NAME}_${CMAKE_BUILD_TYPE}_Static")
    endif()
endif()

function(nls_copy_shared_runtime_to_target TARGET_NAME)
    if(WIN32 AND NLS_USE_SHARED_LIBRARY)
        add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${NLS_APP_OUTPUT_PATH}"
                "$<TARGET_FILE_DIR:${TARGET_NAME}>"
            VERBATIM
            COMMENT "Copying Nullus runtime DLLs for ${TARGET_NAME}"
        )
    endif()
endfunction()

function(nls_target_precompile_headers TARGET_NAME PCH_HEADER)
    if(NOT NLS_ENABLE_PCH)
        return()
    endif()

    if(NOT TARGET ${TARGET_NAME})
        message(FATAL_ERROR "Cannot apply PCH to missing target: ${TARGET_NAME}")
    endif()

    if(NOT EXISTS "${PCH_HEADER}")
        message(FATAL_ERROR "PCH header does not exist: ${PCH_HEADER}")
    endif()

    target_precompile_headers(${TARGET_NAME}
        PRIVATE
            "$<$<COMPILE_LANGUAGE:CXX>:${PCH_HEADER}>"
    )
endfunction()

if(NLS_ENABLE_CXX_MODULES)
    if(MSVC AND CMAKE_GENERATOR MATCHES "Visual Studio")
        set(_nls_modules_pilot_source_dir "${NLS_ROOT_DIR}/Runtime/Math/_DISABLED/ModulesPilot")
        set(_nls_modules_pilot_binary_dir "${CMAKE_BINARY_DIR}/ModulesPilot/$<CONFIG>")
        set(_nls_modules_pilot_ifc "${_nls_modules_pilot_binary_dir}/NullusMathPilot.ifc")
        set(_nls_modules_pilot_interface_obj "${_nls_modules_pilot_binary_dir}/NullusMathPilot.obj")
        set(_nls_modules_pilot_consumer_obj "${_nls_modules_pilot_binary_dir}/NullusMathPilotConsumer.obj")

        add_custom_command(
            OUTPUT
                "${_nls_modules_pilot_ifc}"
                "${_nls_modules_pilot_interface_obj}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${_nls_modules_pilot_binary_dir}"
            COMMAND
                "${CMAKE_CXX_COMPILER}"
                /nologo
                /std:c++20
                /EHsc
                /c
                "${_nls_modules_pilot_source_dir}/NullusMathPilot.ixx"
                "/Fo${_nls_modules_pilot_interface_obj}"
                "/ifcOutput"
                "${_nls_modules_pilot_ifc}"
            DEPENDS "${_nls_modules_pilot_source_dir}/NullusMathPilot.ixx"
            VERBATIM
            COMMENT "Building Nullus C++20 modules pilot interface"
        )

        add_custom_command(
            OUTPUT "${_nls_modules_pilot_consumer_obj}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${_nls_modules_pilot_binary_dir}"
            COMMAND
                "${CMAKE_CXX_COMPILER}"
                /nologo
                /std:c++20
                /EHsc
                /c
                "${_nls_modules_pilot_source_dir}/NullusMathPilotConsumer.cpp"
                "/Fo${_nls_modules_pilot_consumer_obj}"
                "/reference"
                "nullus.math.pilot=${_nls_modules_pilot_ifc}"
            DEPENDS
                "${_nls_modules_pilot_source_dir}/NullusMathPilotConsumer.cpp"
                "${_nls_modules_pilot_ifc}"
            VERBATIM
            COMMENT "Building Nullus C++20 modules pilot consumer"
        )

        add_custom_target(NLS_ModulesPilot
            DEPENDS
                "${_nls_modules_pilot_interface_obj}"
                "${_nls_modules_pilot_consumer_obj}"
        )
        set_target_properties(NLS_ModulesPilot PROPERTIES FOLDER Experimental)
    else()
        message(WARNING "NLS_ENABLE_CXX_MODULES is ON, but the pilot currently supports only MSVC with the Visual Studio generator. No module pilot target will be generated.")
    endif()
endif()

add_subdirectory("ThirdParty")
add_subdirectory("Tools")
add_subdirectory("Runtime")
add_subdirectory("Project")
if(NLS_BUILD_TESTS)
    add_subdirectory("Tests")
endif()
