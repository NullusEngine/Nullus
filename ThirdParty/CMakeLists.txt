set(third_party_folder "ThirdParty")

option(NLS_ASSIMP_BUILD_ALL_FORMATS "Build Assimp with all upstream default importers/exporters" OFF)
option(NLS_ENABLE_AUTODESK_FBX_SDK "Enable Autodesk FBX SDK integration when the bundled SDK is available" ON)
option(NLS_REQUIRE_BUNDLED_FBX_SDK "Require the bundled Autodesk FBX SDK package during configure" OFF)

function(nls_create_fbx_sdk_unavailable_target reason)
    if(TARGET Autodesk::FbxSdk)
        return()
    endif()

    set(_fbx_setup_guidance
        "Run SetupDependencies before configuring to download and prepare ThirdParty/FBX/sdk/<platform>.")

    if(NLS_REQUIRE_BUNDLED_FBX_SDK)
        message(FATAL_ERROR "${reason} ${_fbx_setup_guidance}")
    endif()

    add_library(Autodesk::FbxSdk INTERFACE IMPORTED GLOBAL)
    target_compile_definitions(Autodesk::FbxSdk INTERFACE NLS_HAS_AUTODESK_FBX_SDK=0)

    set(NLS_BUNDLED_FBX_SDK_ROOT "" CACHE PATH "Resolved bundled Autodesk FBX SDK root" FORCE)
    set(NLS_BUNDLED_FBX_SDK_RUNTIME_LIBRARY "" CACHE FILEPATH "Resolved bundled Autodesk FBX SDK runtime library" FORCE)
    set(NLS_BUNDLED_FBX_SDK_DEBUG_RUNTIME_LIBRARY "" CACHE FILEPATH "Resolved bundled Autodesk FBX SDK debug runtime library" FORCE)

    message(WARNING
        "${reason} "
        "Autodesk FBX import will be unavailable in this build. "
        "${_fbx_setup_guidance} "
        "Set NLS_REQUIRE_BUNDLED_FBX_SDK=ON to make this a configure error.")
endfunction()

