# Final Diagnostics: UI FrameGraph

Status: Complete. US1-US3 implementation, Phase 6 DX12 TimelineProfiler,
RenderDoc, product smoke, final plan-review, and multi-agent review gates are
recorded and closed.

## 2026-06-13 - Phase 2 Foundational Validation

- Build: `cmake --build Build --target NullusUnitTests --config Debug`
  - Result: passed after adding `RHIFrameGraph::UIOverlay` profiler classification.
  - Note: build ran the normal `Runtime/UI/Gen` MetaParser generation step; generated files were not hand-edited.
- T019 TDD red check:
  - Command: `Build\bin\Debug\NullusUnitTests.exe --gtest_filter=ProfilerDestinationTest.TimelineTraceExporterCanFilterEditorUiNoise`
  - Result before implementation: failed because `RHIFrameGraph::UIOverlay` and `RHIFrameGraph::UIOverlay.FontAtlasUpload` were not classified as editor UI events, and the trace exporter emitted `RHIFrameGraph::UIOverlay` when editor UI filtering was disabled.
- T019 green check:
  - Command: `Build\bin\Debug\NullusUnitTests.exe --gtest_filter=ProfilerDestinationTest.TimelineTraceExporterCanFilterEditorUiNoise`
  - Result after implementation: passed, 1/1.
- Phase 2 focused unit test command:
  - `Build\bin\Debug\NullusUnitTests.exe --gtest_filter=RHIUiOverlayPassTests.*:RHITypesTests.*:UiDrawDataSnapshotTests.*:RHIUiTextureRegistryTests.*:ProfilerDestinationTest.TimelineTraceExporterCanFilterEditorUiNoise:FrameGraphSceneTargetsTests.BuildThreadedExecutionPlanMapsPassMetadataByKind:FrameGraphSceneTargetsTests.ApplyThreadedExecutionPlanPrependsAdditionalOrderedWorkUnits`
  - Result: passed, 22/22 tests.
- Source guard status:
  - Not used as a Phase 2 pass/fail gate. Legacy UI bridge call paths are still expected to exist until US1 removes the migrated-path direct-submit route.

## 2026-06-13 - Phase 2 Review Fix Validation

- Review fixes:
  - Quarantined US1/US2 source guards with `GTEST_SKIP()` so full unit validation is not broken before product routing migrates.
  - Added stable `UiTextureId` ImGui packing/unpacking and rejected non-zero legacy/native `ImTextureID` values during snapshot capture.
  - Prevented `RenderPassCommandKind::UIOverlay` from being recorded by the generic prepared draw path until the real overlay renderer path is wired.
  - Routed fallback UI overlay pass names through `Context::kUIOverlayRenderPassDebugName`.
  - Changed `RHIImGuiFontAtlas::Invalidate()` to retire old RHI references until frame completion instead of immediately dropping them.
- Build:
  - Command: `cmake --build Build --target NullusUnitTests --config Debug`
  - Result: passed.
- Focused review-fix test command:
  - `Build\bin\Debug\NullusUnitTests.exe --gtest_filter=UiDrawDataSnapshotTests.*:RHIUiOverlayPassTests.*:RHIUiOverlaySourceGuardTests.*:ProfilerDestinationTest.TimelineTraceExporterCanFilterEditorUiNoise:RHITypesTests.*:RHIUiTextureRegistryTests.*:FrameGraphSceneTargetsTests.BuildThreadedExecutionPlanMapsPassMetadataByKind:FrameGraphSceneTargetsTests.ApplyThreadedExecutionPlanPrependsAdditionalOrderedWorkUnits`
  - Result: passed 28/30, skipped 2/30 expected future US1/US2 source guards.
- Full unit validation:
  - Command: `Build\bin\Debug\NullusUnitTests.exe`
  - Result: passed 2280/2283, skipped 3/2283. Skips were Autodesk FBX SDK availability and the two expected future UI source guards.

## 2026-06-13 - Phase 2 Review P1 Closure Validation

- Review fixes:
  - Added a link/behavior test for `DriverUIAccess::PublishUiDrawDataSnapshot`, `ConsumePendingUiDrawDataSnapshot`, `RegisterUiTextureView`, and `ReleaseUiTextureView`; implemented the missing Driver definitions with mutex-protected snapshot publish/consume storage.
  - Changed `RHIImGuiOverlayRenderer::Record()` to fail closed until real RHI ImGui recording lands in T029; visible snapshots no longer return a false-success result from the stub.
  - Removed the default `RHIImGuiFontAtlas::Invalidate()` retire-frame value and added a compile-time capability test proving production code cannot call it without an explicit retire frame id.
- TDD red check:
  - Command: `cmake --build Build --target NullusUnitTests --config Debug`
  - Result before implementation: failed at link with unresolved `DriverUIAccess::{PublishUiDrawDataSnapshot,ConsumePendingUiDrawDataSnapshot,RegisterUiTextureView,ReleaseUiTextureView}` symbols.
- Build:
  - Command: `cmake --build Build --target NullusUnitTests --config Debug`
  - Result after implementation: passed.
- Focused review-fix test command:
  - `Build\bin\Debug\NullusUnitTests.exe --gtest_filter=RHIUiOverlayPassTests.*:UiDrawDataSnapshotTests.*:RHIUiTextureRegistryTests.* --gtest_break_on_failure=1`
  - Result: passed 22/22.
- Phase 2 focused regression command:
  - `Build\bin\Debug\NullusUnitTests.exe --gtest_filter=UiDrawDataSnapshotTests.*:RHIUiOverlayPassTests.*:RHIUiOverlaySourceGuardTests.*:ProfilerDestinationTest.TimelineTraceExporterCanFilterEditorUiNoise:RHITypesTests.*:RHIUiTextureRegistryTests.*:FrameGraphSceneTargetsTests.BuildThreadedExecutionPlanMapsPassMetadataByKind:FrameGraphSceneTargetsTests.ApplyThreadedExecutionPlanPrependsAdditionalOrderedWorkUnits --gtest_break_on_failure=1`
  - Result: passed 31/33, skipped 2/33 expected future US1/US2 source guards.
- Full unit validation:
  - Command: `Build\bin\Debug\NullusUnitTests.exe`
  - Result: passed 2283/2286, skipped 3/2286. Skips were Autodesk FBX SDK availability and the two expected future UI source guards.
- Sanity checks:
  - `git diff --name-only -- "Runtime/*/Gen/*" "Project/*/Gen/*"` returned no generated file edits.
  - `git diff --check` initially caught trailing whitespace in newly-added spec template files; those 049 spec-bundle lines were mechanically cleaned.
  - Final `git diff --check` reported only CRLF conversion warnings; no whitespace errors.

## 2026-06-13 - Phase 2 Multi-Agent Review Fix Validation

- Multi-agent review findings addressed:
  - `RHIImGuiTextureRegistry::RegisterTextureView()` now rejects null views without consuming stable IDs and returns the same `UiTextureId` when the same live view is registered repeatedly.
  - `RHIImGuiTextureRegistry::Resolve()` now fails closed for invalid IDs, null entries, and release-requested entries, so released texture views no longer resolve for new snapshots.
  - `UiTextureId::IsValid()` now requires both non-zero value and non-zero generation.
  - `PackUiTextureIdForImGui()` no longer collapses invalid non-font IDs to `0`; invalid IDs encode as an unsupported marker and are rejected by snapshot capture instead of becoming font-atlas draws.
  - `CaptureUiDrawDataSnapshot()` no longer marks zero display-size or zero framebuffer-scale frames as visible overlay work.
  - `ApplyThreadedRenderSceneExecutionPlan()` now includes `uiOverlayDrawCount` in `drawCommandCount` when scene `recordedDrawCommands` are also present.
  - `DX12CapabilityPlumbingKeepsUiOverlayNotRuntimeSelectable` now queries `CreateDX12DeviceResources(false).capabilities` instead of relying on source-string search.
- TDD red checks:
  - Command: `Build\bin\Debug\NullusUnitTests.exe --gtest_filter=RHIUiTextureRegistryTests.*:UiDrawDataSnapshotTests.InvalidUiTextureIdentityDoesNotCollapseToFontAtlas:UiDrawDataSnapshotTests.ZeroSizedFramebufferDoesNotProduceVisibleSnapshot`
  - Result before implementation: failed 5 tests covering invalid texture ID collapse, zero-sized visible snapshots, null registry IDs, duplicate registry IDs, and released registry resolution.
  - Command: `Build\bin\Debug\NullusUnitTests.exe --gtest_filter=RHIUiOverlayPassTests.ApplyingExecutionPlanIncludesUiOverlayInDrawCommandCount --gtest_break_on_failure=1`
  - Result before implementation: failed because `drawCommandCount` was `2` instead of scene `2` + UI overlay `3`.
- Build:
  - Command: `cmake --build Build --target NullusUnitTests --config Debug`
  - Result after implementation: passed.
- Focused P1-fix test command:
  - `Build\bin\Debug\NullusUnitTests.exe --gtest_filter=RHIUiTextureRegistryTests.*:UiDrawDataSnapshotTests.InvalidUiTextureIdentityDoesNotCollapseToFontAtlas:UiDrawDataSnapshotTests.ZeroSizedFramebufferDoesNotProduceVisibleSnapshot --gtest_break_on_failure=1`
  - Result: passed 6/6.
- Phase 2 focused regression command:
  - `Build\bin\Debug\NullusUnitTests.exe --gtest_filter=UiDrawDataSnapshotTests.*:RHIUiOverlayPassTests.*:RHIUiOverlaySourceGuardTests.*:ProfilerDestinationTest.TimelineTraceExporterCanFilterEditorUiNoise:RHITypesTests.*:RHIUiTextureRegistryTests.*:FrameGraphSceneTargetsTests.BuildThreadedExecutionPlanMapsPassMetadataByKind:FrameGraphSceneTargetsTests.ApplyThreadedExecutionPlanPrependsAdditionalOrderedWorkUnits --gtest_break_on_failure=1`
  - Result: passed 37/39, skipped 2/39 expected future US1/US2 source guards.
- Full unit validation:
  - Command: `Build\bin\Debug\NullusUnitTests.exe`
  - Result: passed 2289/2292, skipped 3/2292. Skips were Autodesk FBX SDK availability and the two expected future UI source guards.
- Sanity checks:
  - `git diff --name-only -- "Runtime/*/Gen/*" "Project/*/Gen/*"` returned no generated file edits.
  - `git diff --check` reported only CRLF conversion warnings; no whitespace errors.

## 2026-06-13 - Phase 2 Second-Round Review Fix Validation

- Second-round review findings addressed:
  - `DriverUIAccess::RegisterUiTextureView()` and `ReleaseUiTextureView()` now route through a `DriverImpl`-owned `RHIImGuiTextureRegistry` instead of no-op stubs.
  - `RHIImGuiTextureRegistry::ReleaseTextureView()` now prunes Phase 2 entries immediately, so released views do not remain retained until registry destruction. US3 will replace this with frame-retired binding/descriptor retention when real overlay texture binding lands.
  - `UiTextureId::IsFontAtlas()` now accepts only the exact `{ value=0, generation=0 }` sentinel; malformed `{ value=0, generation!=0 }` IDs encode as unsupported and cannot collapse to the font-atlas path.
- TDD red check:
  - Command: `cmake --build Build --target NullusUnitTests --config Debug; if ($LASTEXITCODE -eq 0) { Build\bin\Debug\NullusUnitTests.exe --gtest_filter=UiDrawDataSnapshotTests.InvalidUiTextureIdentityDoesNotCollapseToFontAtlas:RHIUiTextureRegistryTests.ReleasePrunesRetainedTextureViewEntry:RHIUiOverlayPassTests.DriverUIAccessRegistersStableUiTextureIdentities --gtest_break_on_failure=1 }`
  - Result before implementation: failed at build because `RHIImGuiTextureRegistry::GetEntryCountForTesting()` did not exist; this forced explicit release/prune test coverage.
- Build:
  - Command: `cmake --build Build --target NullusUnitTests --config Debug`
  - Result after implementation: passed.
- Focused second-round P1-fix command:
  - `Build\bin\Debug\NullusUnitTests.exe --gtest_filter=UiDrawDataSnapshotTests.InvalidUiTextureIdentityDoesNotCollapseToFontAtlas:RHIUiTextureRegistryTests.ReleasePrunesRetainedTextureViewEntry:RHIUiOverlayPassTests.DriverUIAccessRegistersStableUiTextureIdentities --gtest_break_on_failure=1`
  - Result: passed 3/3.
