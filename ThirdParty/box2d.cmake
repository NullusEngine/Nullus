# Box2D v3.2.0, commit 178495ba35695aefe9c321e569d2277f06db3b90.
# The archive hash pins the exact source even if an upstream tag is moved.
include(FetchContent)
set(NLS_BOX2D_SOURCE_DIR "" CACHE PATH "Optional Box2D 3.2.0 source directory")

function(nls_add_box2d_sdk)
    if(CMAKE_VERSION VERSION_LESS 3.22)
        message(FATAL_ERROR "Box2D 3.2.0 requires CMake 3.22 or later")
    endif()
    if(POLICY CMP0135)
        cmake_policy(SET CMP0135 NEW)
    endif()
    if(NLS_BOX2D_SOURCE_DIR)
        set(_box2d_source "${NLS_BOX2D_SOURCE_DIR}")
    else()
        FetchContent_Declare(nullus_box2d
            URL https://codeload.github.com/erincatto/box2d/zip/refs/tags/v3.2.0
            URL_HASH SHA256=ed94013e8d16b19876757920bfa6d7663358aef8ba9407cc18146da6b714029e
            DOWNLOAD_NAME box2d-3.2.0.zip
            DOWNLOAD_DIR "${CMAKE_BINARY_DIR}/_deps"
            SOURCE_SUBDIR nullus-download-only)
        FetchContent_MakeAvailable(nullus_box2d)
        set(_box2d_source "${nullus_box2d_SOURCE_DIR}")
    endif()
    if(NOT EXISTS "${_box2d_source}/include/box2d/box2d.h")
        message(FATAL_ERROR "NLS_BOX2D_SOURCE_DIR must contain the Box2D 3.2.0 source")
    endif()
    file(READ "${_box2d_source}/CMakeLists.txt" _box2d_project)
    if(NOT _box2d_project MATCHES "VERSION[ \t\r\n]+3\\.2\\.0([^0-9]|$)")
        message(FATAL_ERROR "Nullus requires the pinned Box2D 3.2.0 API")
    endif()
    # Isolate SDK options from the engine's shared-library configuration.
    set(BUILD_SHARED_LIBS OFF)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    set(BOX2D_DOUBLE_PRECISION OFF CACHE BOOL "Match Nullus world math" FORCE)
    set(BOX2D_SAMPLES OFF CACHE BOOL "No standalone Box2D samples" FORCE)
    set(BOX2D_UNIT_TESTS OFF CACHE BOOL "Use Nullus GoogleTest integration" FORCE)
    set(BOX2D_BENCHMARKS OFF CACHE BOOL "Use Nullus physics benchmarks" FORCE)
    set(BOX2D_DOCS OFF CACHE BOOL "Do not require Doxygen" FORCE)
    add_subdirectory("${_box2d_source}" "${CMAKE_BINARY_DIR}/box2d-sdk-build" EXCLUDE_FROM_ALL)
    set_target_properties(box2d PROPERTIES FOLDER "ThirdParty/Box2D")
    add_library(NLS_Box2D INTERFACE)
    target_link_libraries(NLS_Box2D INTERFACE box2d)
endfunction()

nls_add_box2d_sdk()
