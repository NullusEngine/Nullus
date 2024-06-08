function(nls_add_target)
    set(options)
    set(oneValueArgs NAME CATEGORY SOURCE_DIR)
    set(multiValueArgs LIBS APPEND_INCLUDE_DIR)

    cmake_parse_arguments(TARGET "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    # 收集头文件和源文件
    file(GLOB_RECURSE HEAD_FILES
      ${TARGET_SOURCE_DIR}/*.h
      ${TARGET_SOURCE_DIR}/*.hpp
      ${TARGET_SOURCE_DIR}/*.inl
    )
    file(GLOB_RECURSE SRC_FILES
      ${TARGET_SOURCE_DIR}/*.cpp
      ${TARGET_SOURCE_DIR}/*.c
    )

    # 对文件进行分组
    source_group(TREE ${TARGET_SOURCE_DIR} FILES ${HEAD_FILES})
    source_group(TREE ${TARGET_SOURCE_DIR} FILES ${SRC_FILES})

    # 创建库
    if(NLS_USE_SHARED_LIBRARY)
      add_library(${TARGET_NAME} SHARED ${HEAD_FILES} ${SRC_FILES})

      # 动态库导出宏
      string(TOUPPER ${TARGET_NAME} NLS_EXPORT_MACRO)
      string(APPEND NLS_EXPORT_MACRO "_EXPORT")

      target_compile_definitions(
        ${TARGET_NAME}
        PRIVATE
          ${NLS_EXPORT_MACRO}
      )
    else()
      add_library(${TARGET_NAME} STATIC ${HEAD_FILES} ${SRC_FILES})
    endif()

    # 指定输出路径
    set_target_properties(${TARGET_NAME} PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${NLS_APP_OUTPUT_PATH}"
    )

    # 指定FOLDER
    set_target_properties(${TARGET_NAME} PROPERTIES FOLDER ${TARGET_CATEGORY})

    # 设置包含路径
    target_include_directories(
      ${TARGET_NAME}
      PUBLIC # 其他项目使用头文件时所需的目录
        $<BUILD_INTERFACE:${TARGET_SOURCE_DIR}>;${TARGET_APPEND_INCLUDE_DIR}
    )

    target_link_libraries(
      ${TARGET_NAME}
      PUBLIC # 使用该库时必须依赖的库
        ${TARGET_LIBS}
    )
endfunction()

function(nls_add_runtime_target)
    set(options)
    set(oneValueArgs NAME SOURCE_DIR)
    set(multiValueArgs LIBS APPEND_INCLUDE_DIR)

    cmake_parse_arguments(TARGET "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    nls_add_target(
        NAME ${TARGET_NAME}
        CATEGORY "Runtime"
        SOURCE_DIR ${TARGET_SOURCE_DIR}
        LIBS
            ${TARGET_LIBS}
        APPEND_INCLUDE_DIR
            ${NLS_ROOT_DIR}/Runtime;${TARGET_APPEND_INCLUDE_DIR}
    )
endfunction()

add_subdirectory("Engine")
add_subdirectory("Math")
add_subdirectory("Platform")
add_subdirectory("Core")
add_subdirectory("Base")
add_subdirectory("Rendering")