- Phase 2 focused regression command:
  - `Build\bin\Debug\NullusUnitTests.exe --gtest_filter=UiDrawDataSnapshotTests.*:RHIUiOverlayPassTests.*:RHIUiOverlaySourceGuardTests.*:ProfilerDestinationTest.TimelineTraceExporterCanFilterEditorUiNoise:RHITypesTests.*:RHIUiTextureRegistryTests.*:FrameGraphSceneTargetsTests.BuildThreadedExecutionPlanMapsPassMetadataByKind:FrameGraphSceneTargetsTests.ApplyThreadedExecutionPlanPrependsAdditionalOrderedWorkUnits --gtest_break_on_failure=1`
  - Result: passed 39/41, skipped 2/41 expected future US1/US2 source guards.
- Full unit validation:
  - Command: `Build\bin\Debug\NullusUnitTests.exe`
  - Result: passed 2291/2294, skipped 3/2294. Skips were Autodesk FBX SDK availability and the two expected future UI source guards.
- Sanity checks:
  - `git diff --name-only -- "Runtime/*/Gen/*" "Project/*/Gen/*"` returned no generated file edits.
  - `git diff --check` reported only CRLF conversion warnings; no whitespace errors.

## 2026-06-13 - Phase 2 External Registry Authority Closure

- Review fix addressed:
  - `RHIImGuiOverlayRenderer` no longer owns a private texture registry authority. It accepts an external `RHIImGuiTextureRegistry*`, exposes the active pointer for validation, and can be retargeted with `SetTextureRegistry()`.
  - `DriverImpl` remains the Phase 2 authority for `DriverUIAccess::RegisterUiTextureView()` and `ReleaseUiTextureView()` via its owned `uiTextureRegistry`.
  - `OverlayRendererUsesExternalTextureRegistryAuthority` covers the previous split-registry P1 by proving the renderer uses caller-supplied registry authority instead of an internal registry instance.
- Build:
  - Command: `cmake --build Build --target NullusUnitTests --config Debug`
  - Result: passed.
- Focused external-registry closure command:
  - `Build\bin\Debug\NullusUnitTests.exe --gtest_filter=RHIUiOverlayPassTests.OverlayRendererUsesExternalTextureRegistryAuthority:RHIUiOverlayPassTests.OverlayRendererFailsClosedUntilRhiRecordingIsImplemented --gtest_break_on_failure=1`
  - Result: passed 2/2.
- Phase 2 focused regression command:
  - `Build\bin\Debug\NullusUnitTests.exe --gtest_filter=UiDrawDataSnapshotTests.*:RHIUiOverlayPassTests.*:RHIUiOverlaySourceGuardTests.*:ProfilerDestinationTest.TimelineTraceExporterCanFilterEditorUiNoise:RHITypesTests.*:RHIUiTextureRegistryTests.*:FrameGraphSceneTargetsTests.BuildThreadedExecutionPlanMapsPassMetadataByKind:FrameGraphSceneTargetsTests.ApplyThreadedExecutionPlanPrependsAdditionalOrderedWorkUnits --gtest_break_on_failure=1`
  - Result: passed 40/42, skipped 2/42 expected future US1/US2 source guards.
- Full unit validation:
  - Command: `Build\bin\Debug\NullusUnitTests.exe`
  - Result: passed 2292/2295, skipped 3/2295. Skips were Autodesk FBX SDK availability and the two expected future UI source guards.
- Sanity checks:
  - `git diff --name-only -- "Runtime/*/Gen/*" "Project/*/Gen/*"` returned no generated file edits.
  - `git diff --check` reported only CRLF conversion warnings; no whitespace errors.
- Scoped Phase 2 limitations:
  - DX12 product routing remains not runtime-selectable until US1-US3 complete overlay recording, UI-only routing, font/texture retention, and direct-submit exclusion.
  - `RHIImGuiTextureRegistry::ReleaseTextureView()` still needed a stronger frame-retired contract at this point; this limitation is superseded by the Phase 2 Registry Lifetime Closure below.

## 2026-06-13 - Phase 2 Registry Lifetime Closure

- Review fixes addressed:
  - `RHIImGuiTextureRegistry` now has an internal mutex around register, release, resolve, test count, and retire operations.
  - `Resolve()` returns an `std::optional<RHIImGuiTextureRegistryEntry>` copy instead of a raw pointer into the registry map.
  - `ReleaseTextureView()` now marks entries `releaseRequested` with `safeToReleaseFrameId` and removes only the live reverse-index entry; `ReleaseRetiredTextureViewsUpTo()` performs physical erase after frame retirement.
  - Registration uses a reverse index from live texture view pointer to UI ID, so re-registering the same live view avoids a full entry scan.
  - Re-registering a released-but-retained view returns a fresh `UiTextureId`, while the old entry remains retained until its retire frame.
  - `DriverUIAccess::ReleaseUiTextureView()` now uses a `DriverImpl` UI snapshot frame watermark; publishing an empty snapshot does not reset the latest non-zero UI frame used for texture retirement.
  - `RecordPreparedDrawCommandsForPassRange()` now rejects `RenderPassCommandKind::UIOverlay`, matching the single-pass generic recording guard.
- TDD red checks:
  - Command: `cmake --build Build --target NullusUnitTests --config Debug; if ($LASTEXITCODE -eq 0) { Build\bin\Debug\NullusUnitTests.exe --gtest_filter=RHIUiTextureRegistryTests.*:RHIUiOverlayPassTests.UIOverlayCannotBeRecordedByGenericDrawPassPath --gtest_break_on_failure=1 }`
  - Result before implementation: failed at build because `Resolve()` still returned a raw pointer and `ReleaseRetiredTextureViewsUpTo()` did not exist; after the registry implementation, `UIOverlayCannotBeRecordedByGenericDrawPassPath` failed because range recording returned `4` instead of `0`.
  - Command: `Build\bin\Debug\NullusUnitTests.exe --gtest_filter=RHIUiOverlayPassTests.DriverUIAccessDoesNotResetUiTextureRetireFrameOnEmptySnapshotPublish --gtest_break_on_failure=1`
  - Result before implementation: failed because publishing an empty snapshot reset the retire watermark and the texture entry retired at frame `76` instead of remaining until frame `77`.
- Build:
  - Command: `cmake --build Build --target NullusUnitTests --config Debug`
  - Result after implementation: passed.
- Focused lifetime-fix command:
  - `Build\bin\Debug\NullusUnitTests.exe --gtest_filter=RHIUiOverlayPassTests.DriverUIAccessDoesNotResetUiTextureRetireFrameOnEmptySnapshotPublish:RHIUiTextureRegistryTests.* --gtest_break_on_failure=1`
  - Result: passed 8/8.
- Phase 2 focused regression command:
  - `Build\bin\Debug\NullusUnitTests.exe --gtest_filter=UiDrawDataSnapshotTests.*:RHIUiOverlayPassTests.*:RHIUiOverlaySourceGuardTests.*:ProfilerDestinationTest.TimelineTraceExporterCanFilterEditorUiNoise:RHITypesTests.*:RHIUiTextureRegistryTests.*:FrameGraphSceneTargetsTests.BuildThreadedExecutionPlanMapsPassMetadataByKind:FrameGraphSceneTargetsTests.ApplyThreadedExecutionPlanPrependsAdditionalOrderedWorkUnits --gtest_break_on_failure=1`
  - Result: passed 43/45, skipped 2/45 expected future US1/US2 source guards.
- Full unit validation:
  - Command: `Build\bin\Debug\NullusUnitTests.exe`
  - Result: passed 2295/2298, skipped 3/2298. Skips were Autodesk FBX SDK availability and the two expected future UI source guards.
- Sanity checks:
  - `git diff --name-only -- "Runtime/*/Gen/*" "Project/*/Gen/*"` returned no generated file edits.
  - `git diff --check` reported only CRLF conversion warnings; no whitespace errors.
- Remaining scoped Phase 2 limitations:
  - The registry now retains `RHITextureView` entries by retire frame, but binding-set and backend descriptor lifetime retention still land with real overlay texture binding in US3.
  - `ReleaseRetiredTextureViewsUpTo()` is covered and available, but at this point it was not yet safely wired to the submitted UI snapshot completion path. This limitation is superseded by the Phase 2 Standalone UI Retirement Closure below.
  - `runtimeSelectable` remains a documentation/model concept in Phase 2; the current C++ capability state safely encodes "not runtime-selectable" as `supported=false` with an explicit reason. Reconcile this model before T057 enables product selection.

## 2026-06-13 - Phase 2 In-Flight Texture Resolve And Cleanup Closure

- Review fixes addressed:
  - `RHIImGuiTextureRegistry::ResolveForFrame()` now allows a released-but-retained texture ID to resolve for the already-published UI frame that can still reference it, while `Resolve()` continues to fail closed for new snapshots.
  - Deferred threaded frame resource cleanup now carries the UI texture retire frame id per frame-context slot and releases retired UI texture views after the deferred frame-scoped fence path completes.
  - Successful non-deferred threaded submissions initially released retired UI texture views by `RhiSubmissionFrame::frameId`; that unsafe scene-frame watermark is superseded by the Phase 2 Standalone UI Retirement Closure below, which uses the submitted `uiOverlaySnapshotFrameId`.
- TDD red checks:
  - Command: `cmake --build Build --target NullusUnitTests --config Debug; if ($LASTEXITCODE -eq 0) { Build\bin\Debug\NullusUnitTests.exe --gtest_filter=RHIUiTextureRegistryTests.ReleasedTextureViewStillResolvesForPublishedFrameUntilRetired:RHIUiOverlayPassTests.DeferredFrameScopedCleanupReleasesRetiredUiTextureViews --gtest_break_on_failure=1 }`
  - Result before implementation: failed at build because `RHIImGuiTextureRegistry::ResolveForFrame()` did not exist.
- Focused in-flight lifetime command:
  - `Build\bin\Debug\NullusUnitTests.exe --gtest_filter=RHIUiTextureRegistryTests.ReleasedTextureViewStillResolvesForPublishedFrameUntilRetired:RHIUiOverlayPassTests.DeferredFrameScopedCleanupReleasesRetiredUiTextureViews --gtest_break_on_failure=1`
  - Result: passed 2/2.
- Phase 2 focused regression command:
  - `Build\bin\Debug\NullusUnitTests.exe --gtest_filter=UiDrawDataSnapshotTests.*:RHIUiOverlayPassTests.*:RHIUiOverlaySourceGuardTests.*:ProfilerDestinationTest.TimelineTraceExporterCanFilterEditorUiNoise:RHITypesTests.*:RHIUiTextureRegistryTests.*:FrameGraphSceneTargetsTests.BuildThreadedExecutionPlanMapsPassMetadataByKind:FrameGraphSceneTargetsTests.ApplyThreadedExecutionPlanPrependsAdditionalOrderedWorkUnits --gtest_break_on_failure=1`
  - Result: passed 45/47, skipped 2/47 expected future US1/US2 source guards.
- Full unit validation:
  - Command: `Build\bin\Debug\NullusUnitTests.exe`
  - Result: passed 2297/2300, skipped 3/2300. Skips were Autodesk FBX SDK availability and the two expected future UI source guards.
- Sanity checks:
  - `git diff --name-only -- "Runtime/*/Gen/*" "Project/*/Gen/*"` returned no generated file edits.
  - `git diff --check` reported only CRLF conversion warnings; no whitespace errors.
- Remaining scoped Phase 2 limitations:
  - UI texture view lifetime now has frame-aware resolve and cleanup hooks, but binding-set/backend descriptor retention still lands with real overlay texture binding in US3.
  - DX12 product routing remains not runtime-selectable until US1-US3 finish overlay recording, UI-only routing, migrated font/texture binding, and direct-submit exclusion.

## 2026-06-13 - Phase 2 Standalone UI Retirement Closure

- Review fix addressed:
  - Standalone explicit frame completion and standalone UI frame completion no longer advance UI texture retirement, because Phase 2 standalone paths do not submit a FrameGraph UI snapshot and cannot prove a UI snapshot frame has GPU-retired.
  - `RhiSubmissionFrame` now carries `uiOverlaySnapshotFrameId`, derived only from a submitted package that actually contains `RHIFrameGraph::UIOverlay`.
  - Non-deferred threaded completion releases retired UI texture views only up to `uiOverlaySnapshotFrameId`, not the unrelated scene/RHI frame id.
  - Deferred threaded cleanup stores and releases the submitted UI snapshot frame id only when one was present; no-UI deferred frames leave the UI registry watermark untouched.
  - `RHIImGuiTextureRegistry` now tracks released entries in a retire-frame ordered queue, so `ReleaseRetiredTextureViewsUpTo()` drains only completed retire entries instead of scanning the entire registry map every completion.