function(nls_find_fbx_sdk)
    if(TARGET Autodesk::FbxSdk)
        return()
    endif()

    if(NOT NLS_ENABLE_AUTODESK_FBX_SDK)
        nls_create_fbx_sdk_unavailable_target("Autodesk FBX SDK integration is disabled by NLS_ENABLE_AUTODESK_FBX_SDK=OFF.")
        return()
    endif()

    if(WIN32)
        set(_fbx_root "${CMAKE_CURRENT_SOURCE_DIR}/FBX/sdk/windows")
    elseif(APPLE)
        set(_fbx_root "${CMAKE_CURRENT_SOURCE_DIR}/FBX/sdk/macos")
    elseif(UNIX)
        set(_fbx_root "${CMAKE_CURRENT_SOURCE_DIR}/FBX/sdk/linux")
    else()
        nls_create_fbx_sdk_unavailable_target("Autodesk FBX SDK has no bundled SDK layout for this platform.")
        return()
    endif()
    file(TO_CMAKE_PATH "${_fbx_root}" _fbx_root)

    if(NOT EXISTS "${_fbx_root}/include/fbxsdk.h")
        nls_create_fbx_sdk_unavailable_target(
            "Bundled Autodesk FBX SDK was not found at '${_fbx_root}'. "
            "Unpack the official package from ThirdParty/FBX/packages into ThirdParty/FBX/sdk/<platform>. "
            "Nullus does not use external FBX SDK installs.")
        return()
    endif()

    if(NOT EXISTS "${_fbx_root}/include/fbxsdk/fbxsdk_version.h")
        nls_create_fbx_sdk_unavailable_target("FBX SDK root '${_fbx_root}' is missing include/fbxsdk/fbxsdk_version.h")
        return()
    endif()

    file(READ "${_fbx_root}/include/fbxsdk/fbxsdk_version.h" _fbx_version_header)
    if(NOT _fbx_version_header MATCHES "FBXSDK_VERSION_MAJOR[ \t]+2020"
        OR NOT _fbx_version_header MATCHES "FBXSDK_VERSION_MINOR[ \t]+3"
        OR NOT _fbx_version_header MATCHES "FBXSDK_VERSION_POINT[ \t]+9")
        nls_create_fbx_sdk_unavailable_target("FBX SDK root '${_fbx_root}' is not Autodesk FBX SDK 2020.3.9.")
        return()
    endif()

    set(_fbx_include_dir "${_fbx_root}/include")
    set(_fbx_compile_definitions FBXSDK_SHARED NLS_HAS_AUTODESK_FBX_SDK=1)
    set(_fbx_runtime_library "")
    set(_fbx_import_library "")
    set(_fbx_link_library "")

    if(WIN32)
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|aarch64")
            set(_fbx_arch "arm64")
        else()
            set(_fbx_arch "x64")
        endif()
        set(_fbx_release_dir "${_fbx_root}/lib/${_fbx_arch}/release")
        set(_fbx_debug_dir "${_fbx_root}/lib/${_fbx_arch}/debug")
        set(_fbx_import_library "${_fbx_release_dir}/libfbxsdk.lib")
        set(_fbx_runtime_library "${_fbx_release_dir}/libfbxsdk.dll")
        set(_fbx_debug_import_library "${_fbx_debug_dir}/libfbxsdk.lib")
        set(_fbx_debug_runtime_library "${_fbx_debug_dir}/libfbxsdk.dll")
        if(EXISTS "${_fbx_debug_import_library}")
            set(_fbx_link_library "$<$<CONFIG:Debug>:${_fbx_debug_import_library}>$<$<NOT:$<CONFIG:Debug>>:${_fbx_import_library}>")
        else()
            set(_fbx_link_library "${_fbx_import_library}")
        endif()
        if(NOT EXISTS "${_fbx_debug_runtime_library}")
            set(_fbx_debug_runtime_library "${_fbx_runtime_library}")
        endif()
    elseif(APPLE)
        foreach(_path
            "${_fbx_root}/lib/clang/release/libfbxsdk.dylib"
            "${_fbx_root}/lib/libfbxsdk.dylib"
            "${_fbx_root}/lib/clang/libfbxsdk.dylib")
            if(EXISTS "${_path}")
                set(_fbx_link_library "${_path}")
                set(_fbx_runtime_library "${_path}")
                break()
            endif()
        endforeach()
    elseif(UNIX)
        foreach(_path
            "${_fbx_root}/lib/gcc/x64/release/libfbxsdk.so"
            "${_fbx_root}/lib/gcc/x86_64/release/libfbxsdk.so"
            "${_fbx_root}/lib/release/libfbxsdk.so"
            "${_fbx_root}/lib/libfbxsdk.so")
            if(EXISTS "${_path}")
                set(_fbx_link_library "${_path}")
                set(_fbx_runtime_library "${_path}")
                break()
            endif()
        endforeach()
    endif()

    if(WIN32)
        if(NOT EXISTS "${_fbx_import_library}" OR NOT EXISTS "${_fbx_runtime_library}")
            nls_create_fbx_sdk_unavailable_target("FBX SDK root '${_fbx_root}' is missing ${_fbx_import_library} or ${_fbx_runtime_library}.")
            return()
        endif()
    elseif(NOT _fbx_link_library OR NOT EXISTS "${_fbx_link_library}")
        nls_create_fbx_sdk_unavailable_target("FBX SDK root '${_fbx_root}' is missing the platform libfbxsdk shared library.")
        return()
    endif()

    add_library(Autodesk::FbxSdk INTERFACE IMPORTED GLOBAL)
    target_include_directories(Autodesk::FbxSdk INTERFACE "${_fbx_include_dir}")
    target_compile_definitions(Autodesk::FbxSdk INTERFACE ${_fbx_compile_definitions})
    target_link_libraries(Autodesk::FbxSdk INTERFACE "${_fbx_link_library}")
    if(UNIX AND NOT APPLE)
        target_link_libraries(Autodesk::FbxSdk INTERFACE stdc++)
    endif()

    set(NLS_BUNDLED_FBX_SDK_ROOT "${_fbx_root}" CACHE PATH "Resolved bundled Autodesk FBX SDK root" FORCE)
    set(NLS_BUNDLED_FBX_SDK_RUNTIME_LIBRARY "${_fbx_runtime_library}" CACHE FILEPATH "Resolved bundled Autodesk FBX SDK runtime library" FORCE)
    set(NLS_BUNDLED_FBX_SDK_DEBUG_RUNTIME_LIBRARY "${_fbx_debug_runtime_library}" CACHE FILEPATH "Resolved bundled Autodesk FBX SDK debug runtime library" FORCE)

    add_custom_target(AutodeskFbxSdkRuntime
        COMMAND ${CMAKE_COMMAND} -E make_directory "${NLS_APP_OUTPUT_PATH}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<IF:$<AND:$<BOOL:${WIN32}>,$<CONFIG:Debug>>,${_fbx_debug_runtime_library},${_fbx_runtime_library}>"
            "${NLS_APP_OUTPUT_PATH}/$<IF:$<BOOL:${WIN32}>,libfbxsdk.dll,$<IF:$<BOOL:${APPLE}>,libfbxsdk.dylib,libfbxsdk.so>>"
        COMMENT "Copying Autodesk FBX SDK runtime to ${NLS_APP_OUTPUT_PATH}"
        VERBATIM)
    set_target_properties(AutodeskFbxSdkRuntime PROPERTIES FOLDER ${third_party_folder}/FBX)

    message(STATUS "Autodesk FBX SDK 2020.3.9 root: ${_fbx_root}")
