nls_add_runtime_target(
  NAME NLS_Platform
  SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}
  LIBS
    NLS_Input
    NLS_Base
    NLS_Math
    glfw
  )
if(WIN32)
  target_link_libraries(NLS_Platform PRIVATE imm32)
endif()
