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

# Group files in IDEs
###############################################################################
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${LAUNCHER_HEAD_FILES})
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${LAUNCHER_SOURCE_FILES})

add_executable(Launcher ${LAUNCHER_SOURCE_FILES} ${LAUNCHER_HEAD_FILES})

# Configure output path
###############################################################################
set_target_properties(Launcher PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${NLS_APP_OUTPUT_PATH}"
)

# Configure include paths
###############################################################################
target_include_directories(
  Launcher
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)

target_link_libraries(
  Launcher
    PUBLIC
    NLS_Engine
)
