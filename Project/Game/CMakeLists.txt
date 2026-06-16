# 收集头文件和源文件
###############################################################################
file(GLOB_RECURSE GAME_HEAD_FILES
  ${CMAKE_CURRENT_SOURCE_DIR}/*.h
  ${CMAKE_CURRENT_SOURCE_DIR}/*.inl
)
file(GLOB_RECURSE GAME_SOURCE_FILES
  ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/*.c
)
list(FILTER GAME_HEAD_FILES EXCLUDE REGEX "/Gen/")
list(FILTER GAME_SOURCE_FILES EXCLUDE REGEX "/Gen/")
nls_split_project_entry_sources(GAME_ENTRY_SOURCE GAME_SOURCE_FILES ${GAME_SOURCE_FILES})

# 对文件进行分组
###############################################################################
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${GAME_HEAD_FILES})
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${GAME_SOURCE_FILES})
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${GAME_ENTRY_SOURCE})

add_library(GameProject STATIC ${GAME_SOURCE_FILES} ${GAME_HEAD_FILES})
set_target_properties(GameProject PROPERTIES
  FOLDER "Project"
)

target_include_directories(
  GameProject
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)

target_link_libraries(
  GameProject
  PUBLIC
    NLS_Engine
)

nls_target_precompile_headers(GameProject "${NLS_ROOT_DIR}/Project/NullusProjectPch.h")

add_executable(Game ${GAME_ENTRY_SOURCE})
nls_add_windows_app_icon(Game)

# 指定输出路径
###############################################################################
set_target_properties(Game PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${NLS_APP_OUTPUT_PATH}"
)

target_include_directories(
  Game
  PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)

target_link_libraries(
  Game
  PRIVATE
    GameProject
)

nls_target_precompile_headers(Game "${NLS_ROOT_DIR}/Project/NullusProjectPch.h")
