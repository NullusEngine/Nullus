# ECS validation record

## Editor authoring identity and Live Baking milestone (2026-10-29/30)

The current Windows Debug native DLL validation covers **167 distinct ECS tests**:

| Suite | Passed | XML evidence under `tmp/` |
| --- | --- | --- |
| Core/Query/Commands/Systems/Jobs/Archive/Streaming | 87/87 | `ecs-authoring-core-tests.xml` |
| Native Transform | 10/10 | `ecs-authoring-transform-tests.xml` |
| Hybrid/PlaySession/Lua | 15/15 | `ecs-authoring-hybrid-tests.xml` |
| Baking and real Scene identity | 16/16, including 8 new identity cases | `ecs-authoring-identity-tests.xml` |
| Cook/native assets/Npak | 13/13 | `ecs-authoring-assets-tests.xml` |
| Editor native asset import | 3/3 | `ecs-authoring-asset-editor-tests.xml`, suite `ECSAssetEditor` |
| Packaged Player startup | 11/11 | `ecs-authoring-player-tests.xml` |
| Editor Live Baking | 12/12 | `ecs-authoring-editor-tests.xml` |

The asset-editor executable also executes the same 13 asset cases. Those are
duplicates and are not added to 167. Including the earlier independent 25
TaskGraph cases yields **192 distinct ECS/TaskGraph cases**, not a new sanitizer
run of the Editor integration.

An additional **80/80 existing regressions** passed without modifying their
assertions: `SceneObjectGraphSerializationTests.cpp`, `PPtrTests.cpp`,
`HierarchyUndoTests.cpp`, and `SceneObjectUndoTargetTests.cpp`.
Evidence: `ecs-authoring-regression-tests.xml` and the matching log. These are
reported separately from the ECS total.

The new cases exercise actual SceneManager disk save/load and ObjectGraph serialization/instantiation, retained
loaded object/component/document IDs, newly assigned GUIDs, duplicate Prefab
instances, scope cleanup, and persistent Undo target rebinding. The Editor cases
use the real Hierarchy command implementations, open Undo transactions, actual
SceneManager Play sandbox and Undo Play domain, bounded direct-field/resource
revision polling, failure retention, and native AssetDatabaseFacade import after
export. An initial export failure exposed use of absolute paths at an API that
indexes logical Editor asset paths. `ImportBakedSceneAsset` now shares the real
`TryGetEditorAssetPath` conversion between the UI and its integration test.

Engine reflection generation, Engine/Scripting DLLs, EditorProject, Editor.exe,
GameProject, Game.exe and the focused native tests were rebuilt in the isolated
`build/ecs-native-validation` output. The validation project copies retain the
original generated project dependencies and add the actual new source files;
the real CMake target lists are updated in `Tests/ECS/CMakeLists.txt`. Native build
logs are `tmp/ecs-authoring-*-build.log`. The temporary project generator and
build runner are `tmp/generate_ecs_authoring_projects.py` and
`tmp/build_ecs_authoring_targets.py`. Builds use Debug x64, MSVC, `/Z7 /Y- /MP1`,
`/m:1`, disabled project-reference builds and isolated intermediates. Shared
IntDir warnings occur during reference `GetTargetPath` evaluation; those
references are not recursively compiled or cleaned.

The previous isolated Physics DLL lacked six imports used by the current
Engine. Only that isolated DLL was refreshed from the completed root build after
checking every required Engine import and verifying SHA-256 equality. Script
callers were recompiled against the current Scripting import library rather than
adding compatibility shims. `tmp/ecs_asset_import_check.py` reported no missing
intra-snapshot DLL exports after those client rebuilds.

The rebuilt actual Game.exe passed `--help` and `--package-self-test` with exit 0,
and rejected an invalid startup GUID with exit 8. It read the existing native
Npak fixture without loose Assets/Library. Evidence:
`tmp/ecs-player-cli-results.json`. Its child PATH supplies the isolated DLLs;
this is not a complete standalone distribution or GPU acceptance.

The final rebuilt Editor.exe also loaded successfully and returned exit 0 for
`--help`. Evidence: `tmp/ecs-authoring-editor-cli-results.json` and
`tmp/ecs-authoring-editor-exe-build.log`. This verifies the executable's native
link/import path, not opening or interacting with a visible Editor window.

