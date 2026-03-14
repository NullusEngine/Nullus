cmake_minimum_required(VERSION 3.18)

project(ReflectionTest LANGUAGES CXX)

add_subdirectory(src)

# 链接到 Base 库
target_link_libraries(ReflectionTest PRIVATE
    NLS_Base
    NLS_Core
    NLS_Engine
)
