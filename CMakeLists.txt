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
    # Release remains optimized (/O2), but Mixed debugging still needs
    # portable native symbols for Editor and the first-party runtime DLLs.
    # Keep this at the build-graph boundary so every native project uses the
    # same PDB contract instead of only the top-level executable.
    add_compile_options("$<$<CONFIG:Release>:/Zi>")
    set(CMAKE_EXE_LINKER_FLAGS_RELEASE
        "${CMAKE_EXE_LINKER_FLAGS_RELEASE} /DEBUG")
    set(CMAKE_SHARED_LINKER_FLAGS_RELEASE
        "${CMAKE_SHARED_LINKER_FLAGS_RELEASE} /DEBUG")
elseif(MINGW)
    # MetaParser can generate translation units that exceed the default
    # GNU assembler section limit on Windows.
    add_compile_options(-Wa,-mbig-obj)
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
option(NLS_BUILD_PERFORMANCE_TESTS "Build Nullus performance and stress tests" OFF)
option(NLS_ENABLE_CSHARP_SCRIPTING "Enable the optional CoreCLR scripting backend" ON)
option(NLS_ENABLE_LUA_SCRIPTING "Enable the optional Lua scripting backend" ON)
# LuaPanda is an Editor Debug dependency. Keep it opt-in so a fresh Release or
# Player configure cannot accidentally ship the debugger and LuaSocket.
option(NLS_ENABLE_LUAPANDA_DEBUGGING "Compile the optional LuaPanda debugger integration for Debug builds" OFF)
set(NLS_PROJECT_ASSETS_DIR "" CACHE PATH "Project Assets directory whose .cs files are included in GameScripts.dll")
set(NLS_ENABLE_TEST_HOOKS "AUTO" CACHE STRING "Expose test-only runtime hooks for Nullus tests: AUTO, ON, or OFF")
set_property(CACHE NLS_ENABLE_TEST_HOOKS PROPERTY STRINGS AUTO ON OFF)
string(TOUPPER "${NLS_ENABLE_TEST_HOOKS}" _nls_enable_test_hooks_mode)
if(_nls_enable_test_hooks_mode STREQUAL "AUTO")
    if(NLS_BUILD_TESTS OR NLS_BUILD_PERFORMANCE_TESTS)
        set(_nls_enable_test_hooks_resolved ON)
    else()
        set(_nls_enable_test_hooks_resolved OFF)
    endif()
elseif(_nls_enable_test_hooks_mode MATCHES "^(ON|TRUE|YES|1)$")
    set(_nls_enable_test_hooks_resolved ON)
elseif(_nls_enable_test_hooks_mode MATCHES "^(OFF|FALSE|NO|0)$")
    set(_nls_enable_test_hooks_resolved OFF)
else()
    message(FATAL_ERROR "NLS_ENABLE_TEST_HOOKS must be AUTO, ON, or OFF; got '${NLS_ENABLE_TEST_HOOKS}'.")
endif()
set(NLS_ENABLE_TEST_HOOKS ${_nls_enable_test_hooks_resolved})
message(STATUS "Nullus test hooks: ${NLS_ENABLE_TEST_HOOKS} (mode: ${_nls_enable_test_hooks_mode})")
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

if(NLS_RENDER_API STREQUAL "Vulkan" AND NOT Vulkan_FOUND)
    message(FATAL_ERROR
        "Vulkan render API was explicitly requested, but the Vulkan SDK/loader was not found. "
        "Install the Vulkan SDK (including headers and vulkan-1.lib/libvulkan), or set "
        "VULKAN_SDK/VK_SDK_PATH. The Vulkan path does not silently fall back to another backend.")
endif()

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

include("${NLS_ROOT_DIR}/Tools/CMake/NullusDotnet.cmake")
nls_resolve_dotnet()