- TDD red check:
  - Command: `cmake --build Build --target NullusUnitTests --config Debug`
  - Result before implementation: failed at link with unresolved `Detail::ReleaseRetiredUiTextureViewsForCompletedUiFrame`, proving the new standalone UI texture-retirement coverage was not implemented yet.
  - Command: `cmake --build Build --target NullusUnitTests --config Debug; if ($LASTEXITCODE -eq 0) { Build\bin\Debug\NullusUnitTests.exe --gtest_filter=RHIUiOverlayPassTests.UncompletedPublishedUiSnapshotDoesNotRetireOnUnrelatedFrameCompletion:RHIUiOverlayPassTests.StandaloneFrameCompletionPathsDoNotAdvanceUiTextureRetirementWatermark --gtest_break_on_failure=1 }`
  - Result before final fix: failed because completed frame `18` retired a texture view whose safe UI snapshot frame was `19`.
  - Command: `cmake --build Build --target NullusUnitTests --config Debug; if ($LASTEXITCODE -eq 0) { Build\bin\Debug\NullusUnitTests.exe --gtest_filter=RHIUiTextureRegistryTests.ReleaseRetirementQueueDrainsOnlyCompletedFrames --gtest_break_on_failure=1 }`
  - Result before queue optimization: failed because released entries did not populate a retire-frame ordered pending queue.
- Build:
  - Command: `cmake --build Build --target NullusUnitTests --config Debug`
  - Result after implementation: passed.
- Focused standalone-retirement command:
  - `Build\bin\Debug\NullusUnitTests.exe --gtest_filter=RHIUiOverlayPassTests.UncompletedPublishedUiSnapshotDoesNotRetireOnUnrelatedFrameCompletion:RHIUiOverlayPassTests.StandaloneFrameCompletionPathsDoNotAdvanceUiTextureRetirementWatermark:RHIUiOverlayPassTests.ThreadedCompletionRetiresUiTexturesOnlyForSubmittedUiSnapshotFrame --gtest_break_on_failure=1`
  - Result: passed 3/3.
- Focused UI overlay/registry command:
  - `Build\bin\Debug\NullusUnitTests.exe --gtest_filter=RHIUiTextureRegistryTests.*:RHIUiOverlayPassTests.* --gtest_break_on_failure=1`
  - Result: passed 33/33.
- Phase 2 focused regression command:
  - `Build\bin\Debug\NullusUnitTests.exe --gtest_filter=UiDrawDataSnapshotTests.*:RHIUiOverlayPassTests.*:RHIUiOverlaySourceGuardTests.*:ProfilerDestinationTest.TimelineTraceExporterCanFilterEditorUiNoise:RHITypesTests.*:RHIUiTextureRegistryTests.*:FrameGraphSceneTargetsTests.BuildThreadedExecutionPlanMapsPassMetadataByKind:FrameGraphSceneTargetsTests.ApplyThreadedExecutionPlanPrependsAdditionalOrderedWorkUnits --gtest_break_on_failure=1`
  - Result: passed 49/51, skipped 2/51 expected future US1/US2 source guards.
- Full unit validation:
  - Command: `Build\bin\Debug\NullusUnitTests.exe`
  - Result: passed 2301/2304, skipped 3/2304. Skips were Autodesk FBX SDK availability and the two expected future UI source guards.
- Sanity checks:
  - `git diff --name-only -- "Runtime/*/Gen/*" "Project/*/Gen/*"` returned no generated file edits.
  - `git diff --check` reported only CRLF conversion warnings; no whitespace errors.
- Remaining scoped Phase 2 limitations:
  - UI texture view lifetime is wired to Phase 2 frame-completion hooks, but binding-set/backend descriptor retention still lands with real overlay texture binding in US3.
  - DX12 product routing remains not runtime-selectable until US1-US3 finish overlay recording, UI-only routing, migrated font/texture binding, and direct-submit exclusion.

## 2026-06-13 - Phase 2 Review Gate Summary

- Mandatory multi-agent review:
  - R1 architecture/performance: found P1 that UI texture retirement used the latest published UI snapshot frame as a completion watermark; found P2 that registry cleanup scanned the full map.
  - R1 GPU correctness: found the same P1 early-retire hazard; no additional P0/P1.
  - R1 code quality/SSoT/tests: found the same P1 and later P2 documentation/test-guard drift; no generated-file edits.
  - R1 industry reference: no P0/P1/P2; UE4.27 references are documented as RHI/viewport lifecycle analogs, not exact FrameGraph equivalence.
  - Fixes applied: `RhiSubmissionFrame::uiOverlaySnapshotFrameId`, submitted-UI-snapshot-only retirement, standalone paths not advancing UI texture retirement, retire-frame ordered registry cleanup queue, stronger standalone source guard, and corrected diagnostics.
  - Deeper audit/final review: architecture final 0 P0/P1/P2; GPU final 0 P0/P1/P2; industry final 0 P0/P1/P2; SSoT final had only stale validation-count P2, fixed by updating the Phase 2 focused and full-suite counts.
- `plan-review` 8-dimension gate:
  - Auto-fail checks: no P0/P1 conditions remain; no generated files were hand-edited; US1-US3 runtime routing remains explicitly incomplete and DX12 `UIOverlayFrameGraph` remains not runtime-selectable.
  - Score: 74/80 after fixes. Correctness 9, robustness 9, performance 9, maintainability 9, industry best practice 9, code quality 10, test coverage 10, security 9.
  - Industry benchmark basis: local `benchmarks/rendering_layout.md` RHI in-render-pass / frame-retired lifetime criteria plus documented UE4.27 Slate/RHI/D3D12 viewport analogs. Runtime performance claims remain deferred until TimelineProfiler/RenderDoc evidence.
  - Result: pass for Phase 2 foundation with no P0/P1. Remaining limitations are scoped to later US1-US3: real overlay draw recording, UI-only product routing, migrated font/texture binding-set and descriptor retention, and DX12 runtime-selectable enablement.

## 2026-06-13 - US1 Scene-Frame Overlay Routing And Minimal Recording

- Implementation progress:
  - `UIManager::Render()` now calls `ImGui::Render()` and, when `UIOverlayFrameGraph` is supported, publishes a copied `UiDrawDataSnapshot` instead of calling `m_uiBridge->RenderDrawData` on the migrated branch.
  - `DriverRendererAccess::TryPublishPreparedFrameBuilder()` consumes pending UI snapshots inside the prepared package builder and attaches visible snapshots to the scene `RenderScenePackage`.
  - `RenderScenePackageBuilder` appends final `RHIFrameGraph::UIOverlay` passes after scene swapchain passes, with swapchain load/store semantics, render-target write access, present transition, and scene-to-UI visibility dependency edge coverage.
  - `DriverImpl` now owns an `RHIImGuiOverlayRenderer` bound to the driver-owned UI texture registry.
  - `RhiThreadCoordinator` records `UIOverlay` pass payloads through `RHIImGuiOverlayRenderer::Record()` on both translated work-unit and main serial command-buffer paths; overlay recording failures now mark existing command-recording telemetry.
  - `RHIImGuiOverlayRenderer::Record()` now performs minimum snapshot-driven RHI command recording: viewport, clipped scissor, and `DrawIndexedChecked()` per visible command. This is a command-level MVP, not the final pipeline/buffer/font implementation.
- TDD red checks:
  - `Build\bin\Debug\NullusUnitTests.exe --gtest_filter=RHIUiOverlayPassTests.OverlayRendererRecordsVisibleDrawCommandsThroughRhiCommandBuffer --gtest_break_on_failure=1`
    - Result before implementation: failed because `RHIImGuiOverlayRenderer::Record()` returned `success=false` with "not implemented".
  - `Build\bin\Debug\NullusUnitTests.exe --gtest_filter=ThreadedRenderingLifecycleTests.ThreadedUiOverlayPassRecordsSnapshotDrawsThroughRhiWorker --gtest_break_on_failure=1`
    - Result before RHI-thread integration: failed because the UI overlay pass was submitted without beginning a render pass or recording an indexed draw.
- Build:
  - `F:\Microsoft Visual Studio\2022\MSBuild\Current\Bin\amd64\MSBuild.exe Build\Runtime\Rendering\NLS_Render.vcxproj /p:Configuration=Debug /p:Platform=x64 /m:1 /v:m`
    - Result: passed.
  - `F:\Microsoft Visual Studio\2022\MSBuild\Current\Bin\amd64\MSBuild.exe Build\Tests\Unit\NullusUnitTests.vcxproj /p:Configuration=Debug /p:Platform=x64 /m:1 /v:m`
    - Result: passed.
  - Note: two earlier `cmake --build` attempts timed out and left orphaned MSBuild/CL processes that caused transient `MetaGenerated.obj` permission errors and one temporary post-build copy failure. The root cause was stale build processes from the timed-out command; after killing those build-only processes, single-worker MSBuild completed successfully. No generated files were edited.
- Focused green checks:
  - `Build\bin\Debug\NullusUnitTests.exe --gtest_filter=ThreadedRenderingLifecycleTests.ThreadedUiOverlayPassRecordsSnapshotDrawsThroughRhiWorker:RHIUiOverlayPassTests.OverlayRendererRecordsVisibleDrawCommandsThroughRhiCommandBuffer --gtest_break_on_failure=1`
    - Result: passed 2/2.
  - `Build\bin\Debug\NullusUnitTests.exe --gtest_filter=UiDrawDataSnapshotTests.*:RHIUiOverlayPassTests.*:RHIUiOverlaySourceGuardTests.*:ProfilerDestinationTest.TimelineTraceExporterCanFilterEditorUiNoise --gtest_break_on_failure=1`
    - Result: passed 38/38.
- Sanity checks:
  - `git diff --name-only -- "Runtime/*/Gen/*" "Project/*/Gen/*"` returned no generated file edits.
- Remaining scoped US1 limitations:
  - T029 is not complete: shader artifact registration, actual graphics pipeline creation/binding, dynamic vertex/index buffer allocation/upload, font atlas first-use upload/binding, and final indexed draw resource binding are still pending.
  - T030 is only partially complete: UIOverlay pass payloads are recorded, but atlas/dynamic-buffer upload-to-read visibility is still pending.
  - T021/T028 are still partially complete because font atlas first-use and font/texture shader-read resource ranges are not yet declared/validated.
  - DX12 `UIOverlayFrameGraph` remains intentionally not runtime-selectable; no FPS/wait improvement claim is made without 300-frame TimelineProfiler/RenderDoc/product evidence.

## 2026-06-13 - US1 Dynamic UI Buffer Recording Step

- Implementation progress:
  - `RHIImGuiOverlayRenderer` now has a device-aware `Record()` path that creates reusable CPU-to-GPU dynamic vertex/index buffers, uploads copied snapshot vertices/indices through `RHIBuffer::UpdateData()`, binds those buffers, and records indexed draws with correct global draw-list vertex/index offsets.
  - The command-level `Record(commandBuffer, snapshot)` overload remains for non-device tests and still records viewport/scissor/indexed draws without claiming full resource binding.
  - Threaded RHI overlay recording paths now call the device-aware overlay renderer so worker/serial command paths exercise the dynamic buffer path.
  - `RenderScenePackageBuilder` declares UI overlay dynamic vertex/index read access in addition to swapchain render-target access and present transition metadata.
  - `ThreadedUiOverlayPassRecordsSnapshotDrawsThroughRhiWorker` now verifies one graphics submit/present, dynamic vertex/index buffer creation/update, buffer binding, render-pass load semantics, and indexed draw recording.
- Build:
  - `dotnet restore Build\windows_timeline\Tools\MetaParser\generated\src\MetaParser.csproj`
    - Result: restored missing `libclang.runtime.win-x64` dependency needed by the MetaParser custom build.
  - `F:\Microsoft Visual Studio\2022\MSBuild\Current\Bin\MSBuild.exe Build\Tests\Unit\NullusUnitTests.vcxproj /p:Configuration=Debug /p:Platform=x64 /m:1 /v:m`
    - Result: passed.
- Focused green check:
  - `Build\bin\Debug\NullusUnitTests.exe --gtest_filter=UiDrawDataSnapshotTests.*:RHIUiOverlayPassTests.*:RHIUiOverlaySourceGuardTests.*:ProfilerDestinationTest.TimelineTraceExporterCanFilterEditorUiNoise:ThreadedRenderingLifecycleTests.ThreadedUiOverlayPassRecordsSnapshotDrawsThroughRhiWorker`
    - Result: passed 41/41.
- Sanity checks:
  - `git diff --check` returned no whitespace errors; it reported only existing CRLF conversion warnings.
  - `git diff --name-only -- "Runtime/*/Gen/*" "Project/*/Gen/*"` returned no generated file edits.
