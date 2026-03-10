add_executable(ReflectionTest main.cpp)

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
