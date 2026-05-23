# Quickstart: Reflection-Driven Component Search Panel

## Goal

Verify that the Inspector can discover addable components through reflection, filter them with search, and add them safely to the selected actor.

## Suggested Implementation Order

1. Extract component discovery and addability evaluation into testable editor-side logic.
2. Add focused unit tests for reflected component filtering, duplicate exclusion, and display/search normalization.
3. Replace the current Inspector combo-driven add flow with the searchable picker UI.
4. Wire picker confirmation to the existing `GameObject::AddComponent(meta::Type)` path.
5. Re-run tests and perform editor verification.

## Automated Validation

Run the targeted unit suite after implementation:

```powershell
cmake --build <build-dir> --target NullusUnitTests
ctest --test-dir <build-dir> --output-on-failure -R NullusUnitTests
```

If test filtering is added for the new picker tests, a narrower command is also acceptable:

```powershell
<build-dir>\bin\NullusUnitTests.exe --gtest_filter=*ComponentPicker*
```

## Manual Editor Verification

1. Launch `Editor`.
2. Select an actor in the scene hierarchy.
3. Click `Add Component`.
4. Confirm the picker shows a reflection-driven list rather than the old fixed dropdown.
5. Search for a component that was not previously exposed by the hardcoded menu and add it.
6. Confirm the component appears immediately in the Inspector.
7. Search for an already-present singleton-style component and confirm it is excluded or unavailable.
8. Enter a query with no matches and confirm a clear empty-state message is shown.
9. Repeat on another actor to confirm the available list updates with actor context.

## Evidence To Capture In Final Validation Note

- Exact unit test command(s) run and their results.
- Exact manual editor scenarios completed.
- Any platform/backend limitation on the verification performed.