- Remaining scoped US1 limitations:
  - T029 remains incomplete: actual ImGui graphics pipeline creation/binding, shader artifact registration, font atlas first-use upload/binding, and registered texture binding still need implementation.
  - T030 remains partial: payload recording is device-aware now, but explicit atlas and dynamic-buffer upload-to-read visibility integration still needs resource-state work beyond CPU-to-GPU `UpdateData()`.
  - No runtime FPS/wait claims are made; DX12 `UIOverlayFrameGraph` remains not runtime-selectable until US1-US3 validation gates finish.

## 2026-06-13 - US1 Dynamic Buffer Slot Isolation And Upload Visibility

- Implementation progress:
  - `RHIImGuiOverlayRenderer` now keeps CPU-to-GPU dynamic vertex/index buffers isolated per frame-resource slot, so in-flight frames do not overwrite one another.
  - The overlay renderer now prepares dynamic buffers before the UI render pass, recording an explicit buffer barrier before pass begin with `Host` -> `VertexInput` stages and `HostWrite` -> `VertexRead` / `IndexRead` access masks on `GenericRead` CPU-visible upload heaps.
  - The renderer now skips unsupported texture-ID commands in the same place it already skips unsupported callbacks and empty commands, keeping recorded draw counts aligned with the pass contract.
  - `RhiThreadCoordinator` now threads `frameContextIndex` through all UI overlay recording paths so slot ownership stays consistent in worker, parallel-fallback, serial, and child-command paths.
  - `RenderScenePackageBuilder` no longer declares null dynamic vertex/index buffer resources in the package; it also resolves scene-to-UI dependency edges from the actual render-target write access instead of blindly using the previous pass's first texture access.
- Validation:
  - `F:\Microsoft Visual Studio\2022\MSBuild\Current\Bin\MSBuild.exe Build\Tests\Unit\NullusUnitTests.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:BuildInParallel=false /m:1 /nr:false /v:m`
    - Result: passed.
  - `Build\bin\Debug\NullusUnitTests.exe --gtest_filter=RHIUiOverlayPassTests.OverlayRendererCreatesBindsAndOffsetsDynamicVertexIndexBuffers`
    - Result before fix: failed because no buffer barriers were recorded for the CPU-visible upload buffers.
  - `Build\bin\Debug\NullusUnitTests.exe --gtest_filter=RHIUiOverlayPassTests.OverlayRendererCreatesBindsAndOffsetsDynamicVertexIndexBuffers:RHIUiOverlayPassTests.OverlayRendererKeepsDynamicBuffersIsolatedPerFrameResourceSlot:RHIUiOverlayPassTests.OverlayRendererSkipsUnsupportedTextureIdCommands`
    - Result after fix: passed 3/3.
  - `Build\bin\Debug\NullusUnitTests.exe --gtest_filter=RHIUiOverlayPassTests.UiOverlayDependencyEdgeUsesSceneSwapchainWriterWhenScenePassHasMultipleTextureAccesses:RHIUiOverlayPassTests.UiOverlayPassDoesNotDeclareNullDynamicVertexIndexBufferResources:RHIUiOverlayPassTests.OverlayRendererCreatesBindsAndOffsetsDynamicVertexIndexBuffers`
    - Result after review fixes: passed 3/3.
  - `Build\bin\Debug\NullusUnitTests.exe --gtest_filter=UiDrawDataSnapshotTests.*:RHIUiOverlayPassTests.*:RHIUiOverlaySourceGuardTests.*:ProfilerDestinationTest.TimelineTraceExporterCanFilterEditorUiNoise:ThreadedRenderingLifecycleTests.ThreadedUiOverlayPassRecordsSnapshotDrawsThroughRhiWorker`
    - Result after initial dynamic-buffer fix: passed 43/43.
  - `Build\bin\Debug\NullusUnitTests.exe --gtest_filter=UiDrawDataSnapshotTests.*:RHIUiOverlayPassTests.*:RHIUiOverlaySourceGuardTests.*:ProfilerDestinationTest.TimelineTraceExporterCanFilterEditorUiNoise:ThreadedRenderingLifecycleTests.ThreadedUiOverlayPassRecordsSnapshotDrawsThroughRhiWorker`
    - Result after review fixes for pre-pass upload visibility and no-null dynamic buffer package declarations: passed 44/44.

## 2026-06-13 - US1 Split Overlay Prepare/Record Review Closure

- Review fix addressed:
  - Removed the public device-backed one-step `RHIImGuiOverlayRenderer::Record(device, commandBuffer, snapshot[, slot])` API because it could prepare dynamic buffers and emit upload-visibility barriers from a caller that was already inside an active render pass.
  - Removed the remaining public command-buffer-only `Record(commandBuffer, snapshot)` convenience API because it could bypass dynamic-buffer preparation and return successful draw recording without the prepared buffer contract.
  - The dynamic-buffer path is now compile-time guided through `PrepareFrameResources(device, commandBuffer, snapshot, frameResourceSlot)` before `BeginPassCommandPlan` and `RecordPrepared(commandBuffer, snapshot, frameResourceSlot)` inside the UI render pass.
  - `RecordPrepared()` now treats prepared dynamic buffers as the draw path authority for global draw-list index/vertex offsets, fixing the split-path offset regression that the old device `Record()` path had hidden.
  - `RecordPrepared()` now rejects snapshots whose frame id or total vertex/index counts do not match the data prepared for that frame-resource slot.
  - `RecordPrepared()` additionally requires the same `UiDrawDataSnapshot` object that was passed to `PrepareFrameResources()`, preventing same-frame/same-count snapshot swaps from reusing stale prepared buffers.
  - Documented the current serial-only staging assumption for overlay CPU scratch buffers; `UIOverlay` remains non-parallel-eligible in the current scheduler.
  - Updated `data-model.md` so `hasVisibleDraws` matches implementation semantics: non-zero draw elements, recordable framebuffer size/scale, and no unsupported callback/texture ID.
  - Added `RHIUiOverlaySourceGuardTests.OverlayRendererDoesNotExposeRecordConveniencePath` so future code cannot reintroduce public `Record(...)` shortcuts.
- TDD red checks:
  - `Build\bin\Debug\NullusUnitTests.exe --gtest_filter=RHIUiOverlaySourceGuardTests.OverlayRendererDoesNotExposeRecordConveniencePath`
    - Result before implementation: failed because `RHIImGuiOverlayRenderer.h` still exposed a public `Record(` convenience path.
  - `Build\bin\Debug\NullusUnitTests.exe --gtest_filter=RHIUiOverlayPassTests.OverlayRendererCreatesBindsAndOffsetsDynamicVertexIndexBuffers:RHIUiOverlaySourceGuardTests.OverlayRendererDoesNotExposeRecordConveniencePath`
    - Result before implementation: failed because split `RecordPrepared()` emitted second draw-list `firstIndex=0` / `vertexOffset=0` instead of `3` / `3`, and the public device `Record()` signature was still present.
  - `Build\bin\Debug\NullusUnitTests.exe --gtest_filter=RHIUiOverlayPassTests.OverlayRendererRejectsMismatchedPreparedSnapshot`
    - Result before implementation: failed because `RecordPrepared()` accepted a different snapshot for an already-prepared frame-resource slot and recorded draws successfully.
  - `Build\bin\Debug\NullusUnitTests.exe --gtest_filter=RHIUiOverlayPassTests.OverlayRendererRejectsEquivalentButDifferentPreparedSnapshotObject`
    - Result covered the architecture-review P2 by requiring `RecordPrepared()` to reject an equivalent copied snapshot object for an already-prepared frame-resource slot.
- Build:
  - `F:\Microsoft Visual Studio\2022\MSBuild\Current\Bin\MSBuild.exe Build\Tests\Unit\NullusUnitTests.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:BuildInParallel=false /p:CL_MPCount=1 /m:1 /nr:false /v:m`
    - Result after implementation: passed.
  - `F:\Microsoft Visual Studio\2022\MSBuild\Current\Bin\MSBuild.exe Build\Runtime\Rendering\NLS_Render.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:BuildInParallel=false /p:CL_MPCount=1 /m:1 /nr:false /v:m`
    - Result after the final public-Record removal: passed.
  - `F:\Microsoft Visual Studio\2022\MSBuild\Current\Bin\MSBuild.exe Build\Tests\Unit\NullusUnitTests.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:BuildInParallel=false /p:CL_MPCount=1 /m:1 /nr:false /v:m`
    - Result after the final public-Record removal: passed.
  - A later constrained MSBuild command timed out while rebuilding the full unit target after the snapshot-identity/doc cleanup, but no MSBuild/CL process remained and the test executable was updated; the focused 47/47 test run below validated the resulting binary.
  - Note: earlier constrained MSBuild commands also timed out at 10 minutes during this work. One exited naturally after the timeout; another left a build-only MSBuild/CL pair compiling for more than 13 minutes, so those stale build processes were stopped before the successful retry.
- Focused green checks:
  - `Build\bin\Debug\NullusUnitTests.exe --gtest_filter=RHIUiOverlaySourceGuardTests.OverlayRendererDoesNotExposeRecordConveniencePath:RHIUiOverlayPassTests.OverlayRendererRejectsMismatchedPreparedSnapshot:RHIUiOverlayPassTests.OverlayRendererRecordsVisibleDrawCommandsThroughRhiCommandBuffer:RHIUiOverlayPassTests.OverlayRendererSkipsUnsupportedTextureIdCommands --gtest_break_on_failure=1`
    - Result: passed 4/4.
  - `Build\bin\Debug\NullusUnitTests.exe --gtest_filter=UiDrawDataSnapshotTests.*:RHIUiOverlayPassTests.*:RHIUiOverlaySourceGuardTests.*:ProfilerDestinationTest.TimelineTraceExporterCanFilterEditorUiNoise:ThreadedRenderingLifecycleTests.ThreadedUiOverlayPassRecordsSnapshotDrawsThroughRhiWorker`
    - Result after device-backed `Record()` removal: passed 45/45.
  - `Build\bin\Debug\NullusUnitTests.exe --gtest_filter=UiDrawDataSnapshotTests.*:RHIUiOverlayPassTests.*:RHIUiOverlaySourceGuardTests.*:ProfilerDestinationTest.TimelineTraceExporterCanFilterEditorUiNoise:ThreadedRenderingLifecycleTests.ThreadedUiOverlayPassRecordsSnapshotDrawsThroughRhiWorker`
    - Result after all public `Record(...)` removal and prepared-snapshot validation: passed 46/46.
  - `Build\bin\Debug\NullusUnitTests.exe --gtest_filter=RHIUiOverlayPassTests.OverlayRendererRejectsEquivalentButDifferentPreparedSnapshotObject:RHIUiOverlayPassTests.OverlayRendererRejectsMismatchedPreparedSnapshot:RHIUiOverlaySourceGuardTests.OverlayRendererDoesNotExposeRecordConveniencePath --gtest_break_on_failure=1`
    - Result after architecture-review P2 cleanup: passed 3/3.
  - `Build\bin\Debug\NullusUnitTests.exe --gtest_filter=UiDrawDataSnapshotTests.*:RHIUiOverlayPassTests.*:RHIUiOverlaySourceGuardTests.*:ProfilerDestinationTest.TimelineTraceExporterCanFilterEditorUiNoise:ThreadedRenderingLifecycleTests.ThreadedUiOverlayPassRecordsSnapshotDrawsThroughRhiWorker`
    - Result after architecture-review P2 cleanup: passed 47/47.
- Sanity checks:
  - `git diff --name-only -- "Runtime/*/Gen/*" "Project/*/Gen/*"` returned no generated file edits.
  - `git diff --check` reported only CRLF conversion warnings; no whitespace errors.
- Review status:
  - GPU/RHI final review: 0 P0/P1; remaining P2 is the known T028 placeholder-level swapchain concrete-resource graph metadata gap.
  - Code-quality final review: 0 P0/P1 after public `Record(...)` removal and prepared snapshot validation; untracked/intent-to-add files are a commit/PR packaging gate, not a no-commit continuation blocker.
  - Industry/reference final review: 0 P0/P1/P2; no UE4.27/D3D12, FPS, wait, or runtime-selectable overclaim found.
  - Architecture/performance final review: 0 P0/P1; its P2 findings for snapshot identity validation and stale `hasVisibleDraws` documentation were addressed, and the serial-only staging assumption is now documented in code.
  - Current no-commit packaging note: newly-added feature files are marked intent-to-add so review diffs include them. They still must be normally staged before any commit/PR sign-off.
- Remaining scoped US1 limitations:
  - T028/T029/T030 remain unchecked. This closure proves the dynamic-buffer prepare/record split and frame-slot ownership, not full shader pipeline, font atlas, registered texture binding, atlas graph resource ranges, or DX12 runtime-selectable enablement.

## 2026-06-13 - US1 Overlay Pipeline Binding Step

