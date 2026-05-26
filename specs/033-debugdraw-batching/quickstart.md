# Quickstart: DebugDrawPass Line Batching

## Focused Unit Validation

```powershell
cmake --build .\Build --target NullusUnitTests --config Release
.\Build\bin\Release\NullusUnitTests.exe --gtest_filter=DebugDrawPassTests.*:DebugDrawGeometryTests.*:DebugDrawTypesTests.*
```

Expected result: all focused DebugDraw tests pass, including tests that verify compatible lines are grouped and incompatible line states split into separate batches.

## Diff Sanity

```powershell
git diff --check
```

Expected result: no whitespace errors.

## Optional Runtime Evidence

For runtime performance confirmation after a successful build, capture a selected-object frame in the editor and compare Debug Draw CPU time before/after. If RenderDoc is used, validate only the backend captured and do not generalize the result to other graphics backends.