function(nls_add_windows_app_icon target_name)
    if(NOT WIN32)
        return()
    endif()

    set(NLS_APP_ICON_ICO "${NLS_ROOT_DIR}/App/Resources/NullusAppIcon.ico")
    set(_nls_app_icon_rc "${CMAKE_CURRENT_BINARY_DIR}/${target_name}NullusAppIcon.rc")
    configure_file(
        "${NLS_ROOT_DIR}/App/Resources/NullusAppIcon.rc.in"
        "${_nls_app_icon_rc}"
        @ONLY)
    target_sources(${target_name} PRIVATE "${_nls_app_icon_rc}")
endfunction()

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

# Shared libraries are build artifacts, so keep them isolated from other CMake
# build trees. Executable targets explicitly deploy this directory to App.
set(NLS_RUNTIME_OUTPUT_PATH "${CMAKE_BINARY_DIR}/runtime/$<CONFIGURATION>")

function(nls_configure_shared_runtime_output TARGET_NAME)
    set_target_properties(${TARGET_NAME} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${NLS_RUNTIME_OUTPUT_PATH}"
        LIBRARY_OUTPUT_DIRECTORY "${NLS_RUNTIME_OUTPUT_PATH}"
    )
    get_target_property(_nls_runtime_target_type ${TARGET_NAME} TYPE)
    if(_nls_runtime_target_type STREQUAL "SHARED_LIBRARY" OR
       _nls_runtime_target_type STREQUAL "MODULE_LIBRARY")
        set_property(GLOBAL APPEND PROPERTY NLS_SHARED_RUNTIME_TARGETS ${TARGET_NAME})
    endif()
endfunction()

function(nls_register_external_runtime_file)
    set(options)
    set(oneValueArgs FILE_NAME PRODUCER_TARGET)
    cmake_parse_arguments(NLS_EXTERNAL "${options}" "${oneValueArgs}" "" ${ARGN})
    if(NLS_EXTERNAL_UNPARSED_ARGUMENTS OR
       NOT NLS_EXTERNAL_FILE_NAME OR
       NOT NLS_EXTERNAL_PRODUCER_TARGET)
        message(FATAL_ERROR
            "nls_register_external_runtime_file requires FILE_NAME and PRODUCER_TARGET")
    endif()
    if(NOT TARGET ${NLS_EXTERNAL_PRODUCER_TARGET})
        message(FATAL_ERROR
            "External runtime producer target does not exist: ${NLS_EXTERNAL_PRODUCER_TARGET}")
    endif()

    set_property(GLOBAL APPEND PROPERTY
        NLS_EXTERNAL_RUNTIME_FILES "${NLS_EXTERNAL_FILE_NAME}")
    set_property(GLOBAL APPEND PROPERTY
        NLS_EXTERNAL_RUNTIME_PRODUCERS "${NLS_EXTERNAL_PRODUCER_TARGET}")
endfunction()

function(nls_copy_shared_runtime_to_target TARGET_NAME)
    if(NLS_USE_SHARED_LIBRARY)
        get_property(_nls_runtime_targets GLOBAL PROPERTY NLS_SHARED_RUNTIME_TARGETS)
        if(_nls_runtime_targets)
            # The deployment manifest contains every shared runtime module, so
            # the consumer must wait for all of them before its post-build copy.
            add_dependencies(${TARGET_NAME} ${_nls_runtime_targets})
        endif()
        set_property(GLOBAL APPEND PROPERTY NLS_SHARED_RUNTIME_CONSUMERS ${TARGET_NAME})
        add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND}
                "-DSTAGE_DIR=${NLS_RUNTIME_OUTPUT_PATH}"
                "-DDEST_DIR=$<TARGET_FILE_DIR:${TARGET_NAME}>"
                "-DCONFIG=$<CONFIG>"
                "-DPRESERVE_FILE=$<TARGET_FILE_NAME:${TARGET_NAME}>"
                "-DEXPECTED_MANIFEST=${CMAKE_BINARY_DIR}/runtime-manifests/${TARGET_NAME}-$<CONFIG>.txt"
                -P "${NLS_ROOT_DIR}/Tools/CMake/DeployRuntime.cmake"
            VERBATIM
            COMMENT "Deploying Nullus runtime libraries for ${TARGET_NAME}"
        )
    endif()
