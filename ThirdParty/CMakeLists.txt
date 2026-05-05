set(third_party_folder "ThirdParty")

if(NLS_ENABLE_TRACY)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Tracy/CMakeLists.txt")
        set(TRACY_ENABLE ON CACHE BOOL "Enable Tracy profiling" FORCE)
        set(TRACY_NO_SYSTEM_TRACING ON CACHE BOOL "Disable Tracy OS tracing for Nullus integration builds" FORCE)
        add_subdirectory(Tracy)
        if(TARGET TracyClient)
            set_target_properties(TracyClient PROPERTIES FOLDER ${third_party_folder}/Tracy)
        endif()
    elseif(NOT TARGET TracyClient)
        add_library(TracyClient INTERFACE)
        target_compile_definitions(TracyClient INTERFACE NLS_TRACY_UNAVAILABLE=1)
        message(WARNING "NLS_ENABLE_TRACY is ON but ThirdParty/Tracy is not present; Tracy destination will report unavailable.")
    endif()
elseif(NOT TARGET TracyClient)
    add_library(TracyClient INTERFACE)
endif()

if(NLS_BUILD_TRACY_PROFILER AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Tracy/profiler/CMakeLists.txt")
    include(ExternalProject)

    set(_nls_tracy_profiler_build_dir "${CMAKE_BINARY_DIR}/Tools/TracyProfiler")
    set(_nls_tracy_profiler_output_dir "${NLS_APP_OUTPUT_PATH}/Tools")

    if(CMAKE_CONFIGURATION_TYPES)
        set(_nls_tracy_profiler_binary "${_nls_tracy_profiler_build_dir}/$<CONFIG>/tracy-profiler.exe")
        set(_nls_tracy_profiler_build_config_args --config $<CONFIG>)
    else()
        set(_nls_tracy_profiler_binary "${_nls_tracy_profiler_build_dir}/tracy-profiler.exe")
        set(_nls_tracy_profiler_build_config_args)
    endif()

    ExternalProject_Add(TracyProfilerExternal
        SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/Tracy/profiler"
        BINARY_DIR "${_nls_tracy_profiler_build_dir}"
        CMAKE_ARGS
            -DCMAKE_INSTALL_PREFIX=${CMAKE_BINARY_DIR}/Tools/TracyProfilerInstall
            -DCMAKE_C_FLAGS=/utf-8
            -DCMAKE_CXX_FLAGS=/utf-8
            -DLEGACY=ON
        BUILD_COMMAND
            ${CMAKE_COMMAND} --build "${_nls_tracy_profiler_build_dir}" ${_nls_tracy_profiler_build_config_args} --target tracy-profiler
        INSTALL_COMMAND ""
        BUILD_BYPRODUCTS "${_nls_tracy_profiler_binary}"
    )
    set_target_properties(TracyProfilerExternal PROPERTIES FOLDER ${third_party_folder}/Tracy)

    add_custom_target(TracyProfiler
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_nls_tracy_profiler_output_dir}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_nls_tracy_profiler_binary}"
            "${_nls_tracy_profiler_output_dir}/tracy-profiler.exe"
        DEPENDS TracyProfilerExternal
        COMMENT "Copying tracy-profiler.exe to ${_nls_tracy_profiler_output_dir}"
    )
    set_target_properties(TracyProfiler PROPERTIES FOLDER ${third_party_folder}/Tracy)
elseif(NLS_BUILD_TRACY_PROFILER)
    message(WARNING "NLS_BUILD_TRACY_PROFILER is ON but ThirdParty/Tracy/profiler is not present.")
endif()

