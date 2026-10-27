# 收集头文件和源文件
###############################################################################
file(GLOB_RECURSE GAME_HEAD_FILES CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/*.h
  ${CMAKE_CURRENT_SOURCE_DIR}/*.inl
)
file(GLOB_RECURSE GAME_SOURCE_FILES CONFIGURE_DEPENDS
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
    NLS_Scripting
)

target_compile_definitions(GameProject PUBLIC
  $<$<BOOL:${NLS_ENABLE_GAME_UI}>:NLS_ENABLE_GAME_UI=1>
)

nls_target_precompile_headers(GameProject "${NLS_ROOT_DIR}/Project/NullusProjectPch.h")

add_executable(Game ${GAME_ENTRY_SOURCE})
nls_add_windows_app_icon(Game)
nls_copy_editor_resources_to_target(Game)

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
nls_copy_shared_runtime_to_target(Game)

# The player template carries the managed assemblies produced by the shared
# scripting build. Shipping validation rejects C# projects when this payload
# is absent, so keep it next to Game.exe instead of relying on an editor cache.
if(NLS_ENABLE_CSHARP_SCRIPTING)
  set(_nls_managed_player_dir "${CMAKE_BINARY_DIR}/Managed/$<CONFIG>")
  add_custom_command(TARGET Game POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
      "$<TARGET_FILE_DIR:Game>/Scripts"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${_nls_managed_player_dir}/GameScripts.dll"
      "${_nls_managed_player_dir}/GameScripts.deps.json"
      "${_nls_managed_player_dir}/GameScripts.runtimeconfig.json"
      "${_nls_managed_player_dir}/EngineScripts.dll"
      "${_nls_managed_player_dir}/EngineScripts.deps.json"
      "${_nls_managed_player_dir}/EngineScripts.runtimeconfig.json"
      "${_nls_managed_player_dir}/Nullus.Managed.dll"
      "$<TARGET_FILE_DIR:Game>/Scripts"
    VERBATIM
    COMMENT "Copying managed player payload for Game")
endif()

nls_target_precompile_headers(Game "${NLS_ROOT_DIR}/Project/NullusProjectPch.h")