These checks validate CPU behavior and native builds. They do not establish
visible Editor mouse interaction, Scene View selection/rendering, full Prefab
Stage behavior, native ECS render/physics/animation/VFX adapters, C#/Lua ECS APIs,
new integration ASan/TSan/UBSan coverage, or non-Windows acceptance. Live preview
still replaces owned entities after a changed artifact; in-place entity patches
and disk Baking Cache remain open. See `Editor-Baking.md` and `Remaining.md`.

## Phase 1 (2026-10-29)

- Root configure: `cmake -S . -B build` passed.
- Initial root target build: `cmake --build build --config Debug --target NullusECSTests -- /m:1 /v:minimal` failed in existing third-party builds with C1902 (compiler PDB manager mismatch). The initial generic deployment helper unnecessarily pulled all shared runtime producers into this focused target; it has been removed from the ECS test target.
- Independent configure: `cmake -S Tests/ECS/Standalone -B build/ecs-headless` passed.
- Independent build: `cmake --build build/ecs-headless --config Debug --target NullusECSTests -- /m:1 /v:minimal` passed, MSVC 19.44, C++20.
- Executed `build/ecs-headless/Debug/NullusECSTests.exe --gtest_output=xml:tmp/ecs-phase1-tests.xml`: **22/22 passed**, 57 ms reported by GoogleTest.
- This build compiles the actual ECS sources and the actual Base TaskGraph, TaskEvent, Profiler and TracyProfiler implementations; it does not substitute a mock scheduler.
- Logs: `tmp/ecs-configure.log`, `tmp/ecs-phase1-build.log`, `tmp/ecs-headless-phase1-build.log`, `tmp/ecs-phase1-tests.log`.

Root Engine/Editor linking, interactive Play/Scene View rendering, C#/Lua execution,
ASan/UBSan/TSan, macOS and Linux have not been validated by these tests.

## Phase 2

Headless Debug build passed. Executed all **37/37** cumulative tests, 123 ms.
Results: `tmp/ecs-phase2-tests.xml`, `tmp/ecs-phase2-tests.log` and
`tmp/ecs-phase2-build.log`. Coverage adds ordered concurrent command recording,
temporary handle/reset/lifetime errors, playback conflicts, system lifecycle,
phase mapping, dependency cycles, reverse explicit ordering, read/write hazards,
separate resource permissions and post-phase playback. The old GoogleTest macro
implementation uses line-number labels, so exception assertions occupy separate
source lines.

## Phase 3

Headless Debug build passed. Executed **45/45** cumulative tests, 1437 ms.
Results: `tmp/ecs-phase3-tests.xml`, `tmp/ecs-phase3-tests.log` and
`tmp/ecs-phase3-build.log`. Parallel tests verify real worker execution,
overlapping independent read systems, main-thread affinity, serialized resource
writers, nested chunk jobs with one worker, cancellation followed by safe
structural mutation, single-thread debugging and parallel command streams.

Added `TaskGraph::SubmitTracked` to obtain a retained completion token atomically
with submission. Existing `Submit` behavior is preserved. A test covers inline
completion with zero retained terminal records; ECS jobs no longer rely on the
timing-sensitive Submit/GetCompletion pair.

## Core milestone: final independent headless checks

The final headless suite contains **75 tests**: 50 ECS tests plus the existing
25 TaskGraph tests, without double counting those regression cases.

| Build | Result | GoogleTest time | Evidence |
| --- | --- | --- | --- |
| Debug, MSVC x64 | 75/75 passed | 1536 ms | `tmp/ecs-final-debug-tests.xml` |
| Release, MSVC x64 | 75/75 passed | 1443 ms | `tmp/ecs-final-release-tests.xml` |
| MSVC AddressSanitizer, Debug | 75/75 passed | 3438 ms | `tmp/ecs-asan-final-tests.xml` |

These checks include the final change-cursor replacement/cache behavior,
last-successful-system cursors, move-only ownership, batch-construction rollback
and teardown callbacks retaining scheduler identity. World close is reentrant
without creating a replacement scheduler during OnDestroy.

ASan command: configure `Tests/ECS/Standalone` with `-DNLS_ECS_ASAN=ON`, build
`NullusECSTests`, and run it with the MSVC compiler's `bin/Hostx64/x64` directory
on the child process PATH. Initially running without that ASan runtime DLL path
returned `0xC0000135`; after adding it the instrumented tests actually executed.
This is Windows ASan, not UBSan/TSan or an exhaustive leak detector.

The eight parallel tests and the scheduler teardown regression also passed ten
consecutive repetitions (**90 executions**, nine distinct tests). Evidence:
`tmp/ecs-parallel-stress.log`. These repetitions are not additional unique cases.

