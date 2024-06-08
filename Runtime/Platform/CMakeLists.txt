nls_add_runtime_target(
  NAME NLS_Platform
  SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}
  LIBS
    NLS_Base
    NLS_Core
    NLS_Math
    glfw
  )
