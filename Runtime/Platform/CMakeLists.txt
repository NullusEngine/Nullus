find_package(glfw3 3.4 REQUIRED PATHS ${NLS_ThirdParty_DIR}/glfw)
nls_add_runtime_target(
  NAME NLS_Platform
  SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}
  LIBS
    NLS_Base
    NLS_Math
    NLS_Render
    glfw
  )