## Phase 4 native engine integration snapshot

Final native shared-DLL builds and executions passed on 2026-10-29:

| Target | Result | GoogleTest time | Evidence |
| --- | --- | --- | --- |
| NullusECSTests | 50/50 passed | 1428 ms | `tmp/ecs-native-final-core-tests.xml` |
| NullusECSHybridTests | 13/13 passed | 51 ms | `tmp/ecs-native-final-hybrid-tests.xml` |
| NullusECSHybridExample | Exit 0 | Not a test timing | `tmp/ecs-final-hybrid-example.log` |

The 50 native ECS cases repeat the same ECS cases in the 75-case headless suite;
they are not 50 additional distinct cases. Hybrid coverage uses real Scene,
GameObject, RuntimePlaySession, Rigidbody/Rigidbody2D and the enabled Lua backend.
It covers explicit links, one-way authority, stale identity, scene cleanup,
main-thread access, optional enablement, Pause/Step/Stop/restart, configured
initialization timing, zero-time-scale Step and legacy Lua/ECS updating once each.
No Hybrid case is skipped in this result.

Root build required scoped compiler settings `_CL_="/Z7 /Y- /MP1"`, single MSBuild
node, `BuildProjectReferences=false` and `LinkIncremental=false`. These settings
were child-process-only. A stale SpriteRenderer/Scene generated-body line mapping
was refreshed through the existing MetaParser by invalidating only the build
stamp, without editing the source component or generated macros manually.

The restricted build could not overwrite an existing `MetaGenerated.obj`
(`C1083 Permission denied`); approved execution access allowed that build to run.
Current dependencies required rebuilding Render for the existing
`Camera::GetGpuProjectionMatrix` and Physics for the existing 3D/2D
`UpdateDynamics` implementations. No renderer/physics source fix was introduced
by this ECS work.

An independently running NullusPhysicsBenchmark held the original Physics and
Engine DLLs open. It was preserved. Final Physics/ECS/Engine/Scripting DLLs and
test executables were instead linked with
`/p:OutDir=D:/Code/Nullus/build/ecs-native-validation/Debug/`; unchanged dependent
DLLs were copied from the normal runtime directory, excluding those four fresh
DLLs. All final native executions above used that isolated directory. The normal
runtime directory was not overwritten to force release of the occupied files.
Final successful build evidence: `tmp/ecs-native-isolated-build.log`; the earlier
LNK1104 attempts are also retained in that log. No persistent compiler settings,
source workaround, process termination or Git reset was used.

## Runnable examples

Final Release Basics and MassSimulation builds passed, and all three examples
actually executed successfully:

```text
Basics:
Position after one second: 2
Old handle alive: false

MassSimulation, 100000 entities, 100 frames:
mode=single   elapsed_ms=136.349  chunks=245 jobs=0    checksum=99999.9
mode=parallel elapsed_ms=47.1269  chunks=245 jobs=3200 checksum=99999.9

Native Lua Hybrid, three session ticks:
legacy_player_updates=3 ecs_enemy_x=0.6 game_objects=3 entities=1
```

Evidence: `tmp/ecs-final-samples-build.log`, `tmp/ecs-final-basics.log`,
`tmp/ecs-final-mass-single.log`, `tmp/ecs-final-mass-parallel.log` and
`tmp/ecs-final-hybrid-example.log`. The Hybrid example is a real no-window CPU
workflow, not a rendered sample or a Lua ECS scripting API.

## Final Release benchmarks

The final benchmark executable ran sequentially with chunk budgets 8192, 16384
and 32768 bytes. Each run produced 40 successful rows: ten operations at four
scales (1,000 / 10,000 / 100,000 / 1,000,000 entities), with no out-of-memory row.
Checksums and applied-command counts passed. Saved raw data:
[8 KiB](BenchmarkResults-8k.csv), [16 KiB](BenchmarkResults-16k.csv),
[32 KiB](BenchmarkResults-32k.csv). Measurements, environment and limits are
explained in [Performance.md](Performance.md).

## Remaining acceptance

Full Editor linking/visible interaction, all NullusUnitTests, GPU rendering,
native ECS physics actors, C# ECS API, Editor Live Baking/Cook and macOS/Linux acceptance remain
separate open items in Remaining.md.

## Phase 5: final results including the parser fix

The current independent suite has **101 distinct cases**: 66 ECS Core/query/
command/system/parallel/serialization cases, 25 existing TaskGraph cases and ten
native ECS Transform cases. The 16 serialization cases include the final SAX
validator, a 10,000-entity round trip, nested duplicate keys, depth/integer/layout
checks, references, migration, resource validation, atomic files, overrides,
batch rollback and deferred/parallel prefab recording.

