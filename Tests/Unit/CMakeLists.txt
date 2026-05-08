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

set(NULLUS_RENDER_ALIGNMENT_CONTRACT_TEST_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/UE5RenderArchitectureContractTests.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/EditorRenderPathContractTests.cpp"
)

set(NULLUS_EDITOR_META_GENERATED_CPP
    "${CMAKE_SOURCE_DIR}/Project/Editor/Gen/MetaGenerated.cpp"
)
set(NULLUS_EDITOR_META_GENERATED_H
    "${CMAKE_SOURCE_DIR}/Project/Editor/Gen/MetaGenerated.h"
)
set_source_files_properties(
    "${NULLUS_EDITOR_META_GENERATED_CPP}"
    "${NULLUS_EDITOR_META_GENERATED_H}"
    PROPERTIES GENERATED TRUE
)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${NULLUS_UNIT_TEST_FILES})
source_group("Contract Tests" FILES ${NULLUS_RENDER_ALIGNMENT_CONTRACT_TEST_FILES})

add_executable(NullusUnitTests
    ${NULLUS_UNIT_TEST_FILES}
    ${NULLUS_RENDER_ALIGNMENT_CONTRACT_TEST_FILES}
)

target_sources(NullusUnitTests PRIVATE
    ${CMAKE_SOURCE_DIR}/Project/Editor/Panels/ReflectedPropertyDrawer.cpp
    ${CMAKE_SOURCE_DIR}/Project/Editor/Panels/ReflectedPropertyDrawer.h
    ${CMAKE_SOURCE_DIR}/Project/Editor/Panels/ProjectSettings.cpp
    ${CMAKE_SOURCE_DIR}/Project/Editor/Panels/ProjectSettings.h
    ${CMAKE_SOURCE_DIR}/Project/Editor/Settings/EditorSettings.cpp
    ${CMAKE_SOURCE_DIR}/Project/Editor/Settings/EditorSettings.h
    ${CMAKE_SOURCE_DIR}/Project/Editor/Settings/EditorSettingObject.h
    ${CMAKE_SOURCE_DIR}/Project/Editor/Settings/EditorSettingsPersistence.cpp
    ${CMAKE_SOURCE_DIR}/Project/Editor/Settings/EditorSettingsPersistence.h
    ${CMAKE_SOURCE_DIR}/Project/Editor/Settings/EditorSettingsRegistry.cpp
    ${CMAKE_SOURCE_DIR}/Project/Editor/Settings/EditorSettingsRegistry.h
    ${NULLUS_EDITOR_META_GENERATED_CPP}
    ${NULLUS_EDITOR_META_GENERATED_H}
    ${CMAKE_SOURCE_DIR}/Project/Editor/Shortcuts/EditorShortcutProfile.cpp
    ${CMAKE_SOURCE_DIR}/Project/Editor/Shortcuts/EditorShortcutProfile.h
    ${CMAKE_SOURCE_DIR}/Project/Editor/Shortcuts/EditorShortcutRegistry.cpp
    ${CMAKE_SOURCE_DIR}/Project/Editor/Shortcuts/EditorShortcutRegistry.h
    ${CMAKE_SOURCE_DIR}/Project/Editor/Shortcuts/EditorShortcutResolver.cpp
    ${CMAKE_SOURCE_DIR}/Project/Editor/Shortcuts/EditorShortcutResolver.h
    ${CMAKE_SOURCE_DIR}/Project/Editor/Shortcuts/EditorShortcutService.cpp
    ${CMAKE_SOURCE_DIR}/Project/Editor/Shortcuts/EditorShortcutService.h
    ${CMAKE_SOURCE_DIR}/Project/Editor/Core/SceneCameraFocus.cpp
    ${CMAKE_SOURCE_DIR}/Project/Editor/Core/SceneCameraFocus.h
    ${CMAKE_SOURCE_DIR}/Project/Editor/Core/SceneViewImGuizmo.cpp
    ${CMAKE_SOURCE_DIR}/Project/Editor/Core/SceneViewImGuizmo.h
    ${CMAKE_SOURCE_DIR}/Project/Editor/Core/AssetFileWatcher.cpp
    ${CMAKE_SOURCE_DIR}/Project/Editor/Core/AssetFileWatcher.h
    ${CMAKE_SOURCE_DIR}/Project/Editor/Panels/ComponentSearchPanel.cpp
    ${CMAKE_SOURCE_DIR}/Project/Editor/Panels/ComponentSearchPanel.h
    ${CMAKE_SOURCE_DIR}/Project/Editor/Core/EditorLaunchArgs.cpp
    ${CMAKE_SOURCE_DIR}/Project/Editor/Core/EditorLaunchArgs.h
    ${CMAKE_SOURCE_DIR}/Project/Editor/Panels/Console.cpp
    ${CMAKE_SOURCE_DIR}/Project/Editor/Panels/Console.h
    ${CMAKE_SOURCE_DIR}/Project/Editor/Panels/FrameInfoRendererStats.cpp
    ${CMAKE_SOURCE_DIR}/Project/Editor/Panels/FrameInfo.h
    ${CMAKE_SOURCE_DIR}/Project/Editor/Panels/ProfilerPanel.cpp
    ${CMAKE_SOURCE_DIR}/Project/Editor/Panels/ProfilerPanel.h
    ${CMAKE_SOURCE_DIR}/Project/Editor/Panels/ShortcutSettingsPanel.cpp
    ${CMAKE_SOURCE_DIR}/Project/Editor/Panels/ShortcutSettingsPanel.h
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
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Project/Editor/Gen
    ${CMAKE_SOURCE_DIR}/Project/Game
    ${CMAKE_SOURCE_DIR}/Project/Editor
    ${CMAKE_SOURCE_DIR}/Project/Launcher
)

target_compile_definitions(NullusUnitTests PRIVATE
    NLS_ROOT_DIR="${CMAKE_SOURCE_DIR}"
    NLS_BUILD_DIR="${CMAKE_BINARY_DIR}"
    WITH_PROFILING=$<AND:$<BOOL:${WIN32}>,$<BOOL:${NLS_ENABLE_TIMELINE_PROFILER}>>
)

target_link_libraries(NullusUnitTests PRIVATE
    gtest
    NLS_Base
    NLS_Core
    NLS_Math
    NLS_Render
    NLS_UI
    NLS_Engine
)

if(APPLE)
    target_link_libraries(NullusUnitTests PRIVATE "-framework CoreServices")
endif()

if(TARGET Editor_MetaGenerated)
    add_dependencies(NullusUnitTests Editor_MetaGenerated)
endif()

set_target_properties(NullusUnitTests PROPERTIES
    FOLDER Tests
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
nls_copy_shared_runtime_to_target(NullusUnitTests)

add_test(NAME NullusUnitTests COMMAND NullusUnitTests)