- Implementation progress:
  - `RHIImGuiOverlayRenderer` now requests and caches a renderer-owned graphics pipeline before recording prepared overlay draws.
  - Prepared overlay draw recording binds that pipeline before issuing indexed draws, and rejects the prepared draw path if pipeline creation fails.
  - The threaded UI overlay integration test now exercises the pipeline creation/bind path on the actual worker-submitted overlay frame.
- Build:
  - Command: `F:\Microsoft Visual Studio\2022\MSBuild\Current\Bin\MSBuild.exe Build\Tests\Unit\NullusUnitTests.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:BuildInParallel=false /p:CL_MPCount=1 /m:1 /nr:false /v:m`
  - Result: passed.
- Focused overlay pipeline checks:
  - `Build\bin\Debug\NullusUnitTests.exe --gtest_filter=RHIUiOverlayPassTests.OverlayRendererRejectsPreparedDrawsWithoutGraphicsPipeline:RHIUiOverlayPassTests.OverlayRendererBindsGraphicsPipelineBeforePreparedDraws`
    - Result: passed 2/2.
  - `Build\bin\Debug\NullusUnitTests.exe --gtest_filter=RHIUiOverlayPassTests.*`
    - Result: passed 37/37.
  - `Build\bin\Debug\NullusUnitTests.exe --gtest_filter=ThreadedRenderingLifecycleTests.ThreadedUiOverlayPassRecordsSnapshotDrawsThroughRhiWorker`
    - Result: passed 1/1.
  - `Build\bin\Debug\NullusUnitTests.exe --gtest_filter=UiDrawDataSnapshotTests.*:RHIUiOverlayPassTests.*:RHIUiOverlaySourceGuardTests.*:ProfilerDestinationTest.TimelineTraceExporterCanFilterEditorUiNoise:ThreadedRenderingLifecycleTests.ThreadedUiOverlayPassRecordsSnapshotDrawsThroughRhiWorker`
    - Result: passed 49/49.
- Sanity checks:
  - `git diff --check` reported only CRLF conversion warnings; no whitespace errors.
  - `git diff --name-only -- "Runtime/*/Gen/*" "Project/*/Gen/*"` returned no generated file edits.
- Remaining scoped US1 limitations:
  - T029 still lacks shader artifact-backed concrete pipeline creation, font atlas first-use upload/binding, and registered texture binding. This step only closes the overlay pipeline request/cache/bind contract.

## 2026-06-14 - US1 Overlay Pipeline Layout And Shader Module Step

- Implementation progress:
  - `RHIImGuiOverlayRenderer` now creates and caches an explicit RHI pipeline layout before graphics pipeline creation.
  - Overlay pipeline creation now creates vertex and fragment shader modules first and passes non-null `pipelineLayout`, `vertexShader`, and `fragmentShader` into `RHIGraphicsPipelineDesc`.
  - Pipeline preparation fails closed before dynamic-buffer upload if pipeline layout, vertex shader module, fragment shader module, or graphics pipeline creation fails.
  - `ThreadedUiOverlayPassRecordsSnapshotDrawsThroughRhiWorker` now validates the same layout/module/graphics-pipeline contract on the worker-submitted overlay frame.
- TDD red checks:
  - `Build\bin\Debug\NullusUnitTests.exe --gtest_filter=RHIUiOverlayPassTests.OverlayRendererRejectsPreparedDrawsWhenPipelineLayoutCreationFails:RHIUiOverlayPassTests.OverlayRendererRejectsPreparedDrawsWhenVertexShaderModuleCreationFails:RHIUiOverlayPassTests.OverlayRendererRejectsPreparedDrawsWhenFragmentShaderModuleCreationFails:RHIUiOverlayPassTests.OverlayRendererBindsGraphicsPipelineBeforePreparedDraws`
    - Result before implementation: failed 4/4 because `EnsureGraphicsPipeline()` did not call `CreatePipelineLayout()` or `CreateShaderModule()`.
- Build:
  - `F:\Microsoft Visual Studio\2022\MSBuild\Current\Bin\MSBuild.exe Build\Runtime\Rendering\NLS_Render.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:BuildInParallel=false /p:CL_MPCount=8 /m:1 /nr:false /v:m`
    - Result: passed.
  - `F:\Microsoft Visual Studio\2022\MSBuild\Current\Bin\MSBuild.exe Build\Tests\Unit\NullusUnitTests.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:BuildInParallel=false /p:CL_MPCount=8 /m:1 /nr:false /v:m /p:BuildProjectReferences=false`
    - Result: passed.
  - Note: two full `NullusUnitTests.vcxproj` builds with project references timed out while rebuilding unrelated `EditorProject` translation units. Stale build-only `MSBuild`/`CL` processes were stopped; `NLS_Render` was then built directly, and the test target was rebuilt with `BuildProjectReferences=false`.
- Focused green checks:
  - `Build\bin\Debug\NullusUnitTests.exe --gtest_filter=RHIUiOverlayPassTests.OverlayRendererRejectsPreparedDrawsWhenPipelineLayoutCreationFails:RHIUiOverlayPassTests.OverlayRendererRejectsPreparedDrawsWhenVertexShaderModuleCreationFails:RHIUiOverlayPassTests.OverlayRendererRejectsPreparedDrawsWhenFragmentShaderModuleCreationFails:RHIUiOverlayPassTests.OverlayRendererBindsGraphicsPipelineBeforePreparedDraws`
    - Result: passed 4/4.
  - `Build\bin\Debug\NullusUnitTests.exe --gtest_filter=ThreadedRenderingLifecycleTests.ThreadedUiOverlayPassRecordsSnapshotDrawsThroughRhiWorker`
    - Result after updating the worker fake RHI device to support layout/module creation: passed 1/1.
  - `Build\bin\Debug\NullusUnitTests.exe --gtest_filter=UiDrawDataSnapshotTests.*:RHIUiOverlayPassTests.*:RHIUiOverlaySourceGuardTests.*:ProfilerDestinationTest.TimelineTraceExporterCanFilterEditorUiNoise:ThreadedRenderingLifecycleTests.ThreadedUiOverlayPassRecordsSnapshotDrawsThroughRhiWorker`
    - Result: passed 52/52.
- Sanity checks:
  - `git diff --check` reported only CRLF conversion warnings; no whitespace errors.
  - `git diff --name-only -- "Runtime/*/Gen/*" "Project/*/Gen/*"` returned no generated file edits.
- Review fix:
  - Initial `/plan-review` attack found a P1 device-lifetime issue: the overlay renderer cached pipeline/layout/shader modules without tracking the owning `RHIDevice`, so a device rebuild or backend switch could reuse old-device RHI objects.
  - A deeper review pass found the paired P1: dynamic vertex/index buffers were also cached only by frame-resource slot, so they could be reused across device rebuilds. The same regression test now covers pipeline objects and dynamic buffers.
  - Added `RHIUiOverlayPassTests.OverlayRendererRebuildsGraphicsPipelineWhenDeviceChanges`, verified it failed before each fix, and fixed the renderer by keying cached pipeline objects and dynamic frame-slot buffers with `RHIDevice::GetCacheIdentity()` and resetting them when the device changes.
  - Regression command: `Build\bin\Debug\NullusUnitTests.exe --gtest_filter=RHIUiOverlayPassTests.OverlayRendererRebuildsGraphicsPipelineWhenDeviceChanges`
    - Result after fix: passed 1/1.
  - Focused regression command after fix: `Build\bin\Debug\NullusUnitTests.exe --gtest_filter=UiDrawDataSnapshotTests.*:RHIUiOverlayPassTests.*:RHIUiOverlaySourceGuardTests.*:ProfilerDestinationTest.TimelineTraceExporterCanFilterEditorUiNoise:ThreadedRenderingLifecycleTests.ThreadedUiOverlayPassRecordsSnapshotDrawsThroughRhiWorker`
    - Result after fix: passed 53/53.
- Remaining scoped US1 limitations:
  - The new shader module descriptors still use placeholder non-empty bytecode to satisfy the RHI module/pipeline contract in fake-device tests. This is not the ShaderArtifact/ShaderManager-backed concrete DX12 PSO path and must be replaced before T029 can be checked complete.
  - Font atlas first-use upload/binding, registered texture binding, atlas/registered texture graph resource ranges, and DX12 runtime-selectable enablement remain pending.

## 2026-06-14 - US1 Overlay Shader Artifact Step

- Implementation progress:
  - `RHIImGuiOverlayRenderer` now resolves `:Shaders/RHIImGuiOverlay.hlsl` through `ShaderManager` and creates overlay vertex/pixel modules from `ShaderArtifact` compiled stages instead of placeholder bytecode.
  - Added `App/Assets/Engine/Shaders/RHIImGuiOverlay.hlsl` as the engine overlay shader source.
  - Overlay pipeline layout now declares a 16-byte vertex push-constant range for projection scale/translate, and prepared recording pushes those constants before indexed draws.
  - `Shader::GetOrCreateExplicitShaderModule()` now prefers `RHIDevice::GetNativeDeviceInfo().backend` before adapter fallback so explicit devices with selected native backend but generic test adapters still resolve the correct artifact target platform.
- TDD red check:
  - `Build\bin\Debug\NullusUnitTests.exe --gtest_filter=RHIUiOverlayPassTests.OverlayRendererBindsGraphicsPipelineBeforePreparedDraws --gtest_break_on_failure=0`
    - Result before implementation: failed because both shader module descriptors still had `shaderToolchainFingerprint == "RHIImGuiOverlay:placeholder"` and lacked DXIL artifact path/profile fingerprints.
- Build:
  - `F:\Microsoft Visual Studio\2022\MSBuild\Current\Bin\MSBuild.exe Build\Runtime\Rendering\NLS_Render.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:BuildInParallel=false /p:CL_MPCount=8 /m:1 /nr:false /v:m /p:BuildProjectReferences=false`
    - Result: passed.
  - `F:\Microsoft Visual Studio\2022\MSBuild\Current\Bin\MSBuild.exe Build\Tests\Unit\NullusUnitTests.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:BuildInParallel=false /p:CL_MPCount=8 /m:1 /nr:false /v:m /p:BuildProjectReferences=false`
    - Result: passed.
- Focused green checks:
  - `Build\bin\Debug\NullusUnitTests.exe --gtest_filter=RHIUiOverlayPassTests.OverlayRendererBindsGraphicsPipelineBeforePreparedDraws --gtest_break_on_failure=0`
    - Result: passed 1/1.
  - `Build\bin\Debug\NullusUnitTests.exe --gtest_filter=RHIUiOverlayPassTests.*:ThreadedRenderingLifecycleTests.ThreadedUiOverlayPassRecordsSnapshotDrawsThroughRhiWorker --gtest_break_on_failure=0`
    - Result: passed 42/42.
- Remaining scoped US1 limitations:
  - T029 remains unchecked: font atlas first-use upload/binding and registered texture binding are still pending.
  - T028/T030 atlas and registered UI texture graph-visible shader-read ranges/upload-to-read visibility remain pending.
  - No DX12 runtime-selectable enablement or FPS/wait improvement claim is made from this unit-test slice.

## 2026-06-14 - US1 Font Atlas First-Use Binding Step

- Implementation progress:
  - `RHIImGuiFontAtlas` now builds the ImGui RGBA32 font atlas on first overlay prepare, creates an RHI texture, texture view, linear clamp sampler, and binding set, and retains them until explicit invalidation/retirement.
  - `RHIImGuiOverlayRenderer` now creates a font atlas texture/sampler binding layout in the overlay pipeline layout, ensures the font atlas is uploaded before dynamic draw-buffer preparation, and binds the font atlas binding set before prepared indexed draws.
  - `RHIImGuiOverlay.hlsl` now samples the font atlas in the pixel shader instead of outputting vertex color only.
- TDD red check:
  - `Build\bin\Debug\NullusUnitTests.exe --gtest_filter=RHIUiOverlayPassTests.OverlayRendererUploadsAndBindsFontAtlasOnFirstUse --gtest_break_on_failure=0`
    - Result before implementation: failed because `FontAtlas().IsUploaded()` was false, no font atlas texture/view/sampler/binding set was created, and no binding set was bound before draws.
- Build:
  - `F:\Microsoft Visual Studio\2022\MSBuild\Current\Bin\MSBuild.exe Build\Runtime\Rendering\NLS_Render.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:BuildInParallel=false /p:CL_MPCount=8 /m:1 /nr:false /v:m /p:BuildProjectReferences=false`
    - Result: passed.
  - `F:\Microsoft Visual Studio\2022\MSBuild\Current\Bin\MSBuild.exe Build\Tests\Unit\NullusUnitTests.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:BuildInParallel=false /p:CL_MPCount=8 /m:1 /nr:false /v:m /p:BuildProjectReferences=false`
    - Result: passed.
