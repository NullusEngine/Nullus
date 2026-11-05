nls_add_runtime_target(
    NAME NLS_Input
    SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}
    NO_REFLECTION
    LIBS NLS_Base NLS_Math
)
# The input kernel can be built and tested without windowing, assets or UI.
set_target_properties(NLS_Input PROPERTIES DISABLE_PRECOMPILE_HEADERS ON)
target_compile_options(NLS_Input PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/Z7>)
