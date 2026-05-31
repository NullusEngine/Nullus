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
  Autodesk::FbxSdk
  assimp
  fg::FrameGraph
  spirv-cross-core
  spirv-cross-glsl
  spirv-cross-util
  )

if(TARGET AutodeskFbxSdkRuntime)
  add_dependencies(NLS_Render AutodeskFbxSdkRuntime)
endif()

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
    "${NLS_ROOT_DIR}/Runtime/Rendering/Assets/MeshArtifact.cpp"
    "${NLS_ROOT_DIR}/Runtime/Rendering/Assets/MeshArtifact.h"
    "${NLS_ROOT_DIR}/Runtime/Rendering/Assets/ShaderArtifact.cpp"
    "${NLS_ROOT_DIR}/Runtime/Rendering/Assets/ShaderArtifact.h"
    "${NLS_ROOT_DIR}/Runtime/Rendering/Assets/TextureBuildSettings.cpp"
    "${NLS_ROOT_DIR}/Runtime/Rendering/Assets/TextureBuildSettings.h"
    "${NLS_ROOT_DIR}/Runtime/Rendering/Assets/TextureArtifact.cpp"
    "${NLS_ROOT_DIR}/Runtime/Rendering/Assets/TextureArtifact.h"
    "${NLS_ROOT_DIR}/Runtime/Rendering/Assets/TextureEncoder.cpp"
    "${NLS_ROOT_DIR}/Runtime/Rendering/Assets/TextureEncoder.h"
    "${NLS_ROOT_DIR}/Runtime/Rendering/Assets/TextureFormatResolver.cpp"
    "${NLS_ROOT_DIR}/Runtime/Rendering/Assets/TextureFormatResolver.h"
    "${NLS_ROOT_DIR}/Runtime/Rendering/Assets/TextureMipGenerator.cpp"
    "${NLS_ROOT_DIR}/Runtime/Rendering/Assets/TextureMipGenerator.h"
    "${NLS_ROOT_DIR}/Runtime/Rendering/Geometry/BoundingSphereUtils.h"
    "${NLS_ROOT_DIR}/Runtime/Core/ResourceManagement/MaterialManager.cpp"
    "${NLS_ROOT_DIR}/Runtime/Core/ResourceManagement/MeshManager.cpp"
    "${NLS_ROOT_DIR}/Runtime/Core/ResourceManagement/ShaderManager.cpp"
    "${NLS_ROOT_DIR}/Runtime/Core/ResourceManagement/TextureManager.cpp"
)

target_compile_definitions(NLS_Render PRIVATE NLS_RESOURCE_MANAGEMENT_EXPORT)
target_compile_definitions(NLS_Render PUBLIC
  NLS_HAS_ASSIMP_FBX_IMPORTER=$<OR:$<BOOL:${NLS_ENABLE_ASSIMP_FBX_IMPORTER}>,$<BOOL:${NLS_ASSIMP_BUILD_ALL_FORMATS}>>
)
target_compile_definitions(NLS_Render PUBLIC
  $<$<BOOL:${NLS_ENABLE_TEST_HOOKS}>:NLS_ENABLE_TEST_HOOKS=1>
)
