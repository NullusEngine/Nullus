nls_add_runtime_target(
    NAME NLS_ECS
    SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}
    NO_REFLECTION
    LIBS NLS_Base
)
target_compile_features(NLS_ECS PUBLIC cxx_std_20)
target_compile_definitions(NLS_ECS PUBLIC $<$<BOOL:${NLS_USE_SHARED_LIBRARY}>:NLS_ECS_SHARED>)
set_target_properties(NLS_ECS PROPERTIES DISABLE_PRECOMPILE_HEADERS ON)
target_compile_options(NLS_ECS PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/Z7>)
