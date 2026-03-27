file(GLOB_RECURSE REFLECTION_TEST_SOURCE_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.inl"
)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${REFLECTION_TEST_SOURCE_FILES})

add_executable(ReflectionTest ${REFLECTION_TEST_SOURCE_FILES})

target_compile_features(ReflectionTest PRIVATE cxx_std_20)

# 设置包含路径
target_include_directories(ReflectionTest PRIVATE
    ${CMAKE_SOURCE_DIR}/Runtime
    ${CMAKE_SOURCE_DIR}/Runtime/Base
    ${CMAKE_SOURCE_DIR}/Runtime/Base/Reflection
)

set_target_properties(ReflectionTest PROPERTIES
    FOLDER Tools
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
