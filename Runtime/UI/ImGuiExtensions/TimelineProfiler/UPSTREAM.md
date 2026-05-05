# TimelineProfiler Upstream

- Source: https://github.com/simco50/TimelineProfiler
- Imported from the existing Nullus profiler integration copy previously stored
  under `ThirdParty/TimelineProfiler`.
- License: MIT, preserved in `LICENSE`.

## Local Notes

Nullus builds only the profiler sources and embedded font/icon headers needed by
the editor profiler panel. The upstream sample app and bundled Dear ImGui copy
are intentionally excluded.

`NullusTimelineProfilerImGuiCompat.h` adapts the upstream profiler window source
to the Dear ImGui version currently vendored by Nullus.
