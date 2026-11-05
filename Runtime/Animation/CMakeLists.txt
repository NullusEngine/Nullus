nls_add_runtime_target(
  NAME NLS_Animation
  SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}
  LIBS NLS_Core NLS_Math
)
target_compile_options(NLS_Animation PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/Z7>)
