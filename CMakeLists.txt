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
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
if(MSVC)
    add_compile_options(/FS /source-charset:utf-8 /execution-charset:utf-8)
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
option(NLS_USE_SHARED_LIBRARY "Use Shared Library : OFF|ON (default is ON)" OFF)

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

add_subdirectory("ThirdParty")
add_subdirectory("Tools")
add_subdirectory("Runtime")
add_subdirectory("Tests")
add_subdirectory("Project")