| Configuration | Core + TaskGraph | Transform | GoogleTest time |
| --- | --- | --- | --- |
| Debug, standalone MSVC x64 | 91/91 passed | 10/10 passed | 8.173 s + 0.386 s |
| Release, standalone MSVC x64 | 91/91 passed | 10/10 passed | 2.098 s + 0.024 s |
| Windows MSVC ASan, standalone | 91/91 passed | 10/10 passed | 30.331 s + 1.446 s |

Evidence is `tmp/ecs-phase5-final-{debug,release,asan}-{core,transform}.xml` and
the corresponding `.log` files. Final build logs use
`tmp/ecs-phase5-final-{debug,asan}-build.log` and
`tmp/ecs-phase5-parser-release-build.log`. Some executions overlapped other
validation/background tasks; these are correctness checks, not timing budgets.

Final native shared-DLL results:

| Target | Result | Time | XML evidence under tmp/ |
| --- | --- | --- | --- |
| NullusECSTests | 66/66 passed | 7.032 s | ecs-phase5-final-native-NullusECSTests.xml |
| NullusECSTransformTests | 10/10 passed | 0.290 s | ecs-phase5-final-native-NullusECSTransformTests.xml |
| NullusECSBakingTests | 8/8 passed | 0.103 s | ecs-phase5-final-native-NullusECSBakingTests.xml |
| NullusECSHybridTests | 13/13 passed | 0.078 s | ecs-phase5-final-native-NullusECSHybridTests.xml |

Core/Transform native cases overlap the independent cases; do not add them as
new unique tests. The eight Baking and 13 Hybrid cases exercise actual engine
objects/DLLs and are additional. All reported cases executed without skips.
ASan covers the independent Core/Math/Transform project; it does not instrument
the whole native Engine/Baking/Hybrid DLL dependency tree.

Native build output remains isolated in `build/ecs-native-validation/Debug`.
`tmp/ecs-phase5-native-build.log` records an initial C3878 from Scene.cpp while
that pre-existing work was being edited. A later read found it already updated;
this task did not patch Scene.cpp. `ecs-phase5-native-rebuild.log` successfully
built the libraries and four test executables but reported MSB1009 for the newly
added sample target. Explicit `cmake -S . -B build` then generated that project
(`ecs-phase5-native-configure.log`), and its build passed
(`ecs-phase5-baking-example-build.log`). Final parser/DLL rebuild passed in
`ecs-phase5-final-native-build.log`. Compiler flags and occupied-process handling
remain as described in the Phase 4 snapshot.

The Baking example actually completed after the final DLL rebuild:

```text
full_rebuilt=2 incremental_rebuilt=1 incremental_reused=1
loaded_entities=2 child_world_x=13 file=tmp/ecs-baked-scene-example.json
```

Evidence: `tmp/ecs-phase5-final-baking-example.log`. A copy of the actual loaded
artifact is [BakedScene.example.json](BakedScene.example.json). It contains no
runtime entity handles and is loaded after the authoring Scene is destroyed.
This is the fourth CPU example; the earlier Basics/Mass/Lua evidence above is
retained as the preceding milestone's record. No GPU window was created.

The Release ArchiveBenchmark completed **36/36 successful measurements** at
1k/10k/100k/1m entities, including file read/write, scene creation/unload and batch
prefab generation/destruction. Raw [final results](ArchiveBenchmarkResults.csv)
and [partial pre-fix results](ArchiveBenchmarkBeforeParserFix.csv) are retained.
The two long-running pre-fix benchmark processes started by this task were
stopped after their exact executable path and start time were verified; user
Editor/Physics processes were preserved. A separate completed 100k diagnosis
also confirmed the callback parser's poor scaling. The source-level cause and
final timings are in [Performance](Performance.md). Interrupted million-entity
parse runs have no fabricated completion row.

Actual engine Baking tests cover Full/Incremental work, cache reuse, typed
persistent source identity, deterministic extras, component/object deletion,
resource revision invalidation, failure preserving the last artifact/cache,
duplicate ownership diagnostics, version advancement, input reordering,
main-thread enforcement, Full fallback and Preview/Play isolation. They do not
validate Editor automatic events, Undo/Redo, GPU extraction, Cook/ArtifactDB
registration, disk cache, streaming SubScene orchestration or script ECS APIs.

