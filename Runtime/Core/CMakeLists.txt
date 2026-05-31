nls_add_runtime_target(
  NAME NLS_Core
  SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}
  LIBS
    NLS_Base
  )

get_target_property(_nls_core_sources NLS_Core SOURCES)
if(_nls_core_sources)
  list(FILTER _nls_core_sources EXCLUDE REGEX "[/\\\\]ResourceManagement[/\\\\](MaterialManager|MeshManager|ShaderManager|TextureManager)\\.cpp$")
  set_property(TARGET NLS_Core PROPERTY SOURCES "${_nls_core_sources}")
endif()

target_compile_definitions(NLS_Core PUBLIC
  $<$<BOOL:${NLS_ENABLE_TEST_HOOKS}>:NLS_ENABLE_TEST_HOOKS=1>
)