- Focused green checks:
  - `Build\bin\Debug\NullusUnitTests.exe --gtest_filter=RHIUiOverlayPassTests.OverlayRendererUploadsAndBindsFontAtlasOnFirstUse --gtest_break_on_failure=0`
    - Result: passed 1/1.
  - `Build\bin\Debug\NullusUnitTests.exe --gtest_filter=ThreadedRenderingLifecycleTests.ThreadedUiOverlayPassRecordsSnapshotDrawsThroughRhiWorker --gtest_break_on_failure=0`
    - Result after updating the worker fake RHI device for font atlas binding layout/sampler support: passed 1/1.
  - `Build\bin\Debug\NullusUnitTests.exe --gtest_filter=UiDrawDataSnapshotTests.*:RHIUiOverlayPassTests.*:RHIUiOverlaySourceGuardTests.*:ProfilerDestinationTest.TimelineTraceExporterCanFilterEditorUiNoise:ThreadedRenderingLifecycleTests.ThreadedUiOverlayPassRecordsSnapshotDrawsThroughRhiWorker --gtest_break_on_failure=0`
    - Result: passed 54/54.
- Sanity checks:
  - `git diff --name-only -- "Runtime/*/Gen/*" "Project/*/Gen/*"` returned no generated file edits.
  - `git diff --check` reported only CRLF conversion warnings; no whitespace errors.
- Remaining scoped US1 limitations:
  - T029 remains unchecked because registered UI texture binding is still pending.
  - T028/T030 atlas and registered UI texture graph-visible shader-read ranges/upload-to-read visibility remain pending.
  - No DX12 runtime-selectable enablement or FPS/wait improvement claim is made from this unit-test slice.

## 2026-06-14 - US1 Registered Texture Binding And Prepared Resource Visibility Step

- Implementation progress:
  - `RHIImGuiTextureRegistry` now creates/caches RHI binding sets for registered `UiTextureId` entries using the same overlay texture/sampler binding layout as the font atlas.
  - `RHIImGuiOverlayRenderer` now resolves registered texture IDs with `ResolveForFrame()`, binds font or registered texture sets per draw, skips unresolved/stale texture commands, and avoids native descriptor handles on the migrated recording path.
  - The renderer exposes prepared dynamic vertex/index buffers, the prepared font atlas view, and registered texture views so `RhiThreadCoordinator` can enrich the effective `UIOverlay` pass input with concrete RHI resource access records after resource preparation.
  - All UI overlay recording paths in `RhiThreadCoordinator` now add concrete vertex/index read accesses, font atlas shader-read texture access plus copy/upload-to-shader-read visibility, and registered texture shader-read accesses before `BeginPassCommandPlan`.
  - Review fix: font atlas copy/upload-to-shader-read visibility is now flagged only when the atlas upload/rebuild actually happened, avoiding repeated `CopyDst -> ShaderRead` transitions on atlas reuse frames.
- TDD red check:
  - `Build\bin\Debug\NullusUnitTests.exe --gtest_filter=RHIUiOverlayPassTests.OverlayRendererBindsRegisteredTexturePerDrawWithoutNativeDescriptors --gtest_break_on_failure=0`
    - Result before implementation: failed because only the font atlas binding set was created; no registered UI texture binding set was created or bound.
- Build:
  - `F:\Microsoft Visual Studio\2022\MSBuild\Current\Bin\MSBuild.exe Build\Runtime\Rendering\NLS_Render.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:BuildInParallel=false /p:CL_MPCount=8 /m:1 /nr:false /v:m /p:BuildProjectReferences=false`
    - Result: passed.
  - `F:\Microsoft Visual Studio\2022\MSBuild\Current\Bin\MSBuild.exe Build\Tests\Unit\NullusUnitTests.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:BuildInParallel=false /p:CL_MPCount=8 /m:1 /nr:false /v:m /p:BuildProjectReferences=false`
    - Result: passed.
- Focused green checks:
  - `Build\bin\Debug\NullusUnitTests.exe --gtest_filter=RHIUiOverlayPassTests.OverlayRendererBindsRegisteredTexturePerDrawWithoutNativeDescriptors --gtest_break_on_failure=0`
    - Result: passed 1/1.
  - `Build\bin\Debug\NullusUnitTests.exe --gtest_filter=RHIUiOverlayPassTests.OverlayRendererExposesPreparedResourcesForFrameGraphVisibility --gtest_break_on_failure=0`
    - Result: passed 1/1.
  - `Build\bin\Debug\NullusUnitTests.exe --gtest_filter=RHIUiOverlayPassTests.OverlayRendererRequiresFontAtlasUploadVisibilityOnlyOnFirstUpload:RHIUiOverlayPassTests.OverlayRendererExposesPreparedResourcesForFrameGraphVisibility --gtest_break_on_failure=0`
    - Result after review fix: passed 2/2.
  - `Build\bin\Debug\NullusUnitTests.exe --gtest_filter=UiDrawDataSnapshotTests.*:RHIUiOverlayPassTests.*:RHIUiTextureRegistryTests.*:RHIUiOverlaySourceGuardTests.*:ProfilerDestinationTest.TimelineTraceExporterCanFilterEditorUiNoise:ThreadedRenderingLifecycleTests.ThreadedUiOverlayPassRecordsSnapshotDrawsThroughRhiWorker --gtest_break_on_failure=0`
    - Result before review fix: passed 65/65.
    - Result after review fix: passed 66/66.
- Remaining scoped US1 limitations:
  - T028 remains partially open at package-build time because font/registered texture resources are only concrete after overlay resource preparation; the concrete prepared-resource visibility path is now in `RhiThreadCoordinator`.
  - DX12 product routing remains not runtime-selectable. No FPS/wait-count improvement claim is made without the planned 300-frame TimelineProfiler and RenderDoc/product evidence.
- Plan-review summary:
  - R1 found one GPU state-model issue: font atlas upload-to-read visibility was initially declared every prepared frame instead of only on atlas upload/rebuild frames.
  - Fix added `fontAtlasUploadTransitionRequired` to the prepared resource snapshot and gated the `CopyDst -> ShaderRead` transition on that flag.
  - R2 after the fix: 0 P0/P1 in the T029/T030 slice. Remaining P2/scoped limitation is that T028 package-build-time resource ranges stay partial because font/registered texture resources become concrete only after overlay resource preparation.

## 2026-06-14 - US1 Direct-Submit Guard And Focused Validation Closure

- Implementation progress:
  - `DX12UIBridge::RenderDrawData()` now rejects the legacy DX12 direct-submit path before `DriverUIAccess::PrepareUIRender()`, allocator reuse waits, or `DX12UIBridge::ExecuteCommandLists` when the active RHI device reports `RHIDeviceFeature::UIOverlayFrameGraph` as supported.
  - `DX12UIBridge::SubmitCommandBuffer()` shares the same migrated-path guard.
  - `CreateRHIUIBridge()` now checks `UIOverlayFrameGraph`; supported overlay devices get a `NullUIBridge` so `RHIFrameGraph::UIOverlay` owns rendering, while unsupported devices log the capability reason before allowing the legacy fallback.
  - `UIManager::BeginFrame()` now allows a null/non-renderer bridge when `ShouldPublishUiSnapshotToFrameGraph()` is true, so migrated UI frame generation is not short-circuited by the intentionally null legacy bridge.
  - Added source guards for the DX12 bridge gate, bridge factory null/fallback selection, `UIManager::BeginFrame()` migrated-path behavior, and RHI-thread prepared-resource pass-input enrichment.
- TDD red checks:
  - `Build\bin\Debug\NullusUnitTests.exe --gtest_filter=RHIUiOverlaySourceGuardTests.DX12LegacyBridgeRejectsDirectSubmitWhenFrameGraphOverlayIsSupported:RHIUiOverlaySourceGuardTests.RHIUIBridgeFactoryReturnsNullBridgeForSupportedFrameGraphOverlay --gtest_break_on_failure=0`
    - Result before implementation: failed 2/2 because the DX12 bridge direct-submit guard and bridge factory overlay capability check were missing.
  - `Build\bin\Debug\NullusUnitTests.exe --gtest_filter=RHIUiOverlaySourceGuardTests.UIManagerBeginFrameAllowsNullRendererBridgeOnFrameGraphOverlayPath --gtest_break_on_failure=0`
    - Result before implementation: failed because `UIManager::BeginFrame()` skipped ImGui frames when the bridge had no renderer backend, even on the overlay FrameGraph path.
- Build:
  - `F:\Microsoft Visual Studio\2022\MSBuild\Current\Bin\MSBuild.exe Build\Runtime\Rendering\NLS_Render.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:BuildInParallel=false /p:CL_MPCount=8 /m:1 /nr:false /v:m /p:BuildProjectReferences=false`
    - Result: passed.
  - `F:\Microsoft Visual Studio\2022\MSBuild\Current\Bin\MSBuild.exe Build\Runtime\UI\NLS_UI.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:BuildInParallel=false /p:CL_MPCount=8 /m:1 /nr:false /v:m /p:BuildProjectReferences=false`
    - Result: passed.
  - `F:\Microsoft Visual Studio\2022\MSBuild\Current\Bin\MSBuild.exe Build\Tests\Unit\NullusUnitTests.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:BuildInParallel=false /p:CL_MPCount=8 /m:1 /nr:false /v:m /p:BuildProjectReferences=false`
    - Result: passed.
- Focused green checks:
  - `Build\bin\Debug\NullusUnitTests.exe --gtest_filter=RHIUiOverlaySourceGuardTests.DX12LegacyBridgeRejectsDirectSubmitWhenFrameGraphOverlayIsSupported:RHIUiOverlaySourceGuardTests.RHIUIBridgeFactoryReturnsNullBridgeForSupportedFrameGraphOverlay --gtest_break_on_failure=0`
    - Result: passed 2/2.
  - `Build\bin\Debug\NullusUnitTests.exe --gtest_filter=RHIUiOverlaySourceGuardTests.UIManagerBeginFrameAllowsNullRendererBridgeOnFrameGraphOverlayPath --gtest_break_on_failure=0`
    - Result after fix: passed as part of the source guard suite.
  - `Build\bin\Debug\NullusUnitTests.exe --gtest_filter=RHIUiOverlayPassTests.RhiThreadUiOverlayPassInputAddsPreparedConcreteResourcesBeforeBeginPass --gtest_break_on_failure=0`
    - Result: passed 1/1.
  - `Build\bin\Debug\NullusUnitTests.exe --gtest_filter=UiDrawDataSnapshotTests.*:RHIUiOverlayPassTests.*:RHIUiTextureRegistryTests.*:RHIUiOverlaySourceGuardTests.*:ProfilerDestinationTest.TimelineTraceExporterCanFilterEditorUiNoise:ThreadedRenderingLifecycleTests.ThreadedUiOverlayPassRecordsSnapshotDrawsThroughRhiWorker --gtest_break_on_failure=0`
    - Result: passed 70/70.
- US1 coverage status:
  - Overlay ordering: `VisibleUiSnapshotAppendsFinalOverlayPassAfterScenePass`.
  - Swapchain access/export/dependency: `UiOverlayPassDeclaresSwapchainAccessPresentTransitionAndDependencyEdge` and `UiOverlayDependencyEdgeUsesSceneSwapchainWriterWhenScenePassHasMultipleTextureAccesses`.
  - Font atlas MVP: `OverlayRendererUploadsAndBindsFontAtlasOnFirstUse` and `OverlayRendererRequiresFontAtlasUploadVisibilityOnlyOnFirstUpload`.
  - Registered texture/resource visibility: `OverlayRendererBindsRegisteredTexturePerDrawWithoutNativeDescriptors`, `OverlayRendererExposesPreparedResourcesForFrameGraphVisibility`, and `RhiThreadUiOverlayPassInputAddsPreparedConcreteResourcesBeforeBeginPass`.
  - Direct-submit exclusion: `DX12LegacyBridgeRejectsDirectSubmitWhenFrameGraphOverlayIsSupported`, `RHIUIBridgeFactoryReturnsNullBridgeForSupportedFrameGraphOverlay`, and the migrated UI source guard suite.
- Scoped limitations:
  - DX12 `UIOverlayFrameGraph` remains intentionally not runtime-selectable until US2 UI-only routing and US3 texture/font retention validation finish.
  - This is unit/source/build evidence only. No FPS, wait-count, submit-count, or runtime visual claim is made without the planned 300-frame TimelineProfiler, RenderDoc, and product smoke evidence.

## 2026-06-14 - US2 And US3 Runtime-Selectable Unit Validation

- US2 implementation:
  - `DriverUIAccess::PublishUiOnlyFrame()` now publishes a normal threaded swapchain package containing only `RHIFrameGraph::UIOverlay`.
  - `RhiThreadCoordinator::PrepareUIRender()` skips legacy `BeginStandaloneUiExplicitFrame` when `UIOverlayFrameGraph` is supported and routes UI-only work through normal prepared-frame publication.
  - Resize during UI-only frames waits for normal threaded frame retirement before applying the pending swapchain resize.
