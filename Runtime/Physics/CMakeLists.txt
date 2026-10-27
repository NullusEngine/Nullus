nls_add_runtime_target(
  NAME NLS_Physics
  NO_REFLECTION
  SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}
  LIBS NLS_Core NLS_Math
)
target_link_libraries(NLS_Physics PRIVATE NLS_PhysX)
