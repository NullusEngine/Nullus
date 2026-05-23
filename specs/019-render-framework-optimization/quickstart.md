# Quickstart: Render Framework Optimization

## Working Loop

1. Open `specs/019-render-framework-optimization/tasks.md`.
2. Pick the first unchecked task by priority and dependency.
3. Write or adjust a focused test and confirm it fails for the intended reason.
4. Implement the smallest production change that passes the test.
5. Run targeted tests, then broader relevant tests, then full `NullusUnitTests` when practical.
6. Run `git diff --check`.
7. Mark the task complete only after evidence is recorded in `tasks.md`.

## Common Commands

```powershell
cmake --build Build --target NullusUnitTests --config Debug
Build\bin\Debug\NullusUnitTests.exe --gtest_filter=<RelevantSuite>.<RelevantTest>
Build\bin\Debug\NullusUnitTests.exe
git diff --check
```

## Known Validation Blocker

If Debug build fails with `LNK1168` on `NLS_Renderd.dll`, check for a running editor:

```powershell
Get-Process Editor -ErrorAction SilentlyContinue | Select-Object ProcessName,Id,Path
```

Close the editor or switch to an unlocked configuration before rerunning validation.
