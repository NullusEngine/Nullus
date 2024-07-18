set(imgui_SOURCE_DIR_ ${CMAKE_CURRENT_SOURCE_DIR}/ImGui)
    
file(GLOB imgui_sources CONFIGURE_DEPENDS  "${imgui_SOURCE_DIR_}/*.cpp")
file(GLOB imgui_impl CONFIGURE_DEPENDS  
"${imgui_SOURCE_DIR_}/backends/imgui_impl_glfw.cpp" 
"${imgui_SOURCE_DIR_}/backends/imgui_impl_glfw.h"
"${imgui_SOURCE_DIR_}/backends/imgui_impl_opengl3.cpp" 
"${imgui_SOURCE_DIR_}/backends/imgui_impl_opengl3.h"
"${imgui_SOURCE_DIR_}/backends/imgui_impl_opengl3_loader.h")
# 创建库
add_library(ImGui STATIC ${imgui_sources} ${imgui_impl})

# 指定输出路径
set_target_properties(ImGui PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${NLS_APP_OUTPUT_PATH}"
)

# 设置包含路径
target_include_directories(
    ImGui
    PUBLIC # 其他项目使用头文件时所需的目录
    $<BUILD_INTERFACE:${imgui_SOURCE_DIR_}>;
)

target_link_libraries(
    ImGui
    PRIVATE # 使用该库时必须依赖的库
    glfw
)