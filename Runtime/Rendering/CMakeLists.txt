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
