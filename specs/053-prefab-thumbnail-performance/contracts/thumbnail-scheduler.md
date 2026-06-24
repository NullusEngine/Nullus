# Contract: Thumbnail Scheduler

## Scope

This contract defines the observable behavior of thumbnail request scheduling, deduplication, cancellation, prioritization, and frame budgeting.

## Required States

- Missing
- Queued
- Preparing
- WaitingForResources
- Rendering
- WaitingForGpu
- Readback
- Ready
- Failed
- Cancelled

## Scheduling Rules

- Requests with the same thumbnail key share one active generation task.
- Visible Project Browser items are higher priority than prefetch items.
- Prefetch items are higher priority than low-priority background items.
- Queued work that is no longer visible may be cancelled before GPU submission.
- GPU-submitted work may be deprioritized even when it cannot be cancelled.
- The editor must keep showing a placeholder, asset icon, or prior thumbnail while a request is pending.

## Frame Budget Rules

- `ThumbnailGenerationBudget` is the configuration authority for scheduler tests and editor defaults.
- Preview render count per frame is bounded by the active `previewRenderCountBudget`.
- Readback count per frame is bounded by the active `readbackCountBudget`.
- Cache write count per frame is bounded by the active `cacheWriteCountBudget`.
- Polling an already-submitted preview readback must not consume preview render or readback submission budget.
- Tests may inject deterministic budget values and must assert that completed work does not exceed those values in a single frame.

## Non-Blocking Rules

- The editor main thread must not wait synchronously for GPU thumbnail fences in normal thumbnail generation.
- Readback completion must be polled or resumed on later frames.
- Encoding and disk writes must not block the main thread for GPU preview thumbnails.
- GPU preview thumbnails may remain `Pending` while readback, encoding, and disk metadata writes finish on later frames or background work.

## Shutdown Rules

- Editor shutdown must be able to cancel queued work or allow in-flight work to finish safely.
- No stale work may continue indefinitely after the owning editor context is gone.

## Acceptance Rules

- Same-key concurrent requests generate at most one active task.
- Rapid scrolling results in bounded queued work, not unbounded accumulation.
- A thumbnail that has not completed may still be replaced by a later fresh request.
- Failed tasks retry only within a finite policy and do not spin forever.