endfunction()

function(nls_generate_shared_runtime_manifests)
    if(NOT NLS_USE_SHARED_LIBRARY)
        return()
    endif()

    get_property(_nls_runtime_targets GLOBAL PROPERTY NLS_SHARED_RUNTIME_TARGETS)
    get_property(_nls_runtime_consumers GLOBAL PROPERTY NLS_SHARED_RUNTIME_CONSUMERS)
    get_property(_nls_external_runtime_files GLOBAL PROPERTY NLS_EXTERNAL_RUNTIME_FILES)
    get_property(_nls_external_runtime_producers GLOBAL PROPERTY NLS_EXTERNAL_RUNTIME_PRODUCERS)
    list(REMOVE_DUPLICATES _nls_runtime_targets)
    list(REMOVE_DUPLICATES _nls_runtime_consumers)
    list(REMOVE_DUPLICATES _nls_external_runtime_files)
    list(REMOVE_DUPLICATES _nls_external_runtime_producers)

    set(_nls_expected_runtime_files "")
    foreach(_nls_runtime_target IN LISTS _nls_runtime_targets)
        string(APPEND _nls_expected_runtime_files "$<TARGET_FILE_NAME:${_nls_runtime_target}>\n")
        get_target_property(_nls_runtime_target_type ${_nls_runtime_target} TYPE)
        if(UNIX AND _nls_runtime_target_type STREQUAL "SHARED_LIBRARY")
            string(APPEND _nls_expected_runtime_files "$<TARGET_SONAME_FILE_NAME:${_nls_runtime_target}>\n")
            string(APPEND _nls_expected_runtime_files "$<TARGET_LINKER_FILE_NAME:${_nls_runtime_target}>\n")
        endif()
    endforeach()
    foreach(_nls_external_runtime_file IN LISTS _nls_external_runtime_files)
        string(APPEND _nls_expected_runtime_files "${_nls_external_runtime_file}\n")
    endforeach()

    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/runtime-manifests")
    foreach(_nls_runtime_consumer IN LISTS _nls_runtime_consumers)
        # Some consumers are declared before Runtime, so their copy helper
        # cannot see the complete shared-runtime target list yet.
        if(_nls_runtime_targets)
            add_dependencies(${_nls_runtime_consumer} ${_nls_runtime_targets})
        endif()
        if(_nls_external_runtime_producers)
            add_dependencies(${_nls_runtime_consumer} ${_nls_external_runtime_producers})
        endif()
        file(GENERATE
            OUTPUT "${CMAKE_BINARY_DIR}/runtime-manifests/${_nls_runtime_consumer}-$<CONFIG>.txt"
            CONTENT "${_nls_expected_runtime_files}")
    endforeach()
endfunction()

function(nls_copy_app_assets_to_target TARGET_NAME)
    set(_nls_app_assets_source "${NLS_ROOT_DIR}/App/Assets")
    if(NOT EXISTS "${_nls_app_assets_source}")
        return()
    endif()

    add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${_nls_app_assets_source}"
            "$<TARGET_FILE_DIR:${TARGET_NAME}>/Assets"
        VERBATIM
        COMMENT "Copying Nullus app assets for ${TARGET_NAME}"
    )
endfunction()

