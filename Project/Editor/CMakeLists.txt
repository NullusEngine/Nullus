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

# 对文件进行分组
###############################################################################
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${EDITOR_HEAD_FILES})
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${EDITOR_SOURCE_FILES})

add_executable(Editor ${EDITOR_HEAD_FILES} ${EDITOR_SOURCE_FILES})

# 指定输出路径
###############################################################################
set_target_properties(Editor PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${NLS_APP_OUTPUT_PATH}"
)

# 设置包含路径
###############################################################################
target_include_directories(
  Editor
  PUBLIC # 其他项目使用头文件时所需的目录
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)

target_link_libraries(
  Editor
    PUBLIC # 使用该库时必须依赖的库
    NLS_Engine
)
