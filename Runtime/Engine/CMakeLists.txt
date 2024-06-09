
nls_add_runtime_target(
  NAME NLS_Engine
  SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}
  LIBS
    NLS_Math
    NLS_Core
    NLS_Base
    NLS_Render
  )
