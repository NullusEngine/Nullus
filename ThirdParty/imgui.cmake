set(imgui_SOURCE_DIR_ ${CMAKE_CURRENT_SOURCE_DIR}/ImGui)
    
file(GLOB imgui_sources CONFIGURE_DEPENDS  "${imgui_SOURCE_DIR_}/*.cpp")
list(APPEND imgui_sources
    "${imgui_SOURCE_DIR_}/misc/cpp/imgui_stdlib.cpp"
)
file(GLOB imgui_impl CONFIGURE_DEPENDS  
"${imgui_SOURCE_DIR_}/backends/imgui_impl_glfw.cpp" 
"${imgui_SOURCE_DIR_}/backends/imgui_impl_glfw.h"
"${imgui_SOURCE_DIR_}/backends/imgui_impl_opengl3.cpp" 
"${imgui_SOURCE_DIR_}/backends/imgui_impl_opengl3.h"
"${imgui_SOURCE_DIR_}/backends/imgui_impl_opengl3_loader.h")
if(WIN32)
list(APPEND imgui_impl
    "${imgui_SOURCE_DIR_}/backends/imgui_impl_dx12.cpp"
    "${imgui_SOURCE_DIR_}/backends/imgui_impl_dx12.h"
    "${imgui_SOURCE_DIR_}/backends/imgui_impl_dx11.cpp"
    "${imgui_SOURCE_DIR_}/backends/imgui_impl_dx11.h"
)
endif()

find_package(Vulkan QUIET)
if(Vulkan_FOUND)
list(APPEND imgui_impl
    "${imgui_SOURCE_DIR_}/backends/imgui_impl_vulkan.cpp"
    "${imgui_SOURCE_DIR_}/backends/imgui_impl_vulkan.h"
)
endif()
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
    $<BUILD_INTERFACE:${imgui_SOURCE_DIR_}>
    $<BUILD_INTERFACE:${imgui_SOURCE_DIR_}/misc/cpp>
)

target_link_libraries(
    ImGui
    PRIVATE # 使用该库时必须依赖的库
    glfw
)

if(WIN32)
    target_link_libraries(ImGui PRIVATE d3d12 dxgi dxguid d3d11)
    target_compile_definitions(ImGui PUBLIC NLS_HAS_IMGUI_DX12_BACKEND=1 NLS_HAS_IMGUI_DX11_BACKEND=1)
else()
    target_compile_definitions(ImGui PUBLIC NLS_HAS_IMGUI_DX12_BACKEND=0 NLS_HAS_IMGUI_DX11_BACKEND=0)
endif()

if(Vulkan_FOUND)
    target_link_libraries(ImGui PRIVATE Vulkan::Vulkan)
    target_compile_definitions(ImGui PUBLIC NLS_HAS_IMGUI_VULKAN_BACKEND=1)
else()
    target_compile_definitions(ImGui PUBLIC NLS_HAS_IMGUI_VULKAN_BACKEND=0)
endif()
