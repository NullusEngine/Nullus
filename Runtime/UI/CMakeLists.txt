nls_add_runtime_target(
  NAME NLS_UI
  SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}
  APPEND_INCLUDE_DIR
    ${NLS_ThirdParty_DIR}
  LIBS
    NLS_Core
    NLS_Render
    ImGui
)