if(NOT TARGET glfw)
    option(GLFW_BUILD_EXAMPLES "" OFF)
    option(GLFW_BUILD_TESTS "" OFF)
    option(GLFW_BUILD_DOCS "" OFF)
    option(GLFW_INSTALL "" OFF)
    set(BUILD_SHARED_LIBS  ${NLS_USE_SHARED_LIBRARY})
    add_subdirectory(glfw)
    set_target_properties(glfw PROPERTIES FOLDER ${third_party_folder}/glfw)
    set_target_properties(update_mappings PROPERTIES FOLDER ${third_party_folder}/glfw)
    set_target_properties(glfw PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${NLS_APP_OUTPUT_PATH}")
endif()

if(NOT TARGET assimp)
    set(ASSIMP_BUILD_TESTS OFF)
    set(ASSIMP_INSTALL OFF)
    set(ASSIMP_BUILD_ZLIB ON CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS  ${NLS_USE_SHARED_LIBRARY})
    add_subdirectory(assimp)
    if(APPLE)
        set_source_files_properties(
            ${CMAKE_CURRENT_SOURCE_DIR}/assimp/contrib/zip/src/zip.c
            PROPERTIES COMPILE_OPTIONS "-Wno-shorten-64-to-32"
        )
        # assimp's bundled zlib assumes legacy Mac fdopen behavior and may
        # redefine fdopen to NULL on modern Apple toolchains. Keep the symbol
        # defined as itself so the fallback macro path stays disabled.
        if(TARGET zlibstatic)
            target_compile_definitions(zlibstatic PRIVATE "fdopen=fdopen")
        endif()
        if(TARGET assimp)
            target_compile_definitions(assimp PRIVATE "fdopen=fdopen")
        endif()
    endif()
    set_target_properties(assimp PROPERTIES FOLDER ${third_party_folder}/assimp)
    set_target_properties(assimp PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${NLS_APP_OUTPUT_PATH}")
    if(TARGET UpdateAssimpLibsDebugSymbolsAndDLLs)
    set_target_properties(UpdateAssimpLibsDebugSymbolsAndDLLs PROPERTIES FOLDER ${third_party_folder}/assimp)
    endif()
    if(TARGET zlibstatic)
    set_target_properties(zlibstatic PROPERTIES FOLDER ${third_party_folder}/assimp)
    endif()
endif()

if(NOT TARGET ImGui)
    include(imgui.cmake)
    # 指定FOLDER
    set_target_properties(ImGui PROPERTIES FOLDER ${third_party_folder}/ImGui)
endif()

if(NOT TARGET json11)
    add_subdirectory(json11)
    if(MSVC)
        get_target_property(_json11_compile_options json11 COMPILE_OPTIONS)
        if(_json11_compile_options)
            list(REMOVE_ITEM _json11_compile_options "-fPIC" "-fno-rtti" "-fno-exceptions" "-Wextra" "-Werror")
            set_target_properties(json11 PROPERTIES COMPILE_OPTIONS "${_json11_compile_options}")
        endif()
    endif()
    set_target_properties(json11 PROPERTIES FOLDER ${third_party_folder}/json11)
    set_target_properties(json11 PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${NLS_APP_OUTPUT_PATH}")
endif()

if(NOT TARGET FrameGraph)
    set(FG_BUILD_TEST OFF CACHE BOOL "" FORCE)
    add_subdirectory(FrameGraph)
    set_target_properties(FrameGraph PROPERTIES FOLDER ${third_party_folder}/FrameGraph)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/SPIRV-Cross/CMakeLists.txt" AND NOT TARGET spirv-cross-core)
    set(SPIRV_CROSS_CLI OFF CACHE BOOL "" FORCE)
    set(SPIRV_CROSS_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
    set(SPIRV_CROSS_SKIP_INSTALL ON CACHE BOOL "" FORCE)
    set(SPIRV_CROSS_ENABLE_GLSL ON CACHE BOOL "" FORCE)
    set(SPIRV_CROSS_ENABLE_HLSL OFF CACHE BOOL "" FORCE)
    set(SPIRV_CROSS_ENABLE_MSL OFF CACHE BOOL "" FORCE)
    set(SPIRV_CROSS_ENABLE_CPP OFF CACHE BOOL "" FORCE)
    set(SPIRV_CROSS_ENABLE_REFLECT OFF CACHE BOOL "" FORCE)
    set(SPIRV_CROSS_ENABLE_C_API OFF CACHE BOOL "" FORCE)
    set(SPIRV_CROSS_ENABLE_UTIL ON CACHE BOOL "" FORCE)
    add_subdirectory(SPIRV-Cross)
    set_target_properties(spirv-cross-core PROPERTIES FOLDER ${third_party_folder}/SPIRV-Cross)
    if(TARGET spirv-cross-glsl)
        set_target_properties(spirv-cross-glsl PROPERTIES FOLDER ${third_party_folder}/SPIRV-Cross)
    endif()
    if(TARGET spirv-cross-util)
        set_target_properties(spirv-cross-util PROPERTIES FOLDER ${third_party_folder}/SPIRV-Cross)
    endif()
endif()
