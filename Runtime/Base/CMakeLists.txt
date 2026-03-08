nls_add_runtime_target(
  NAME NLS_Base
  SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}
  LIBS
    json11
  APPEND_INCLUDE_DIR
    ${NLS_ThirdParty_DIR}
)

set(NLS_META_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/Generated")
set(NLS_META_GEN_CPP "${NLS_META_GEN_DIR}/MetaGenerated.cpp")

add_custom_command(
  OUTPUT "${NLS_META_GEN_CPP}"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${NLS_META_GEN_DIR}"
  COMMAND $<TARGET_FILE:MetaParser>
          --root "${NLS_ROOT_DIR}"
          --out "${NLS_META_GEN_CPP}"
  DEPENDS
          MetaParser
  COMMENT "Generating reflection glue via MetaParser"
  VERBATIM
)

add_custom_target(NLS_MetaParser_Generate DEPENDS "${NLS_META_GEN_CPP}")
add_dependencies(NLS_Base NLS_MetaParser_Generate)

target_sources(NLS_Base PRIVATE "${NLS_META_GEN_CPP}")
