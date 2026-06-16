# Collect headers and source files
###############################################################################
file(GLOB_RECURSE LAUNCHER_HEAD_FILES
  ${CMAKE_CURRENT_SOURCE_DIR}/*.h
  ${CMAKE_CURRENT_SOURCE_DIR}/*.inl
)
file(GLOB_RECURSE LAUNCHER_SOURCE_FILES
  ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/*.c
)
list(FILTER LAUNCHER_HEAD_FILES EXCLUDE REGEX "/Gen/")
list(FILTER LAUNCHER_SOURCE_FILES EXCLUDE REGEX "/Gen/")
nls_split_project_entry_sources(LAUNCHER_ENTRY_SOURCE LAUNCHER_SOURCE_FILES ${LAUNCHER_SOURCE_FILES})

# Group files in IDEs
###############################################################################
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${LAUNCHER_HEAD_FILES})
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${LAUNCHER_SOURCE_FILES})
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${LAUNCHER_ENTRY_SOURCE})

add_library(LauncherProject STATIC ${LAUNCHER_SOURCE_FILES} ${LAUNCHER_HEAD_FILES})
set_target_properties(LauncherProject PROPERTIES
  FOLDER "Project"
)

target_include_directories(
  LauncherProject
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)

target_link_libraries(
  LauncherProject
  PUBLIC
    NLS_Engine
)

nls_target_precompile_headers(LauncherProject "${NLS_ROOT_DIR}/Project/NullusProjectPch.h")

add_executable(Launcher ${LAUNCHER_ENTRY_SOURCE})
nls_add_windows_app_icon(Launcher)

# Configure output path
###############################################################################
set_target_properties(Launcher PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${NLS_APP_OUTPUT_PATH}"
)

target_include_directories(
  Launcher
  PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)

target_link_libraries(
  Launcher
  PRIVATE
    LauncherProject
)

nls_target_precompile_headers(Launcher "${NLS_ROOT_DIR}/Project/NullusProjectPch.h")
