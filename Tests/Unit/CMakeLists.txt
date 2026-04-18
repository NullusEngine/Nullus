file(GLOB_RECURSE NULLUS_UNIT_TEST_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.inl"
)

# Exclude DX12 tests on non-Windows platforms
if(NOT WIN32)
    list(FILTER NULLUS_UNIT_TEST_FILES EXCLUDE REGEX "[/\\\\]DX12.*\\.cpp$")
endif()

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${NULLUS_UNIT_TEST_FILES})

add_executable(NullusUnitTests ${NULLUS_UNIT_TEST_FILES})

target_sources(NullusUnitTests PRIVATE
    ${CMAKE_SOURCE_DIR}/Project/Editor/Panels/FrameInfoRendererStats.cpp
    ${CMAKE_SOURCE_DIR}/Project/Editor/Panels/FrameInfo.h
    ${CMAKE_SOURCE_DIR}/Project/Game/LaunchArgs.cpp
    ${CMAKE_SOURCE_DIR}/Project/Game/LaunchArgs.h
    ${CMAKE_SOURCE_DIR}/Project/Launcher/Core/LauncherLocalization.cpp
    ${CMAKE_SOURCE_DIR}/Project/Launcher/Core/LauncherLocalization.h
    ${CMAKE_SOURCE_DIR}/Project/Launcher/Core/LauncherProjectMetadata.cpp
    ${CMAKE_SOURCE_DIR}/Project/Launcher/Core/LauncherProjectMetadata.h
    ${CMAKE_SOURCE_DIR}/Project/Launcher/Core/LauncherSettings.cpp
    ${CMAKE_SOURCE_DIR}/Project/Launcher/Core/LauncherSettings.h
    ${CMAKE_SOURCE_DIR}/Project/Launcher/Core/LauncherNavigation.h
)

target_compile_features(NullusUnitTests PRIVATE cxx_std_20)

target_include_directories(NullusUnitTests PRIVATE
    ${CMAKE_SOURCE_DIR}/Runtime
    ${CMAKE_SOURCE_DIR}/Runtime/Base
    ${CMAKE_SOURCE_DIR}/Runtime/Base/Reflection
    ${CMAKE_SOURCE_DIR}/Runtime/Core
    ${CMAKE_SOURCE_DIR}/Runtime/Engine
    ${CMAKE_SOURCE_DIR}/Runtime/Math
    ${CMAKE_SOURCE_DIR}/Runtime/Rendering
    ${CMAKE_SOURCE_DIR}/Project/Game
    ${CMAKE_SOURCE_DIR}/Project/Editor
    ${CMAKE_SOURCE_DIR}/Project/Launcher
)

target_compile_definitions(NullusUnitTests PRIVATE
    NLS_ROOT_DIR="${CMAKE_SOURCE_DIR}"
    NLS_BUILD_DIR="${CMAKE_BINARY_DIR}"
)

target_link_libraries(NullusUnitTests PRIVATE
    gtest
    NLS_Base
    NLS_Core
    NLS_Math
    NLS_Render
    NLS_Engine
)

set_target_properties(NullusUnitTests PROPERTIES
    FOLDER Tests
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

add_test(NAME NullusUnitTests COMMAND NullusUnitTests)
