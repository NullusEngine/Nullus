nls_add_runtime_target(
  NAME NLS_VFX
  NO_REFLECTION
  SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}
  LIBS NLS_Core NLS_Math NLS_Physics NLS_Render
)
