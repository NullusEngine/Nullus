# CMake 最低版本号要求
# ##############################################################################
cmake_minimum_required(VERSION 3.18)

# 设置CMake安装目录
# ##############################################################################
set(CMAKE_INSTALL_PREFIX ${CMAKE_CURRENT_SOURCE_DIR}/Install CACHE PATH "set install path!" FORCE)

# 在生成的项目文件中使用文件夹。开启文件夹组织项
# ##############################################################################
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# 设置项目名称
# ##############################################################################
project(Nullus VERSION 1.0.0 LANGUAGES C CXX)

# 设置c++版本要求
# ##############################################################################
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)

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

set(NLS_RENDER_API_LIST "OpenGL" "Vulkan")
set(NLS_RENDER_API "OpenGL" CACHE STRING "Choose render api")
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
        set(NLS_APP_OUTPUT_PATH "${NLS_APP_PATH}/${TARGET_PLATFORM_NAME}/${CMAKE_BUILD_TYPE}_Shared")
    else()
        set(NLS_APP_OUTPUT_PATH "${NLS_APP_PATH}/${TARGET_PLATFORM_NAME}/${CMAKE_BUILD_TYPE}_Static")
    endif()
endif()

add_subdirectory("Runtime")
add_subdirectory("Project")
add_subdirectory("Plugins")
add_subdirectory("ThirdParty")