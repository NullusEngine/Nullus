nls_add_runtime_target(
  NAME NLS_AnimationCompiler
  NO_REFLECTION
  SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}
  LIBS NLS_Animation
)
target_compile_options(NLS_AnimationCompiler PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/Z7>)
