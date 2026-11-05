nls_add_runtime_target(
  NAME NLS_Base
  SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}
  LIBS
    TracyClient
  APPEND_INCLUDE_DIR
    ${NLS_ThirdParty_DIR}
)

if(WIN32)
  target_link_libraries(NLS_Base PRIVATE dbghelp NLS_DbgHelpLock)
endif()

# Logging and stack resolution are intentionally self-contained. They also
# remain buildable when a stale shared MSVC precompiled header needs renewal.
set_source_files_properties(
  "${CMAKE_CURRENT_SOURCE_DIR}/Debug/Logger.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/Debug/FileHandler.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/Debug/StackTrace.cpp"
  PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

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