Full end-to-end task acceptance is still open in Remaining.md. Scoped
`git diff --check` and source/document checks are recorded separately from these
actual builds and executions; they do not substitute for runtime validation.

## JobSystem and asynchronous Scene chunks (2026-10-29)

ECS systems now use the existing Base JobSystem; chunk/hierarchy work uses
`IJobParallelFor` / `ScheduleParallelFor`. The ECS layer owns only access checks,
conflict ordering, scoped joins and phase command playback. It creates no pool
and does not initialize or shut down the shared service. The early TaskGraph
SubmitTracked addition and its regression test remain; ECS no longer calls it.

The headless project compiles the actual JobSystem, JobQueue,
BackgroundJobQueue, JobBatchDispatcher, diagnostics, range and safety sources,
in addition to the existing TaskGraph/Profiler regression implementations.
All three configurations passed **120 distinct tests**: 85 ECS core/query/
command/system/parallel/serialization/streaming, 25 existing TaskGraph tests and
10 native Transform tests. No failures, errors, disabled or skipped cases.

| Configuration | Core + TaskGraph | Transform | GoogleTest seconds |
| --- | ---: | ---: | ---: |
| Debug | 110/110 | 10/10 | 10.263 + 0.713 |
| Release | 110/110 | 10/10 | 1.357 + 0.039 |
| Windows MSVC ASan | 110/110 | 10/10 | 66.934 + 1.731 |

XML evidence: `tmp/ecs-jobs-final-{debug,release,asan}-{core,transform}.xml`.
Build logs: `tmp/ecs-jobs-final-{debug,release,asan}-build.log`; Release's benchmark
missing an explicit `<array>` include was corrected and the affected sample/
benchmark targets rebuilt successfully in `tmp/ecs-jobs-release-examples-build.log`.
Final cancellation handling was rebuilt in `tmp/ecs-jobs-final-{debug,release,asan}-refresh.log`
and all 110 main-suite cases rerun in each configuration. This includes World
cancellation from a resource callback before candidate publication.
An initial Release/ASan attempt caught concurrently edited JobQueue header/source
interfaces out of sync. That work was preserved; the later matching implementation
compiled and passed the results above. ASan uses the compiler runtime PATH
prefix documented in the earlier record.

New tests cover real Jobs scheduling and concurrent readers, serialized writers,
one-worker nested queries, caller-assisted completion, exceptions/cancellation
joining accepted work, main-thread-only execution and pre-submit rejection in a
DisallowJobSyncWaitScope. Phase failure drops recorded structural commands after
joining the other accepted systems.

Fourteen SceneStreaming cases cover background-only parsing, owner-only commit,
local reference isolation between chunks, failed replacement retaining old data,
superseded request identity, unload/cancel without resurrection, destruction while
parsing is queued, bounded commit count/history, resource resolution/cancellation,
foreign handles, closed Worlds, wrong document/limits and World cancellation.
Pump polls published parse state without waiting. Wait is an explicit synchronous
tool path. Parse payloads retain no World, manager, resolver or GameObject.

Standalone streaming sample ran in Debug and Release and validated the output:
`loaded_chunks=2 entities=4 background=1`, followed by `unloaded=2 remaining=2`.
The Release 100k/100-frame MassSimulation passed both modes, with `jobs=600` /
`jobs=0` and validated floating-point positions. Performance is documented
separately from these correctness checks. The new 16 KiB Core benchmark completed
40/40 measurements at 1k/10k/100k/1m with all statuses `ok`.

Native DLL validation passed **118/118** cases with no skips:

| Target | Cases | GoogleTest seconds |
| --- | ---: | ---: |
| NullusECSTests | 85/85 | 12.932 |
| NullusECSTransformTests | 10/10 | 0.740 |
| NullusECSHybridTests | 15/15 | 0.257 |
| NullusECSBakingTests | 8/8 | 0.309 |

Evidence: `tmp/ecs-jobs-final-native-{core,transform,hybrid,baking}.xml` and matching
logs. Core/Transform repeat the same source cases under DLL linking and are not
additional unique tests; the 15 Hybrid/8 Baking cases bring the distinct case
count, including the 25 existing TaskGraph regressions, to 143.

Built native Base/ECS/Engine/Scripting and the focused targets with `/Z7 /Y- /MP1`,
`/m:1`, `BuildProjectReferences=false`, `LinkIncremental=false`, and isolated
`OutDir=D:/Code/Nullus/build/ecs-native-validation/Debug/`. Final small refresh
used `'/t:ClCompile;_Link'` with `PostBuildEventUseInBuild=false` after the normal
build had generated the required metadata. Logs: `tmp/ecs-jobs-native-build.log`,
`tmp/ecs-jobs-final-native-build.log`, `tmp/ecs-jobs-native-refresh.log`.

