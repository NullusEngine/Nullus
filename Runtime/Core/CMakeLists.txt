nls_add_runtime_target(
  NAME NLS_Core
  SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}
  LIBS
    NLS_Base
  )

get_target_property(_nls_core_sources NLS_Core SOURCES)
if(_nls_core_sources)
  list(FILTER _nls_core_sources EXCLUDE REGEX "[/\\\\]ResourceManagement[/\\\\](MaterialManager|ModelManager|ShaderManager|TextureManager)\\.cpp$")
  set_property(TARGET NLS_Core PROPERTY SOURCES "${_nls_core_sources}")
endif()
