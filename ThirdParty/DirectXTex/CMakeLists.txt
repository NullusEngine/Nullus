set(NLS_DIRECTXTEX_VERSION "jul2025" CACHE STRING "Pinned DirectXTex source drop expected by Nullus.")
set(NLS_DIRECTXTEX_COMMIT "32b2a8e" CACHE STRING "Pinned DirectXTex commit prefix expected by Nullus.")
set(NLS_DIRECTXTEX_LICENSE "MIT" CACHE STRING "Pinned DirectXTex license.")

set(_nls_directxtex_source_dir "${CMAKE_CURRENT_SOURCE_DIR}/src")

if(WIN32 AND EXISTS "${_nls_directxtex_source_dir}/DirectXTex/DirectXTex.h")
    if(EXISTS "${_nls_directxtex_source_dir}/.git")
        execute_process(
            COMMAND git -C "${_nls_directxtex_source_dir}" rev-parse --short=7 HEAD
            OUTPUT_VARIABLE _nls_directxtex_head
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
        if(_nls_directxtex_head AND NOT _nls_directxtex_head STREQUAL NLS_DIRECTXTEX_COMMIT)
            message(FATAL_ERROR
                "DirectXTex source commit ${_nls_directxtex_head} does not match pinned ${NLS_DIRECTXTEX_COMMIT}; update NLS_DIRECTXTEX_VERSION/COMMIT when changing the source drop.")
        endif()
    endif()
    set(BUILD_SAMPLE OFF CACHE BOOL "" FORCE)
    set(BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    set(BUILD_DX11 OFF CACHE BOOL "" FORCE)
    set(BUILD_DX12 OFF CACHE BOOL "" FORCE)
    set(BUILD_BC7ENC OFF CACHE BOOL "" FORCE)
    set(ENABLE_OPENEXR_SUPPORT OFF CACHE BOOL "" FORCE)
    set(ENABLE_LIBJPEG_SUPPORT OFF CACHE BOOL "" FORCE)
    set(ENABLE_LIBPNG_SUPPORT OFF CACHE BOOL "" FORCE)
    add_subdirectory("${_nls_directxtex_source_dir}" "${CMAKE_BINARY_DIR}/ThirdParty/DirectXTex-src" EXCLUDE_FROM_ALL)
    if(NOT TARGET DirectXTex::DirectXTex)
        add_library(DirectXTex::DirectXTex ALIAS DirectXTex)
    endif()
    target_compile_definitions(DirectXTex INTERFACE
        NLS_HAS_DIRECTXTEX=1
        NLS_DIRECTXTEX_VERSION="${NLS_DIRECTXTEX_VERSION}"
        NLS_DIRECTXTEX_COMMIT="${NLS_DIRECTXTEX_COMMIT}"
        NLS_DIRECTXTEX_LICENSE="${NLS_DIRECTXTEX_LICENSE}")
else()
    add_library(DirectXTex::DirectXTex INTERFACE IMPORTED GLOBAL)
    target_compile_definitions(DirectXTex::DirectXTex INTERFACE
        NLS_HAS_DIRECTXTEX=0
        NLS_DIRECTXTEX_VERSION="${NLS_DIRECTXTEX_VERSION}"
        NLS_DIRECTXTEX_COMMIT="${NLS_DIRECTXTEX_COMMIT}"
        NLS_DIRECTXTEX_LICENSE="${NLS_DIRECTXTEX_LICENSE}")
endif()
