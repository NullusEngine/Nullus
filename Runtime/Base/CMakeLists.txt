nls_add_runtime_target(
  NAME NLS_Base
  SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}
  LIBS
    TracyClient
  APPEND_INCLUDE_DIR
    ${NLS_ThirdParty_DIR}
)

if(MSVC)
  # Task failures must unwind locks and thread-local rendering scopes even when
  # cached CMAKE_CXX_FLAGS omit the compiler's usual exception-handling flags.
  target_compile_options(NLS_Base PUBLIC "$<$<COMPILE_LANGUAGE:CXX>:/EHsc>")
endif()

target_compile_definitions(NLS_Base PUBLIC
  $<$<BOOL:${NLS_ENABLE_PROFILING}>:NLS_ENABLE_PROFILING=1>
  $<$<BOOL:${NLS_ENABLE_TRACY}>:NLS_ENABLE_TRACY=1>
  $<$<BOOL:${NLS_ENABLE_TIMELINE_PROFILER}>:NLS_ENABLE_TIMELINE_PROFILER=1>
)

target_compile_definitions(NLS_Base PUBLIC
  $<$<BOOL:${NLS_ENABLE_TEST_HOOKS}>:NLS_ENABLE_TEST_HOOKS=1>
)