Other active Editor/2D/Physics builds were inspected read-only and preserved.
The isolated runtime retains this task's rebuilt Base/ECS/Engine/Scripting DLLs.
The matching Physics DLL was copied from the other completed scoped Physics build
at `build/physics-validation/commands/Debug/lib/NLS_Physicsd.dll`, with identical
source/destination hashes; other dependencies come from `build/runtime/Debug`.
No user build or Editor process was stopped and the normal runtime directory was
not overwritten by this task's native builds.

The native streaming example also passed. New Hybrid cases exercise ready chunk
submission through RuntimeWorld, a no-Jobs-wait frame safe point, Stop cleanup
and unloading on legacy Scene replacement. Native Lua Hybrid and Baking examples
were rerun with these DLLs; output is in `tmp/ecs-jobs-final-{hybrid,baking}-example.log`.

Full Editor visible interaction, GPU/subsystem/script ECS integration,
Cook/ArtifactDB, cross-chunk references, TSan/UBSan and macOS/Linux remain outside
this milestone's acceptance.

## Native assets, Cook and packaged content (2026-10-29)

This milestone adds real `.ecsscene` / `.ecsprefab` asset types and native
artifact schema mappings, a shared Engine compiler used by Cook and Editor,
target-specific resource resolution, dependency ordering/invalidation, and
RuntimeWorld loading through the existing RuntimeAssetDatabase/ContentSource.
It reuses the existing Build dependency collector, RuntimeAssetManifest and Npak.
No additional database, package format, worker pool or fake loader is introduced.

The independent suite now contains **122 cases per configuration**: 87 ECS
core/query/command/system/parallel/serialization/streaming, 25 existing TaskGraph
regressions, and 10 Transform cases. All passed with no failures or skips.

| Configuration | Core + TaskGraph | Transform | GoogleTest seconds |
| --- | ---: | ---: | ---: |
| Debug | 112/112 | 10/10 | 5.450 + 0.173 |
| Release | 112/112 | 10/10 | 0.697 + 0.040 |
| Windows MSVC ASan | 112/112 | 10/10 | 29.399 + 2.112 |

Evidence: `tmp/ecs-assets-final-{debug,release,asan}-{core,transform}.xml` and
matching logs. The final main-suite rebuilds are recorded in
`tmp/ecs-assets-final-{debug,release,asan}-parallel-build.log`. The two new
streaming cases validate shared document-source ownership, actual background
execution, propagated archive limits and rejected null sources.

A native rerun exposed a scheduling-dependent test: sufficiently short chunks
could all be drained by one lane, including the completing caller. The test now
holds its first callback until another lane enters, with a two-second timeout
to prevent deadlock if execution regresses to serial. It retains the data,
join and multi-job checks. The affected native test passed 20 consecutive runs
in `tmp/ecs-assets-native-parallel-repeat.log`; the complete suites above were
then rerun. No scheduler behavior was changed to satisfy the test.

The native asset suite passed **13/13** cases in
`tmp/ecs-assets-final-native-assets.xml` (1.278 seconds). It exercises real
texture Cook, cold ECS dependency sorting, unchanged skips, resource-version
invalidation, pinned-version failures retaining prior artifacts, invalid kind,
missing/stale/wrong-target resources, dependency cycles, prefab overrides and
per-instance entity remapping. Runtime tests cover authorization, wrong native
identity/platform/schema, corrupt content, byte limits and resource availability.
The package case uses the real BuildDependencyCollector, writes and reloads a
runtime manifest and Npak, removes its source Assets and Library, mounts packaged
content and asynchronously commits the scene to RuntimeWorld.

`NullusECSAssetPipelineExample` also completed using actual GameObjects and the
Transform Baker, native metadata, Cook, runtime manifest serialization, Npak
and RuntimeWorld. After removing its development Library it printed:

```text
baked_entities=2 cooked_assets=2 packaged_artifacts=2 scene_entities=2 total_entities=6 child_world_x=13 background_parse=1
```

Evidence: `tmp/ecs-assets-final-pipeline-example.log`. The generated package and
manifest remain under the unique `tmp/ecs-asset-example-<guid>/` directory printed
in that log. The example validates both asynchronous scene loading and two
independent prefab instances from packaged content. It opens no GPU window.

