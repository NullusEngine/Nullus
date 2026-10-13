# Tracy Thread Names (2026-10-13)

Render, TaskGraph and Jobs workers already called `NLS_PROFILE_REGISTER_THREAD`,
but `Profiler::RegisterThread` only updated engine TLS. The Tracy destination
never forwarded the names to Tracy, leaving Windows thread tracks unnamed.

Registration now forwards the name through `TracyProfiler::RegisterThread` to
`tracy::SetThreadName` before profiling is enabled or a capture connects. Tracy
copies the name and also sets the native thread description. Duplicate names are
not registered repeatedly, and empty registration does not erase an existing name.
An unnamed thread that emits a CPU or GPU scope receives a stable `Thread <id>`
fallback. Scope collection and metadata registration remain separate.

Explicit startup names now cover asset metadata/import workers, startup cache
analysis, prefab dependency preparation, shader compile/reflection/output workers,
the asset file watcher, managed script builds, the Editor debug endpoint, the
progress dialog and the profiler trace writer. Existing Render, Dedicated RHI,
TaskGraph, foreground Jobs and background Jobs names use the same registration.
Tasks-mode RHI work retains the physical TaskGraph worker name.

Release and RelWithDebInfo Editor, Game and Launcher builds pass. The profiler,
TaskGraph, shader and file-watcher regression has 126 passing tests; Jobs and
rendering-thread context tests add 172 passing tests. No failures were reported.
Deployment hashes match the linked executables and built runtime libraries.

The actual DX12 Editor capture is
`build/validation/tracy-thread-names-20261013/20261013-112355/capture.tracy`.
Tracy 0.13.3's own server reader reports 17 thread tracks and zero empty names:
Main thread, Render Thread, TaskGraph Worker 0-11, Background Job Worker 1-2 and
Tracy Symbol Worker. `thread-names.csv` beside the capture records the names,
native thread IDs and zone counts. Idle threads without zones need not appear in
Tracy's CPU timeline even though their startup names are registered.

Restart the Editor and record a new capture to see the names. Existing capture
files retain the metadata recorded when they were created.
