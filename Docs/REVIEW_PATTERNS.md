# Review Patterns

This file records project-specific review patterns that should be checked before `plan-review` on larger Nullus changes.

## JobSystem

- Callback exception boundaries: foreground, background, combine, cancel, cleanup, and continuation callbacks must be caught at the scheduler boundary and reported through `JobViolationKind::CallbackException`.
- Terminal cleanup versus cancel semantics: payload cleanup must run at most once, and successful or started-failure grouped jobs must not run per-job cancel callbacks after ownership has moved to the group terminal cleanup path.
- Cross-queue wait/help: foreground and background queues must avoid synchronous waits that can include the currently executing job or another non-terminal background job on the same background lane.
- Scoped wait helping: `Complete(backgroundHandle)` may help foreground dependencies in that background handle's dependency chain, but it must not pump foreground dependencies owned only by unrelated background jobs.
- Shutdown ordering: immediate shutdown must not report a group as terminal while a combine or completion callback is still running.
- Binding ABI: C-facing structs must keep `structSize`, `version`, plain fields, named constants, and deterministic invalid-handle/version errors.
- Generated code: files under `Runtime/*/Gen/` are generated output and must not be hand-edited.

Useful grep:

```powershell
rg -n "CallbackException|terminalCleanup|completeCallbackRunning|CompleteNoClear|NLS_JOB_BINDING_VERSION|Runtime/.*/Gen" Runtime Tests Docs specs
```