- US3 implementation:
  - `UIManager::ResolveTextureView()` registers migrated overlay texture views through `DriverUIAccess::RegisterUiTextureView()` and returns packed stable `UiTextureId` identities for ImGui.
  - `Image` and `ButtonImage` prefer packed UI texture identity values before falling back to native handles.
  - `ReleaseTextureViewHandle`, `NotifySwapchainWillResize`, and font rebuild notification now route through overlay resource hooks on the migrated path.
  - `RHIImGuiOverlayRenderer` binds registered texture binding sets per draw without native DX12 descriptor handles, and font atlas resources retire by UI frame id.
  - DX12 `RHIDeviceFeature::UIOverlayFrameGraph` is now reported supported with an explicit `US1/US2/US3` validation reason.
- Builds:
  - `F:\Microsoft Visual Studio\2022\MSBuild\Current\Bin\MSBuild.exe Build\Runtime\Rendering\NLS_Render.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:BuildInParallel=false /p:CL_MPCount=8 /m:1 /nr:false /v:m /p:BuildProjectReferences=false`
    - Result: passed.
  - `F:\Microsoft Visual Studio\2022\MSBuild\Current\Bin\MSBuild.exe Build\Runtime\UI\NLS_UI.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:BuildInParallel=false /p:CL_MPCount=8 /m:1 /nr:false /v:m /p:BuildProjectReferences=false`
    - Result: passed.
  - `F:\Microsoft Visual Studio\2022\MSBuild\Current\Bin\MSBuild.exe Build\Tests\Unit\NullusUnitTests.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:BuildInParallel=false /p:CL_MPCount=8 /m:1 /nr:false /v:m /p:BuildProjectReferences=false`
    - Result: passed.
- TDD red checks:
  - `RHIUiOverlaySourceGuardTests.UIManagerResolveTextureViewUsesPackedUiTextureIdentityOnMigratedPath:RHIUiOverlaySourceGuardTests.ImageWidgetsPreferPackedUiTextureIdentityWhenAvailable`
    - Result before implementation: failed because `UIManager::ResolveTextureView()` did not call `DriverUIAccess::RegisterUiTextureView()`/`PackUiTextureIdForImGui()`, and `Image`/`ButtonImage` did not consume `nativeHandle.value`.
  - `RHIUiOverlaySourceGuardTests.UIManagerResourceNotificationsRouteThroughFrameGraphOverlayResources:RHIUiOverlaySourceGuardTests.DriverUIAccessExposesUiOverlayResourceLifecycleHooks`
    - Result before implementation: failed because `UIManager` resource notifications still routed only through the legacy UI bridge and `DriverUIAccess` had no overlay resource lifecycle hooks.
  - `RHIUiOverlayPassTests.DX12CapabilityPlumbingMarksUiOverlayRuntimeSelectableAfterUS3Validation`
    - Result before implementation: failed because DX12 still reported `UIOverlayFrameGraph.supported == false` with the prior not-runtime-selectable reason.
- Focused green checks:
  - `Build\bin\Debug\NullusUnitTests.exe --gtest_filter=RHIUiOverlaySourceGuardTests.UIManagerResolveTextureViewUsesPackedUiTextureIdentityOnMigratedPath:RHIUiOverlaySourceGuardTests.ImageWidgetsPreferPackedUiTextureIdentityWhenAvailable --gtest_break_on_failure=0`
    - Result: passed 2/2.
  - `Build\bin\Debug\NullusUnitTests.exe --gtest_filter=RHIUiOverlaySourceGuardTests.UIManagerResourceNotificationsRouteThroughFrameGraphOverlayResources:RHIUiOverlaySourceGuardTests.DriverUIAccessExposesUiOverlayResourceLifecycleHooks --gtest_break_on_failure=0`
    - Result: passed 2/2.
  - `Build\bin\Debug\NullusUnitTests.exe --gtest_filter=RHIUiTextureRegistryTests.*:RHIUiOverlayPassTests.OverlayRendererBindsRegisteredTexturePerDrawWithoutNativeDescriptors:RHIUiOverlayPassTests.OverlayRendererExposesPreparedResourcesForFrameGraphVisibility:RHIUiOverlayPassTests.OverlayRendererRequiresFontAtlasUploadVisibilityOnlyOnFirstUpload:RHIUiOverlayPassTests.FontAtlasInvalidateRetiresResourcesUntilFrameCompletion:RHIUiOverlaySourceGuardTests.* --gtest_break_on_failure=0`
    - Result: passed 25/25.
  - `Build\bin\Debug\NullusUnitTests.exe --gtest_filter=RHIUiOverlayPassTests.DX12CapabilityPlumbingMarksUiOverlayRuntimeSelectableAfterUS3Validation --gtest_break_on_failure=0`
    - Result: passed 1/1.
  - `Build\bin\Debug\NullusUnitTests.exe --gtest_filter=UiDrawDataSnapshotTests.*:RHIUiOverlayPassTests.*:RHIUiTextureRegistryTests.*:RHIUiOverlaySourceGuardTests.*:ProfilerDestinationTest.TimelineTraceExporterCanFilterEditorUiNoise:ThreadedRenderingLifecycleTests.ThreadedUiOverlayPassRecordsSnapshotDrawsThroughRhiWorker:ThreadedRenderingLifecycleTests.UiOnlyFramePublishesOverlayOnlySwapchainPackageAndUsesSingleSubmitPresent:ThreadedRenderingLifecycleTests.ResizeDuringUiOnlyFrameWaitsForNormalFrameRetirement --gtest_break_on_failure=0`
    - Result: passed 77/77.
- Scoped limitations:
  - This remains automated unit/source/build evidence. The planned 300-frame DX12 TimelineProfiler, RenderDoc capture, and product smoke evidence are still required in Phase 6 before final sign-off.

## 2026-06-14 - Phase 6 Quickstart Targeted Validation

- Quickstart migration notes:
  - `specs/049-integrate-ui-framegraph/quickstart.md` was reviewed after US1-US3 completion. The targeted unit-test command, source guard grep, TimelineProfiler evidence fields, RenderDoc command, and product smoke checklist still match the current migrated UI overlay scope.
- Full targeted quickstart command:
  - `Build\bin\Debug\NullusUnitTests.exe --gtest_filter=UiDrawDataSnapshotTests.*:RHIUiOverlayPassTests.*:RHIUiTextureRegistryTests.*:ThreadedRenderingLifecycleTests.*:FrameGraphSceneTargetsTests.*:ProfilerDestinationTest.*:RHIUiOverlaySourceGuardTests.* --gtest_break_on_failure=1`
  - Result: passed 363/363, exit code 0, total GoogleTest time approximately 10.0 seconds.
  - Note: output included expected test-injected `ERROR` logs for unsupported backend requests, simulated fence/resize timeouts, simulated submit/draw failures, and OpenGL shader artifact failures. These are covered failure-path tests; the process exited successfully.
- Source guard grep command:
  - `rg -n "WaitForBackbufferReuse|WaitForAllocatorReuse|DX12UIBridge::ExecuteCommandLists|DX12UIBridge::SubmitCommandBuffer|m_uiBridge->RenderDrawData|SubmitUIRendering|PrepareUIRender|BeginStandaloneUiExplicitFrame|PresentStandaloneUiFrame|ExecuteCommandLists\(|->ExecuteCommandLists|ID3D12CommandQueue::Signal|Signal\(|m_queue->Signal|m_graphicsQueue->Signal|PresentInternal|->Present\(|\.Present\(|swapchain->Present" Runtime Project Tests`
  - Result: expected hits only after call-path triage.
  - Legacy fallback hits remain in `Runtime/Rendering/RHI/Backends/DX12/DX12UIBridge.cpp`, guarded by `RejectLegacyDirectSubmitWhenFrameGraphOverlayIsSupported(...)` and used for unsupported/fallback paths.
  - Product call-site hits remain in `Project/Editor/Core/Editor.cpp`, `Project/Launcher/Core/Launcher.cpp`, and `Runtime/UI/UIManager.cpp` because fallback `SubmitUIRendering()`/`m_uiBridge->RenderDrawData(...)` still exists. Source guards cover that migrated DX12 `UIOverlayFrameGraph` branches publish snapshots or route UI-only frames instead of invoking the legacy submit path.
  - Normal RHI submit/present hits remain in `Runtime/Rendering/RHI/Backends/DX12/DX12Queue.cpp`; these are the intended frame-owned queue/present path.
  - Non-UI infrastructure hits remain in Profiler GPU resolve, readback utilities, resource/descriptor initialization, and tests; these are not the migrated UI overlay direct-submit path.
  - `RhiThreadCoordinator::BeginStandaloneUiExplicitFrame` and `PresentStandaloneUiFrame` remain for legacy/unsupported UI explicit-frame behavior. Source guards cover that `PrepareUIRender()` checks `UIOverlayFrameGraph` before considering the standalone path and publishes UI-only migrated frames through normal frame packages.
- Sanity checks:
  - `git diff --name-only -- "Runtime/*/Gen/*" "Project/*/Gen/*"` returned no generated file edits.
  - `git diff --check` reported only LF-to-CRLF conversion warnings; no whitespace errors were reported.

## 2026-06-14 - Phase 6 Regression Refresh

- Build/runtime package:
  - `F:\Microsoft Visual Studio\2022\MSBuild\Current\Bin\MSBuild.exe Build\Project\Editor\Editor.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:BuildInParallel=false /p:CL_MPCount=1 /m:1 /nr:false /v:m`
  - Result: passed and refreshed `App\Win64_Debug_Runtime_Shared\Editor.exe`, `NLS_Renderd.dll`, `NLS_UId.dll`, and dependent runtime DLLs.
- Machine/workload:
  - OS: Microsoft Windows 11 Pro, version `10.0.26200`, build `26200`.
  - CPU: 12th Gen Intel Core i7-12700KF, 12 cores / 20 logical processors.
  - GPU: NVIDIA GeForce RTX 3080 Ti, driver `32.0.15.9597` dated 2026-03-17.
  - Backend/build: DX12 Debug runtime package, Profiler panel opened by validation flag, Scene View focused, 300 exported TimelineProfiler frames.
- Latest DX12 Editor TimelineProfiler trace:
  - Command: `App\Win64_Debug_Runtime_Shared\Editor.exe --backend dx12 --no-renderdoc --editor-validation-focus-view scene --editor-validation-open-profiler --editor-validation-trace-frames 300 --editor-log-render-draw-path TestProject\TestProject.nullus`
  - Trace: `TestProject\Logs\trace.json`, last written 2026-06-14 16:46:06.
  - Log: `TestProject\Logs\2026-06-14_16-45-10.log`.
  - The trace exporter reported `exportedFrames=300`; the validation process did not auto-exit after trace export, so it was terminated by the validation harness after the export was detected.
  - Trace counts: `DX12UIBridge::WaitForBackbufferReuse=0`, `DX12UIBridge::WaitForAllocatorReuse=0`, `DX12UIBridge::RenderDrawData=0`, `DX12UIBridge::ExecuteCommandLists=0`, `DX12UIBridge::SubmitCommandBuffer=0`, `UIManager::SubmitUIRendering=0`.
  - Trace counts: `RHIFrameGraph::UIOverlay=523`; `RHIFrameGraph::UIOverlay avg=0.5384ms p95=0.7750ms max=3.5290ms`.
  - Trace counts: `CPU Frame=300`; `CPU Frame avg=75.3710ms p95=286.3200ms max=962.2890ms`. Startup/editor validation warmup frames remain included, so this is not a steady-state FPS claim.
  - Log submit/present counts during the run: `NativeDX12Queue::Submit: executing=394`, `NativeDX12Queue::Present: called=303`, and `uiSemaphore=0 uiSignalValue=0` on all 303 presents.
  - Log diagnostics: `CreateRHIUIBridge: UIOverlayFrameGraph is supported; returning null UI bridge so RHIFrameGraph::UIOverlay owns migrated UI rendering.` No `DX12UIBridge::`, `UIManager::SubmitUIRendering`, `device status`, `DEVICE_HUNG`, or `quarantine` hits were present in the latest trace log.
  - Before/after baseline note: the pre-migration direct-submit baseline is not reproduced from the old code in this working tree. The 049 acceptance comparison is therefore scoped to the current migrated run plus source/unit guard evidence; current migrated frames satisfy the "fewer than 1%" bridge wait target with 0 matching wait events over the exported run.
  - Snapshot-copy/dynamic-buffer note: the trace contains the UI render/snapshot path by callsite names but does not currently emit explicit copied vertex/index byte counters or dynamic-buffer reallocation counters. Effective dynamic vertex/index buffer resource visibility is covered by `RHIUiOverlayPassTests.RhiThreadUiOverlayPassInputAddsPreparedConcreteResourcesBeforeBeginPass`.