Native DLL validation passed **136/136** cases with no failures or skips:

| Target | Cases | GoogleTest seconds |
| --- | ---: | ---: |
| NullusECSTests | 87/87 | 3.377 |
| NullusECSTransformTests | 10/10 | 0.129 |
| NullusECSHybridTests | 15/15 | 0.053 |
| NullusECSBakingTests | 8/8 | 0.046 |
| NullusECSAssetTests | 13/13 | 1.278 |
| NullusECSAssetEditorTests | 3/3 | 0.675 |

Evidence: `tmp/ecs-assets-final-native-{core,transform,hybrid,baking,assets,editor}.xml`
and matching logs; the final integration/sample run is summarized in
`tmp/ecs-assets-final-native-run.log`. Core/Transform repeat independent cases
under DLL linking. Including the 25 existing TaskGraph regressions, there are
**161 distinct cases** across the independent and native suites. Older results
elsewhere in this file are historical milestones, not additional unique tests.

The three Editor cases use the real AssetDatabaseFacade and the project root's
`Assets` mount and `Library`. They validate native import of both types,
persisted-manifest reload and artifact lookup, failed reimport retaining the
last good artifact, and deferred batch imports. The batch case uses
ImportAssetFromCurrentDatabase, proves the prefab manifest is absent from the
on-disk database before flush, and successfully compiles a scene depending on
the cached prefab. These are Editor API tests, not visible UI interaction tests.

Native builds use `/Z7 /Y- /MP1`, `/m:1`, `BuildProjectReferences=false`,
`LinkIncremental=false`, and
`OutDir=D:/Code/Nullus/build/ecs-native-validation/Debug/`. The final Engine build
also uses its own `IntDir` under `build/ecs-native-validation/obj/asset-final-Engine/`;
the final Scripting native build uses the corresponding `asset-final-Scripting/`.
An earlier mixed-intermediate rerun raised an access violation during legacy
Rigidbody destruction. Full recompilation in independent intermediate directories
resolved it; all 15 Hybrid cases passed with the resulting Engine/Scripting DLLs.
The debugger evidence is retained in `tmp/ecs-assets-hybrid-access-violation.log`.
No Physics source workaround or disabled test was introduced.

Build logs include `tmp/ecs-assets-native-build.log`,
`tmp/ecs-assets-direct-sync.log`, `tmp/ecs-assets-engine-isolated-build.log`,
`tmp/ecs-assets-scripting-native-isolated-build.log`,
`tmp/ecs-assets-editor-final-build.log` and
`tmp/ecs-assets-editor-test-final-build.log`. Native PE import/export checks
finished with an empty mismatch list in `tmp/ecs-assets-import-mismatches.json`.
Matching Physics, Platform and Packages DLLs were copied from completed normal
runtime builds into the isolated directory and source/destination SHA-256 values
checked. This supersedes the older Physics dependency copy recorded above.

The full Scripting build's managed custom step **did not pass**: copying
`build/Managed/Debug/Nullus.Managed.dll` failed with MSB3027/MSB3021 because an
existing process held the file. That process was preserved. Reflection generation
had succeeded; the final native-only `'/t:ClCompile;_Link'` build completed in the
independent intermediate directory. The failed full attempt is retained in
`tmp/ecs-assets-scripting-isolated-build.log`; these native results do not claim
a successful managed rebuild. An automatic root CMake recheck also encountered
a concurrently edited Physics test target without sources; already generated
native projects were built directly with MSBuild. The complete repository build
and test suite are not claimed as passed.

Scoped `git diff --check`, source whitespace/conflict-marker checks and relative
documentation links passed separately. No user process was terminated, no normal
native runtime DLL was overwritten, and the Git index was left unchanged.

Player automatic ECS startup, cross-chunk references, complete Editor visible
interaction/Live Baking, native rendering/physics components, script ECS APIs,
macOS/Linux and TSan/UBSan remain unaccepted. This milestone's package test and
console example do not constitute a visible full Player or GPU acceptance run.

## Player ECS startup and executable self-test (2026-10-29)

The Player selects a native startup path for `.ecsscene`. Build collection treats
it as a cooked root, staging writes its GUID/sub-asset identity, and startup
registers built-in Transform and game codecs before decoding. The initial chunk
commits before Begin succeeds; failure publishes no World. Assets prefixes are
normalized consistently in settings, descriptors and loose legacy scene copies.

**147 native cases passed, with no failures, errors or skips:**

