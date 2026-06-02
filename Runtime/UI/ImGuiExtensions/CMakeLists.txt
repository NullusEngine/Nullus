include("${CMAKE_CURRENT_LIST_DIR}/cmake/RegisterImGuiExtension.cmake")

nls_register_imgui_extension(
  NAME ImGuizmo
)

set(_timeline_profiler_libs)
set(_timeline_profiler_with_profiling 0)
if(WIN32 AND NLS_ENABLE_TIMELINE_PROFILER)
  list(APPEND _timeline_profiler_libs d3d12 dxgi)
  set(_timeline_profiler_with_profiling 1)
endif()

nls_register_imgui_extension(
  NAME TimelineProfiler
  COMPILE_DEFINITIONS
    IMGUI_DEFINE_MATH_OPERATORS
    WITH_PROFILING=${_timeline_profiler_with_profiling}
  COMPILE_OPTIONS
    $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/FI"${CMAKE_CURRENT_LIST_DIR}/TimelineProfiler/NullusTimelineProfilerImGuiCompat.h">
  LIBS
    ${_timeline_profiler_libs}
)
