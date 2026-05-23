# Main Realignment Note (2026-04-14)

## Summary

`main` briefly advanced to commit `8c50c60` (`fix: stabilize runtime backend startup and resize flow`).
That selective-port merge path was later confirmed to be incorrect for this feature line.

The accepted resolution was:

- treat `002-rhi-framework-cleanup` tip `64e7def` as the authoritative final content
- realign `main` so its tracked file tree matches that branch state exactly
- keep the repair as a normal forward commit instead of rewriting remote history

The realignment commit on `main` is:

- `b709208` `fix: realign main to final 002-rhi-framework-cleanup state`

## Current Baseline

After `b709208`, the tracked content on `main` matches:

- branch: `002-rhi-framework-cleanup`
- tip commit used as source tree: `64e7def`

For future follow-up work on this feature, treat `b709208` / `64e7def` content as the baseline,
not the earlier selective-port state from `8c50c60`.

## Verification Run After Realignment

The following commands were re-run after `main` was realigned:

```powershell
cmake -S . -B build
cmake --build build --config Debug -- /m:1
ctest --test-dir build -C Debug --output-on-failure
powershell -ExecutionPolicy Bypass -File .\verify_all_backends.ps1
```

Observed backend smoke results:

- `dx12`: Editor/Game `StillRunning`
- `vulkan`: Editor/Game `StillRunning`
- `dx11`: Editor `Exited 0`, Game `Exited 1`
- `opengl`: Editor `Exited 0`, Game `Exited 1`

## Working Guidance

- If someone needs the "last known good" state for this line, use `b709208` on `main`.
- If someone needs the original feature-branch endpoint, use `64e7def` on `002-rhi-framework-cleanup`.
- Do not use `8c50c60` as the content baseline for future follow-up changes.
