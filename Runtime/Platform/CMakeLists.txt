
IF (WIN32)
	set(GLFWPATH ${NLS_ThirdParty_DIR}/glfw/Windows)
ELSEIF (UNIX)
  set(GLFWPATH ${NLS_ThirdParty_DIR}/glfw/Linux)
ENDIF ()
find_package(glfw3 3.4 REQUIRED PATHS ${GLFWPATH})
nls_add_runtime_target(
  NAME NLS_Platform
  SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}
  LIBS
    NLS_Base
    NLS_Core
    NLS_Math
    NLS_Render
    glfw
  )