| Suite | Cases | GoogleTest seconds |
| --- | ---: | ---: |
| Core | 87/87 | 11.850 |
| Transform | 10/10 | 1.102 |
| Hybrid | 15/15 | 0.291 |
| Baking | 8/8 | 0.468 |
| Native assets | 13/13 | 3.823 |
| Editor asset import | 3/3 | 2.570 |
| Player startup | 11/11 | 1.824 |

Evidence: `tmp/ecs-player-native-{core,transform,hybrid,baking,assets,editor}.xml`
and matching logs, `tmp/ecs-player-final-tests.xml`,
`tmp/ecs-player-final-tests.log`, and `tmp/ecs-player-regression-results.log`.
Together with the previously passed 25 TaskGraph cases, there are **172 distinct
cases**. The earlier independent Debug/Release/ASan 122-case results were not
rerun for these Game/Build changes. Player tests are native Debug results.
Earlier timing tables are historical; some asset XML/log paths were refreshed
by this run. Their original aggregate summary remains in
`tmp/ecs-assets-final-native-run.log`.

The new tests use real Cook, dependency collection, staging DLL exports, Npak,
Player manifest/authorization and PlaySession. They cover dependency roots,
mount prefixes, metadata, stale/wrong targets, stable settings, removal of
development Assets/Library, pre-Begin commit, hierarchy update, restart identity,
custom codec registration, missing codec/corrupt-content rollback, development
manifest loading, and rejection of a prefab as startup identity. The legacy case
uses an explicitly retained real prefab: the existing Npak writer rejects an
entirely empty package, which is not claimed as supported here.

The real **Game.exe** was compiled and linked with its resource file, then run
from the example's content directory after deleting its Assets and Library:

| Scenario | Exit code | Evidence |
| --- | ---: | --- |
| `--help` | 0 | executable/dependency loading and argument parsing |
| `--package-self-test` | 0 | 1 Npak, 2 entries, native ECS startup content |
| same command with invalid startup GUID | 8 | explicit ECS identity failure |

Results: `tmp/ecs-player-cli-results.json`, `tmp/ecs-player-final-cli.log`.
Game.exe was copied into the unique example directory; DLLs were resolved via
the isolated validation directory in the child PATH. This is a content self-test,
not a fully assembled standalone distribution. The example also validated
`scene_entities=2 total_entities=6 child_world_x=13 background_parse=1`.
No render window, mouse interaction or GPU acceptance run was performed.

Root CMake configuration **did not complete**: `Tools/CMake/NullusPlugins.cmake:86`
rejected changed/unapproved scripts for `org.nullus.sample.editor`. The failure
is in `tmp/ecs-player-configure.log`. No plugin approval was changed and no
unapproved plugin build script was run. Native validation instead used copies of
already generated MSBuild projects with current sources and existing libraries,
created by `tmp/generate_ecs_player_projects.py`. The normal CMake test target is
`NullusECSPlayerTests`; the executable used here is `NullusECSPlayerValidation.exe`.

Core, Engine, Cook, Plugins and Build were rebuilt. Engine reflection was refreshed
before compiling the updated Collider headers. GameProject uses
`'/t:ClCompile;_Lib'`; executables use ClCompile/ResourceCompile/_Link as applicable.
Flags: `/Z7 /Y- /MP1`, `/m:1`, `BuildProjectReferences=false`,
`LinkIncremental=false`, `PostBuildEventUseInBuild=false`; output and intermediate
directories are under `build/ecs-native-validation/`. GetTargetPath requests
emitted shared-IntDir warnings for referenced projects; no clean or rebuild of
those references was requested. Existing Core/Engine/Cook/Build projects were
checked for missing current source files; none were missing. The new Game helper
is explicitly included in the validation project.

Build logs: `tmp/ecs-player-native-{core,engine,cook,plugins,build}-build.log`,
`tmp/ecs-player-engine-metadata.log`, `tmp/ecs-player-game-project-build.log`,
`tmp/ecs-player-game-executable-build.log`, `tmp/ecs-player-tests-build.log`,
`tmp/ecs-player-regression-build.log`, `tmp/ecs-player-example-build.log`.
Matching completed Physics/Platform DLLs were copied into the isolated runtime
with source/destination SHA-256 checks. PE import/export checks finished with
an empty mismatch list in `tmp/ecs-player-import-mismatches.json`.
User processes, unrelated source changes and staging boundaries were preserved.

Visible Player/GPU acceptance, native ECS rendering/physics, script ECS APIs,
Editor Live Baking and platform/sanitizer acceptance remain in Remaining.md.
