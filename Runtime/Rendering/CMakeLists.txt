nls_add_runtime_target(
  NAME NLS_Render
  SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}
  APPEND_INCLUDE_DIR
  ${NLS_ThirdParty_DIR}
  ${NLS_ThirdParty_DIR}/ImGui
  LIBS
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
  if(NOT WIN32)
    list(FILTER _nls_render_sources EXCLUDE REGEX "[/\\\\]RHI[/\\\\]Backends[/\\\\]DX11[/\\\\]")
    list(FILTER _nls_render_sources EXCLUDE REGEX "[/\\\\]RHI[/\\\\]Backends[/\\\\]DX12[/\\\\]")
  endif()

  if(NOT Vulkan_FOUND)
    list(FILTER _nls_render_sources EXCLUDE REGEX "[/\\\\]RHI[/\\\\]Backends[/\\\\]Vulkan[/\\\\]")
  endif()

  if(NOT APPLE)
    list(FILTER _nls_render_sources EXCLUDE REGEX "[/\\\\]RHI[/\\\\]Backends[/\\\\]Metal[/\\\\]")
  endif()

  if(APPLE)
    set(_nls_objcxx_sources ${_nls_render_sources})
    list(FILTER _nls_objcxx_sources INCLUDE REGEX "([/\\\\]RHI[/\\\\]Backends[/\\\\]Metal[/\\\\].*\\.cpp$)|([/\\\\]RHI[/\\\\]Backends[/\\\\]RHIDeviceFactory\\.cpp$)")
    if(_nls_objcxx_sources)
      set_source_files_properties(${_nls_objcxx_sources} PROPERTIES LANGUAGE OBJCXX)
    endif()
  endif()

  set_property(TARGET NLS_Render PROPERTY SOURCES "${_nls_render_sources}")
endif()

if(Vulkan_FOUND)
  target_link_libraries(NLS_Render PUBLIC Vulkan::Vulkan)
  target_compile_definitions(NLS_Render PUBLIC NLS_HAS_VULKAN=1)
else()
  target_compile_definitions(NLS_Render PUBLIC NLS_HAS_VULKAN=0)
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
  )