function(nls_copy_editor_resources_to_target TARGET_NAME)
    nls_copy_app_assets_to_target(${TARGET_NAME})
    set(_nls_engine_scripts_source "${NLS_ROOT_DIR}/Managed/Nullus.EngineScripts/Scripts")
    if(EXISTS "${_nls_engine_scripts_source}")
        add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_FILE_DIR:${TARGET_NAME}>/Assets/Engine/Scripts"
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${_nls_engine_scripts_source}"
                "$<TARGET_FILE_DIR:${TARGET_NAME}>/Assets/Engine/Scripts"
            VERBATIM
            COMMENT "Copying Nullus built-in scripts for ${TARGET_NAME}"
        )
    endif()
endfunction()

if(EXISTS "${NLS_ROOT_DIR}/App/Assets")
    install(
        DIRECTORY "${NLS_ROOT_DIR}/App/Assets/"
        DESTINATION "Assets")
endif()
if(EXISTS "${NLS_ROOT_DIR}/Managed/Nullus.EngineScripts/Scripts")
    install(
        DIRECTORY "${NLS_ROOT_DIR}/Managed/Nullus.EngineScripts/Scripts/"
        DESTINATION "Assets/Engine/Scripts")
endif()

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
        file(GLOB _nls_modules_pilot_interface_files CONFIGURE_DEPENDS
            "${_nls_modules_pilot_source_dir}/*.ixx"
        )
        file(GLOB _nls_modules_pilot_consumer_files CONFIGURE_DEPENDS
            "${_nls_modules_pilot_source_dir}/*.cpp"
        )
        list(LENGTH _nls_modules_pilot_interface_files _nls_modules_pilot_interface_count)
        list(LENGTH _nls_modules_pilot_consumer_files _nls_modules_pilot_consumer_count)
        if(NOT _nls_modules_pilot_interface_count EQUAL 1 OR NOT _nls_modules_pilot_consumer_count EQUAL 1)
            message(FATAL_ERROR "Modules pilot expects exactly one .ixx interface and one .cpp consumer in ${_nls_modules_pilot_source_dir}.")
        endif()
        list(GET _nls_modules_pilot_interface_files 0 _nls_modules_pilot_interface_source)
        list(GET _nls_modules_pilot_consumer_files 0 _nls_modules_pilot_consumer_source)
        get_filename_component(_nls_modules_pilot_interface_stem "${_nls_modules_pilot_interface_source}" NAME_WE)
        get_filename_component(_nls_modules_pilot_consumer_stem "${_nls_modules_pilot_consumer_source}" NAME_WE)
        set(_nls_modules_pilot_binary_dir "${CMAKE_BINARY_DIR}/ModulesPilot/$<CONFIG>")
        set(_nls_modules_pilot_ifc "${_nls_modules_pilot_binary_dir}/${_nls_modules_pilot_interface_stem}.ifc")
        set(_nls_modules_pilot_interface_obj "${_nls_modules_pilot_binary_dir}/${_nls_modules_pilot_interface_stem}.obj")
        set(_nls_modules_pilot_consumer_obj "${_nls_modules_pilot_binary_dir}/${_nls_modules_pilot_consumer_stem}.obj")

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
                "${_nls_modules_pilot_interface_source}"
                "/Fo${_nls_modules_pilot_interface_obj}"
                "/ifcOutput"
                "${_nls_modules_pilot_ifc}"
            DEPENDS "${_nls_modules_pilot_interface_source}"
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
                "${_nls_modules_pilot_consumer_source}"
                "/Fo${_nls_modules_pilot_consumer_obj}"
                "/reference"
                "nullus.math.pilot=${_nls_modules_pilot_ifc}"
            DEPENDS
                "${_nls_modules_pilot_consumer_source}"
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

