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
  set(_nls_static_mesh_build_sources
    "${CMAKE_CURRENT_SOURCE_DIR}/Assets/MeshReduction.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Assets/StaticMeshBuilder.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Assets/StaticMeshCookBuilder.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Assets/StaticMeshLODSettings.cpp")
  list(REMOVE_ITEM _nls_render_sources ${_nls_static_mesh_build_sources})
  list(FILTER _nls_render_sources EXCLUDE REGEX "[/\\\\]RHI[/\\\\]Backends[/\\\\]DX11[/\\\\]")
  list(FILTER _nls_render_sources EXCLUDE REGEX "[/\\\\]RHI[/\\\\]Backends[/\\\\]OpenGL[/\\\\]")
  list(FILTER _nls_render_sources EXCLUDE REGEX "[/\\\\]RHI[/\\\\]Backends[/\\\\]Vulkan[/\\\\]")
  list(FILTER _nls_render_sources EXCLUDE REGEX "[/\\\\]RHI[/\\\\]Backends[/\\\\]Metal[/\\\\]")
  list(FILTER _nls_render_sources EXCLUDE REGEX "[/\\\\]RHI[/\\\\]Backends[/\\\\]DX12[/\\\\]DX12UIBridge\\.cpp$")
  list(FILTER _nls_render_sources EXCLUDE REGEX "[/\\\\]Backend[/\\\\]OpenGL[/\\\\]")
  list(FILTER _nls_render_sources EXCLUDE REGEX "[/\\\\]glad[/\\\\]")
  list(FILTER _nls_render_sources EXCLUDE REGEX "[/\\\\]KHR[/\\\\]")

  if(NOT WIN32)
    list(FILTER _nls_render_sources EXCLUDE REGEX "[/\\\\]RHI[/\\\\]Backends[/\\\\]DX12[/\\\\]")
  endif()

  set_property(TARGET NLS_Render PROPERTY SOURCES "${_nls_render_sources}")
endif()

add_library(NLS_StaticMeshBuild STATIC ${_nls_static_mesh_build_sources})
target_compile_features(NLS_StaticMeshBuild PUBLIC cxx_std_20)
target_include_directories(NLS_StaticMeshBuild PUBLIC
  $<BUILD_INTERFACE:${NLS_ROOT_DIR}/Runtime>
  $<BUILD_INTERFACE:${NLS_ROOT_DIR}/Runtime/Rendering>)
target_link_libraries(NLS_StaticMeshBuild PUBLIC NLS_Render PRIVATE meshoptimizer)
set_target_properties(NLS_StaticMeshBuild PROPERTIES FOLDER "Runtime/Build")

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

target_compile_definitions(NLS_Render PRIVATE NLS_RESOURCE_MANAGEMENT_EXPORT)
target_compile_definitions(NLS_Render PRIVATE
  NLS_DEVELOPMENT_ENGINE_ASSETS_ROOT="${NLS_ROOT_DIR}/App/Assets/Engine"
)
target_compile_definitions(NLS_Render PUBLIC
  NLS_HAS_ASSIMP_FBX_IMPORTER=$<OR:$<BOOL:${NLS_ENABLE_ASSIMP_FBX_IMPORTER}>,$<BOOL:${NLS_ASSIMP_BUILD_ALL_FORMATS}>>
)
target_compile_definitions(NLS_Render PUBLIC
  $<$<BOOL:${NLS_ENABLE_TEST_HOOKS}>:NLS_ENABLE_TEST_HOOKS=1>
)