endfunction()

nls_find_fbx_sdk()

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
    if(MSVC)
        set(_nls_saved_cmake_cxx_flags_debug "${CMAKE_CXX_FLAGS_DEBUG}")
        string(REGEX REPLACE "(^| )/RTC[0-9A-Za-z+-]*" " " CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
    endif()
    set(ASSIMP_BUILD_TESTS OFF)
    set(ASSIMP_INSTALL OFF)
    set(ASSIMP_BUILD_ZLIB ON CACHE BOOL "" FORCE)
    if(NLS_ASSIMP_BUILD_ALL_FORMATS)
        set(ASSIMP_BUILD_ALL_IMPORTERS_BY_DEFAULT ON CACHE BOOL "default value of all ASSIMP_BUILD_XXX_IMPORTER values" FORCE)
        set(ASSIMP_BUILD_ALL_EXPORTERS_BY_DEFAULT ON CACHE BOOL "default value of all ASSIMP_BUILD_XXX_EXPORTER values" FORCE)
        set(ASSIMP_NO_EXPORT OFF CACHE BOOL "Disable Assimp's export functionality." FORCE)
    else()
        set(ASSIMP_BUILD_ALL_IMPORTERS_BY_DEFAULT OFF CACHE BOOL "default value of all ASSIMP_BUILD_XXX_IMPORTER values" FORCE)
        set(ASSIMP_BUILD_ALL_EXPORTERS_BY_DEFAULT OFF CACHE BOOL "default value of all ASSIMP_BUILD_XXX_EXPORTER values" FORCE)
        set(ASSIMP_BUILD_FBX_IMPORTER OFF CACHE BOOL "FBX is imported through Autodesk FBX SDK only" FORCE)
        set(ASSIMP_BUILD_GLTF_IMPORTER ON CACHE BOOL "Enable glTF file import" FORCE)
        set(ASSIMP_BUILD_OBJ_IMPORTER ON CACHE BOOL "Enable OBJ file import" FORCE)
        set(ASSIMP_NO_EXPORT ON CACHE BOOL "Disable Assimp's export functionality." FORCE)
    endif()
    set(BUILD_SHARED_LIBS  ${NLS_USE_SHARED_LIBRARY})
    add_subdirectory(assimp)
    if(MSVC)
        set(CMAKE_CXX_FLAGS_DEBUG "${_nls_saved_cmake_cxx_flags_debug}")
    endif()
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
    if(MSVC)
        target_compile_options(assimp PRIVATE "$<$<CONFIG:Debug>:/O2>")
    endif()
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

if(NOT TARGET FrameGraph)
    set(FG_BUILD_TEST OFF CACHE BOOL "" FORCE)
    set(_nls_framegraph_build_shared_libs "${BUILD_SHARED_LIBS}")
    set(BUILD_SHARED_LIBS OFF)
    add_subdirectory(FrameGraph)
    set(BUILD_SHARED_LIBS "${_nls_framegraph_build_shared_libs}")
    if(NLS_USE_SHARED_LIBRARY)
        set_target_properties(FrameGraph PROPERTIES POSITION_INDEPENDENT_CODE ON)
    endif()
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
    if(NLS_USE_SHARED_LIBRARY)
        foreach(_spirv_cross_target spirv-cross-core spirv-cross-glsl spirv-cross-util)
            if(TARGET ${_spirv_cross_target})
                set_target_properties(${_spirv_cross_target} PROPERTIES POSITION_INDEPENDENT_CODE ON)
            endif()
        endforeach()
    endif()
    set_target_properties(spirv-cross-core PROPERTIES FOLDER ${third_party_folder}/SPIRV-Cross)
    if(TARGET spirv-cross-glsl)
        set_target_properties(spirv-cross-glsl PROPERTIES FOLDER ${third_party_folder}/SPIRV-Cross)
    endif()
    if(TARGET spirv-cross-util)
        set_target_properties(spirv-cross-util PROPERTIES FOLDER ${third_party_folder}/SPIRV-Cross)
    endif()
endif()