# Vulkan is an explicit runtime choice on both Linux and Windows. Fail during
# configure when a required WSI or shader tool is absent instead of producing a
# binary that can only fail later while compiling shaders or creating a window.
if(NLS_RENDER_API STREQUAL "Vulkan")
    if(NOT TARGET glfw)
        message(FATAL_ERROR
            "Vulkan requires the bundled GLFW target for Vulkan WSI. "
            "Ensure ThirdParty/glfw is present and configure again.")
    endif()

    if(DEFINED ENV{DXC_PATH} AND NOT "$ENV{DXC_PATH}" STREQUAL "")
        set(_nls_dxc_env_path "$ENV{DXC_PATH}")
    endif()
    if(WIN32)
        set(_nls_dxc_names dxc.exe dxc)
        set(_nls_dxc_paths
            "${CMAKE_CURRENT_SOURCE_DIR}/Tools/DXC/1.9.2602/bin/x64"
            "${CMAKE_CURRENT_SOURCE_DIR}/Tools/DXC/bin/x64")
    else()
        set(_nls_dxc_names dxc)
        set(_nls_dxc_paths
            "${CMAKE_CURRENT_SOURCE_DIR}/Tools/DXC/1.7.2308/bin"
            "${CMAKE_CURRENT_SOURCE_DIR}/Tools/DXC/bin")
    endif()
    find_program(NLS_VULKAN_DXC_EXECUTABLE
        NAMES ${_nls_dxc_names}
        HINTS "${_nls_dxc_env_path}"
        PATHS ${_nls_dxc_paths})
    if(NOT NLS_VULKAN_DXC_EXECUTABLE)
        message(FATAL_ERROR
            "Vulkan requires a native dxc executable for SPIR-V shader compilation. "
            "Set DXC_PATH or install DirectXShaderCompiler; the bundled path is "
            "Tools/DXC/1.9.2602/bin/x64/dxc.exe on Windows or "
            "Tools/DXC/1.7.2308/bin/dxc on Linux.")
    endif()
    set(DXC_PATH "${NLS_VULKAN_DXC_EXECUTABLE}" CACHE FILEPATH
        "Native DXC executable used by the Vulkan shader compiler" FORCE)

    if(NOT WIN32)
        find_program(NLS_SPIRV_VALIDATOR_EXECUTABLE
            NAMES spirv-val spirvValidator glslangValidator)
        if(NOT NLS_SPIRV_VALIDATOR_EXECUTABLE)
            message(FATAL_ERROR
                "Linux Vulkan requires a SPIR-V validation tool (spirv-val or glslangValidator). "
                "Install spirv-tools or the Vulkan SDK.")
        endif()
        set(NLS_SPIRV_VALIDATOR_EXECUTABLE "${NLS_SPIRV_VALIDATOR_EXECUTABLE}" CACHE FILEPATH
            "SPIR-V validation tool used by the Vulkan build" FORCE)
    endif()
endif()

add_subdirectory("Tools")
add_subdirectory("Runtime")
if(NLS_ENABLE_CSHARP_SCRIPTING)
    # Managed artifact commands consume the Engine-generated Script API. Keep
    # this directory after Runtime so the file-level producer is already in
    # the CMake graph when the command is declared.
    add_subdirectory("Managed")
endif()
add_subdirectory("Project")
if(NLS_BUILD_TESTS OR NLS_BUILD_PERFORMANCE_TESTS)
    add_subdirectory("Tests")
endif()
nls_generate_shared_runtime_manifests()

# Describe the existing CMake/Visual Studio build graph for project-scoped
# mixed debugging. The generated project solution references these projects;
# it never creates a second native build tree or copies native sources.
set(_nls_engine_workspace_projects
    "Editor|Project/Editor/Editor.vcxproj|Project/Editor"
    "EditorProject|Project/Editor/EditorProject.vcxproj|Project/Editor"
    "NLS_Base|Runtime/Base/NLS_Base.vcxproj|Runtime/Base"
    "NLS_Core|Runtime/Core/NLS_Core.vcxproj|Runtime/Core"
    "NLS_Math|Runtime/Math/NLS_Math.vcxproj|Runtime/Math"
    "NLS_Platform|Runtime/Platform/NLS_Platform.vcxproj|Runtime/Platform"
    "NLS_Render|Runtime/Rendering/NLS_Render.vcxproj|Runtime/Rendering"
    "NLS_UI|Runtime/UI/NLS_UI.vcxproj|Runtime/UI"
    "NLS_Engine|Runtime/Engine/NLS_Engine.vcxproj|Runtime/Engine"
    "NLS_Scripting|Runtime/Scripting/NLS_Scripting.vcxproj|Runtime/Scripting")

