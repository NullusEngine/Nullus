
if(NLS_RENDER_API STREQUAL "OpenGL")
  nls_add_runtime_target(
    NAME NLS_Engine
    SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}
    LIBS
      NLS_Math
      OpenGL_Rendering
    )
elseif(NLS_RENDER_API STREQUAL "Vulkan")
  nls_add_runtime_target(
    NAME NLS_Engine
    SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}
    LIBS
      NLS_Math
      Vulkan_Rendering
    )
else()
    message(FATAL_ERROR "NLS_RENDER_API is not supported. Aborting.")
endif()
