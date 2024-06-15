nls_add_runtime_target(
  NAME NLS_Core
  SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}
  LIBS
    NLS_Render
    ImGui
  )
