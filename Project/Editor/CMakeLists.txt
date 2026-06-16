# 收集头文件和源文件
###############################################################################
file(GLOB_RECURSE EDITOR_HEAD_FILES
  ${CMAKE_CURRENT_SOURCE_DIR}/*.h
  ${CMAKE_CURRENT_SOURCE_DIR}/*.inl
)
file(GLOB_RECURSE EDITOR_SOURCE_FILES
  ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/*.c
)

list(FILTER EDITOR_HEAD_FILES EXCLUDE REGEX "/Gen/")
list(FILTER EDITOR_SOURCE_FILES EXCLUDE REGEX "/Gen/")
nls_split_project_entry_sources(EDITOR_ENTRY_SOURCE EDITOR_SOURCE_FILES ${EDITOR_SOURCE_FILES})

# 对文件进行分组
###############################################################################
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${EDITOR_HEAD_FILES})
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${EDITOR_SOURCE_FILES})
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${EDITOR_ENTRY_SOURCE})

add_library(EditorProject STATIC ${EDITOR_HEAD_FILES} ${EDITOR_SOURCE_FILES})
set_target_properties(EditorProject PROPERTIES
  FOLDER "Project"
)

target_include_directories(
  EditorProject
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<BUILD_INTERFACE:${NLS_ROOT_DIR}>
)

target_link_libraries(
  EditorProject
  PUBLIC
    NLS_Engine
  PRIVATE
    DirectXTex::DirectXTex
)

nls_target_precompile_headers(EditorProject "${NLS_ROOT_DIR}/Project/NullusProjectPch.h")

if(APPLE)
  target_link_libraries(EditorProject PUBLIC "-framework CoreServices")
endif()

if(WIN32)
  target_link_libraries(EditorProject PUBLIC Comctl32)
endif()

nls_add_meta_generation(EditorProject "${CMAKE_CURRENT_SOURCE_DIR}" "${EDITOR_HEAD_FILES}")

add_executable(Editor ${EDITOR_ENTRY_SOURCE})
nls_add_windows_app_icon(Editor)

# 指定输出路径
###############################################################################
set_target_properties(Editor PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${NLS_APP_OUTPUT_PATH}"
)

# 设置包含路径
###############################################################################
target_include_directories(
  Editor
  PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)

target_link_libraries(
  Editor
  PRIVATE
    EditorProject
)

nls_target_precompile_headers(Editor "${NLS_ROOT_DIR}/Project/NullusProjectPch.h")