- Latest DX12 RenderDoc evidence:
  - Command: `py -3 Tools/RenderDoc/renderdoc_runner.py --target editor --backend dx12 --exe App\Win64_Debug_Runtime_Shared\Editor.exe --project TestProject\TestProject.nullus --capture --capture-after-frames 120 --terminate-after-capture --timeout 180 --app-arg=--editor-validation-focus-view --app-arg=scene --app-arg=--editor-validation-open-profiler`
  - Runner result: capture succeeded, process was terminated by the runner after the capture became stable.
  - Capture: `TestProject\Logs\RenderDoc\Editor\dx12\editor_dx12_DX12_capture.rdc`, last updated 2026-06-14 16:48:37.
  - Analysis command: `py -3 Tools/RenderDoc/rdc_analyze.py TestProject\Logs\RenderDoc\Editor\dx12\editor_dx12_DX12_capture.rdc --json-out Build\RenderDocAnalysis\ui-framegraph.json`
  - Analysis result: API `D3D12`; pass `Nullus/RHIFrameGraph::UIOverlay` from EID `24` to `170`; 35 indexed draws; 1415 triangles; focus draw PS binding includes `FontAtlasTexture`; `Present(ResourceId::320)` appears after the UI overlay pass.
  - Evidence scope: this proves the captured UI frame records ImGui draw work through the migrated RHI FrameGraph overlay pass and presents afterward. It is not used as sole proof of a full scene-pass -> overlay chain or registered UI texture subresource coverage in that capture; those resource-state contracts are covered by the unit tests and source guards listed above.
- Product smoke:
  - Editor DX12: latest trace and RenderDoc runs reached DX12 device creation, null legacy UI bridge selection, Profiler opening, overlay frame recording, submit, and present.
  - Launcher DX12 command: `App\Win64_Debug_Runtime_Shared\Launcher.exe --backend dx12`; the process remained running after 8 seconds and was terminated by the validation harness. Log `App\Win64_Debug_Runtime_Shared\2026-06-14_16-52-53.log` shows DX12 device/swapchain creation and `UIOverlayFrameGraph` null-bridge selection.
  - Game DX12 command: `App\Win64_Debug_Runtime_Shared\Game.exe --backend dx12 TestProject\TestProject.nullus`; the process remained running after 8 seconds and was terminated by the validation harness. Log `App\Win64_Debug_Runtime_Shared\2026-06-14_16-53-01.log` shows DX12 device/swapchain creation.
  - Unsupported backend diagnostics: `Launcher.exe --backend vulkan` exits 1 with `Launcher CLI only supports DX12 during UE5 alignment phase 1. Requested backend: Vulkan.`; `Game.exe --backend vulkan TestProject\TestProject.nullus` exits 1 with `Game CLI only supports DX12 during UE5 alignment phase 1. Requested backend: Vulkan.`
- Multi-agent review findings already fixed before this refresh:
  - Architecture/performance: no P0/P1; noted stale docs and optional pending-generation optimization as P2.
  - Industry/evidence: P1 documentation gap and RenderDoc overclaim risk; addressed here by recording precise trace/RenderDoc/product evidence and explicitly scoping RenderDoc claims.
  - Code quality/SSoT: P1 behavior coverage gaps; fixed with `ThreadedRenderingLifecycleTests.OffscreenPreparedBuilderLeavesPendingUiSnapshotForUiOnlyFrame`, `ThreadedRenderingLifecycleTests.PresentSwapchainDrainsPreparedSceneBeforeUiOnlyFallback`, and `UIAndToolingBackendAwarenessTests.UIManagerFontLoadInvalidatesDriverOwnedFrameGraphFontAtlas`.
  - GPU correctness: P1 barrier ordering and Present fallback ordering; fixed by appending prepared UI resources before `BeginPassCommandPlan` and draining already-published scene/UI work before UI-only fallback publication.

## 2026-06-14 - Review Gate Remediation Refresh

- Review finding fixes:
  - Fixed stale pending UI snapshot restoration by adding generation-aware consume/restore semantics. `TryPublishPreparedFrameBuilder()` now records the consumed pending generation and calls `RestoreConsumedUiDrawDataSnapshotIfUnchanged()` only when no newer pending snapshot replaced it.
  - Fixed font atlas invalidation safety between UI-side notification and RHI overlay record by serializing `RHIImGuiOverlayRenderer` state and by recording the prepared font-atlas binding set in the frame slot. `RecordPrepared()` now binds the prepared resource instead of rereading the mutable current atlas after invalidation.
  - Hardened `PresentSwapchain()` so UI-only fallback publication is skipped if the synchronous drain watchdog fails. Normal path review confirmed `TryDrainThreadedRendering()` loops until threaded lifecycle in-flight depth reaches zero, so already-published scene/UI frames are drained before UI-only fallback.
  - Added the untracked overlay shader asset `App/Assets/Engine/Shaders/RHIImGuiOverlay.hlsl` to the delivery set.
- TDD red/green evidence:
  - `ThreadedRenderingLifecycleTests.ConsumedUiSnapshotRestoreDoesNotOverwriteNewerPendingSnapshot` initially failed to compile until generation-aware consume/restore APIs were added; after the fix it passed 1/1.
  - `RHIUiOverlayPassTests.RecordPreparedKeepsPreparedFontAtlasBindingAfterInvalidation` initially failed because `RecordPrepared()` bound `nullptr` after atlas invalidation; after the fix it passed 1/1.
- Build:
  - `F:\Microsoft Visual Studio\2022\MSBuild\Current\Bin\MSBuild.exe Build\Tests\Unit\NullusUnitTests.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:BuildInParallel=false /p:CL_MPCount=1 /m:1 /nr:false /v:m`
  - Result: passed and produced `Build\bin\Debug\NullusUnitTests.exe` at 2026-06-14 17:50 local time.
- Focused validation:
  - `Build\bin\Debug\NullusUnitTests.exe --gtest_filter=UiDrawDataSnapshotTests.*:RHIUiOverlayPassTests.*:RHIUiTextureRegistryTests.*:ThreadedRenderingLifecycleTests.*:FrameGraphSceneTargetsTests.*:ProfilerDestinationTest.*:RHIUiOverlaySourceGuardTests.*:UIAndToolingBackendAwarenessTests.* --gtest_break_on_failure=1`
  - Result: passed 418/418, exit code 0, total GoogleTest time 6.880 seconds.
  - Note: output included expected test-injected `ERROR` logs for unsupported backend requests, simulated fence/resize timeouts, simulated submit/draw failures, and OpenGL shader artifact failures. These are negative-path tests; the process exited successfully.
- Documentation refresh:
  - `quickstart.md` and `final-diagnostics.md` now use the actual singular fixture name `ProfilerDestinationTest.*`.
- Sanity checks:
  - `git diff --name-only -- "Runtime/*/Gen/*" "Project/*/Gen/*"` returned no generated file edits.
  - `git diff --check` reported only LF-to-CRLF conversion warnings; no whitespace errors were reported.
- Full regression:
  - `Build\bin\Debug\NullusUnitTests.exe --gtest_break_on_failure=1`
  - Result: passed 2351/2352, skipped 1/2352 (`AssetDatabaseFacadeTests.ImportedAssimpModelManifestRecordsParserTextureDependencies`, FBX SDK dependency), exit code 0, total GoogleTest time 202.000 seconds.
  - Note: output included expected negative-path `ERROR` logs for invalid materials, descriptor exhaustion, zero-sized resources, unsupported backend requests, simulated fence/submit/draw failures, missing shader/image inputs, and GPU-resource quarantine tests. These are covered failure-path tests; the process exited successfully.

## 2026-06-14 - Final Review Gate Closure

- `plan-review` gate:
  - Auto-fail check: no P0/P1 runtime or packaging issue remained after review closure; the earlier shader delivery and unsupported-callback diagnostic concerns were fixed and rechecked.
  - Result: passed with scoped benchmark evidence. The closest local benchmark is UE4.27 `FParallelMeshDrawCommandPass::DispatchDraw` / `FParallelCommandListSet` and D3D12 bundle semantics for RHI command ownership, backend gates, explicit resource synchronization, and frame-retired lifetime. `research.md` explicitly records that this is an adjacent benchmark, not an exact UI overlay/present-ownership match.
  - Summary: 0 P0 and 0 blocking P1 after closure. Remaining notes are P2 scope/evidence trade-offs.
- Multi-agent review gate:
  - Architecture/performance: 0 P0, 0 P1.
  - GPU correctness/RHI lifetime: 0 P0, 0 P1.
  - Code quality/SSoT/test coverage: 0 P0, 0 P1.
  - Industry/evidence claims: 0 P0, 0 P1.
- Closure evidence:
  - `git status --short` shows the required shader asset staged as `A  App/Assets/Engine/Shaders/RHIImGuiOverlay.hlsl`; no untracked required runtime asset remains.
  - `git diff --name-only -- "Runtime/*/Gen/*" "Project/*/Gen/*"` returned no generated edits.
  - `git diff --check` reported LF-to-CRLF warnings only; no whitespace errors were reported.
  - Build: `cmake --build Build --target NullusUnitTests --config Debug -- /m:1 /p:CL_MPCount=1 /v:m` passed.
  - Latest focused validation: `Build\bin\Debug\NullusUnitTests.exe --gtest_filter=UiDrawDataSnapshotTests.*:RHIUiOverlayPassTests.*:RHIUiTextureRegistryTests.*:ThreadedRenderingLifecycleTests.*:FrameGraphSceneTargetsTests.*:ProfilerDestinationTest.*:RHIUiOverlaySourceGuardTests.*:UIAndToolingBackendAwarenessTests.* --gtest_break_on_failure=1` passed 424/424 in 8.882 seconds.
  - Registered UI textures now require an explicit synchronization scope. `UiTextureSynchronizationScope::PreviousFrameOrStatic` is used by the migrated UI preview path, and `SameFrameProducer` is rejected by the registry with a failing regression test.
  - Note: `T061` remains scoped to the current migrated run plus code/unit telemetry. It does not claim a steady-state FPS baseline or a reproduced pre-migration performance baseline.

## 2026-06-15 - Merge Gate P1 Closure Refresh

- Multi-agent merge review P1 fixes:
  - UIOverlay swapchain resource access is materialized during RHI recording from the acquired `frameContext.swapchainBackbufferView`. The materialized pass input and incoming dependency edges now carry the concrete backbuffer texture before dependency visibility barriers and `BeginPassCommandPlan`.
  - `TimelineProfilerSink` trace export state is owned per sink instance instead of a process-level static session, preventing multiple profiler panels or validation exporters from sharing path/frame-count state.
  - Registered `PreviousFrameOrStatic` UI textures are rejected unless their texture is already in `ShaderRead` state. `SameFrameProducer` remains rejected until producer dependency metadata is implemented.
  - Evidence wording was tightened so UE4.27 parallel draw references are recorded as adjacent benchmark evidence, not an exact UI overlay/present-ownership benchmark.
- TDD red checks:
  - `RHIUiOverlayPassTests.RhiThreadUiOverlayPassInputMaterializesSwapchainBackbufferBeforeVisibility` failed before implementation because no materialization helper or call site existed.
  - `ProfilerDestinationTest.TimelineTraceExporterStateIsOwnedPerSinkInstance` failed before implementation because two sink instances observed the same static export session.
  - `RHIUiTextureRegistryTests.RejectsPreviousFrameStaticTextureViewsThatAreNotShaderReadable` failed before implementation because render-target-state views were accepted as previous-frame/static UI textures.
- Build:
  - `cmake --build Build --target NullusUnitTests --config Debug -- /m:1 /p:CL_MPCount=1 /p:UseSharedCompilation=false /nr:false /v:m`
  - Result: passed.
- Focused validation:
  - `Build\bin\Debug\NullusUnitTests.exe --gtest_filter="RHIUiOverlaySourceGuardTests.*:RHIUiOverlayPassTests.*:RHIUiTextureRegistryTests.*:UiDrawDataSnapshotTests.*:UIAndToolingBackendAwarenessTests.*:ProfilerDestinationTest.TimelineTraceExporter*:ProfilerDestinationTest.DX12UiBridgeDirectSubmitSourceIsRemoved:RHITypesTests.SwapchainDescDefaultsToTripleBuffering:DX12PresentPolicyTests.*:PanelWindowHookTests.ProfilerPanelDrawDoesNotAdvanceTimelineFrameInsideActiveScope" --gtest_break_on_failure=1`
  - Result: passed 113/113.