if(CMAKE_GENERATOR MATCHES "Visual Studio")
    set(_nls_engine_workspace_visual_studio true)
else()
    set(_nls_engine_workspace_visual_studio false)
endif()

file(TO_CMAKE_PATH "${NLS_ROOT_DIR}" _nls_engine_workspace_source_root)
file(TO_CMAKE_PATH "${CMAKE_BINARY_DIR}" _nls_engine_workspace_build_root)
if(MSVC)
    file(TO_CMAKE_PATH
        "${NLS_ROOT_DIR}/App/${TARGET_PLATFORM_NAME}_Debug_Runtime_Shared/Editor.pdb"
        _nls_engine_workspace_debug_pdb)
    file(TO_CMAKE_PATH
        "${NLS_ROOT_DIR}/App/${TARGET_PLATFORM_NAME}_Release_Runtime_Shared/Editor.pdb"
        _nls_engine_workspace_release_pdb)
else()
    file(TO_CMAKE_PATH
        "${NLS_ROOT_DIR}/App/${TARGET_PLATFORM_NAME}_Debug_Shared/Editor.pdb"
        _nls_engine_workspace_debug_pdb)
    file(TO_CMAKE_PATH
        "${NLS_ROOT_DIR}/App/${TARGET_PLATFORM_NAME}_Release_Shared/Editor.pdb"
        _nls_engine_workspace_release_pdb)
endif()
set(_nls_engine_workspace_project_json "")
list(LENGTH _nls_engine_workspace_projects _nls_engine_workspace_project_count)
set(_nls_engine_workspace_project_index 0)
foreach(_nls_project IN LISTS _nls_engine_workspace_projects)
    string(REPLACE "|" ";" _nls_project_parts "${_nls_project}")
    list(GET _nls_project_parts 0 _nls_project_name)
    list(GET _nls_project_parts 1 _nls_project_path)
    list(GET _nls_project_parts 2 _nls_project_source_root)
    math(EXPR _nls_engine_workspace_project_index "${_nls_engine_workspace_project_index} + 1")
    set(_nls_project_comma ",")
    if(_nls_engine_workspace_project_index EQUAL _nls_engine_workspace_project_count)
        set(_nls_project_comma "")
    endif()
    string(APPEND _nls_engine_workspace_project_json
        "    {\"name\": \"${_nls_project_name}\", \"path\": \"${_nls_project_path}\", \"sourceRoot\": \"${_nls_project_source_root}\", \"kind\": \"native\"}${_nls_project_comma}\n")
endforeach()
set(NLS_ENGINE_WORKSPACE_VISUAL_STUDIO "${_nls_engine_workspace_visual_studio}")
set(NLS_ENGINE_WORKSPACE_SOURCE_ROOT "${_nls_engine_workspace_source_root}")
set(NLS_ENGINE_WORKSPACE_BUILD_ROOT "${_nls_engine_workspace_build_root}")
set(NLS_ENGINE_WORKSPACE_DEBUG_PDB "${_nls_engine_workspace_debug_pdb}")
set(NLS_ENGINE_WORKSPACE_RELEASE_PDB "${_nls_engine_workspace_release_pdb}")
set(NLS_ENGINE_WORKSPACE_PROJECTS "${_nls_engine_workspace_project_json}")
configure_file(
    "${NLS_ROOT_DIR}/Tools/CMake/NullusEngineWorkspace.json.in"
    "${CMAKE_BINARY_DIR}/Nullus.EngineWorkspace.json"
    @ONLY)
