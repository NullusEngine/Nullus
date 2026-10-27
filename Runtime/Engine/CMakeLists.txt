
nls_add_runtime_target(
  NAME NLS_Engine
  SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}
  LIBS
    NLS_Core
    NLS_UI
    NLS_VFX
    NLS_Physics
  )

target_compile_definitions(NLS_Engine PUBLIC
  $<$<BOOL:${NLS_ENABLE_TEST_HOOKS}>:NLS_ENABLE_TEST_HOOKS=1>
  $<$<BOOL:${NLS_ENABLE_GAME_UI}>:NLS_ENABLE_GAME_UI=1>
)
