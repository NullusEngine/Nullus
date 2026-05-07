nls_add_runtime_target(
  NAME NLS_Render
  SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}
  APPEND_INCLUDE_DIR
  ${NLS_ThirdParty_DIR}
  ${NLS_ThirdParty_DIR}/ImGui
  LIBS
  NLS_Core
  NLS_Platform
  ImGui
  assimp
  fg::FrameGraph
  spirv-cross-core
  spirv-cross-glsl
  spirv-cross-util
  )

get_target_property(_nls_render_sources NLS_Render SOURCES)
if(_nls_render_sources)
  list(FILTER _nls_render_sources EXCLUDE REGEX "[/\\\\]RHI[/\\\\]Backends[/\\\\]DX11[/\\\\]")
  list(FILTER _nls_render_sources EXCLUDE REGEX "[/\\\\]RHI[/\\\\]Backends[/\\\\]OpenGL[/\\\\]")
  list(FILTER _nls_render_sources EXCLUDE REGEX "[/\\\\]RHI[/\\\\]Backends[/\\\\]Vulkan[/\\\\]")
  list(FILTER _nls_render_sources EXCLUDE REGEX "[/\\\\]RHI[/\\\\]Backends[/\\\\]Metal[/\\\\]")
  list(FILTER _nls_render_sources EXCLUDE REGEX "[/\\\\]Backend[/\\\\]OpenGL[/\\\\]")
  list(FILTER _nls_render_sources EXCLUDE REGEX "[/\\\\]glad[/\\\\]")
  list(FILTER _nls_render_sources EXCLUDE REGEX "[/\\\\]KHR[/\\\\]")

  if(NOT WIN32)
    list(FILTER _nls_render_sources EXCLUDE REGEX "[/\\\\]RHI[/\\\\]Backends[/\\\\]DX12[/\\\\]")
  endif()

  set_property(TARGET NLS_Render PROPERTY SOURCES "${_nls_render_sources}")
endif()

if(Vulkan_FOUND)
  target_link_libraries(NLS_Render PUBLIC Vulkan::Vulkan)
  target_compile_definitions(NLS_Render PUBLIC NLS_HAS_VULKAN=1)
else()
  target_compile_definitions(NLS_Render PUBLIC NLS_HAS_VULKAN=0)
endif()

if(WIN32)
  target_link_libraries(NLS_Render PUBLIC d3d12 dxgi dxguid d3d11)
endif()

if(APPLE)
  target_compile_definitions(NLS_Render PUBLIC NLS_HAS_METAL=1)
  target_link_libraries(NLS_Render PUBLIC "-framework Metal" "-framework QuartzCore" "-framework Foundation")
else()
  target_compile_definitions(NLS_Render PUBLIC NLS_HAS_METAL=0)
endif()

target_include_directories(
  NLS_Render
  PUBLIC
    $<BUILD_INTERFACE:${NLS_ROOT_DIR}/Runtime/Core>
    $<BUILD_INTERFACE:${NLS_ROOT_DIR}/Runtime/Engine>
  )

target_sources(
  NLS_Render
  PRIVATE
    "${NLS_ROOT_DIR}/Runtime/Core/ResourceManagement/MaterialManager.cpp"
    "${NLS_ROOT_DIR}/Runtime/Core/ResourceManagement/ModelManager.cpp"
    "${NLS_ROOT_DIR}/Runtime/Core/ResourceManagement/ShaderManager.cpp"
    "${NLS_ROOT_DIR}/Runtime/Core/ResourceManagement/TextureManager.cpp"
)

target_compile_definitions(NLS_Render PRIVATE NLS_RESOURCE_MANAGEMENT_EXPORT)
