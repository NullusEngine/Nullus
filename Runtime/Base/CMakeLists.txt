nls_add_runtime_target(
  NAME NLS_Base
  SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}
  LIBS
    json11
  APPEND_INCLUDE_DIR
    ${NLS_ThirdParty_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}/Generated
)
