# Quickstart: Object Graph Serialization Validation

## Goal

Validate the Object Graph serialization feature incrementally while keeping Editor and Game buildable.

## Expected Development Flow

1. Start from `specs/018-object-serialization/tasks.md`.
2. For each behavior-changing task, write the focused test first.
3. Confirm the test fails for the expected reason.
4. Implement the smallest matching runtime change.
5. Re-run the focused test.
6. Re-run the broader target listed in the task checkpoint.

## First Checkpoint: GUID Foundation

Focused tests:

- `Guid::New` creates non-empty values.
- deterministic test generation is stable.
- canonical formatting is lowercase UUID text.
- parse/format round-trips.
- invalid strings fail.
- GUID works as a hash key.

Suggested command:

```powershell
cmake --build build --config Debug --target NullusUnitTests -- /m:1
```

## Second Checkpoint: Runtime Identity Cleanup

Focused tests:

- `Scene` is a reflected object.
- `GameObject` construction does not require actor ID or playing-state reference.
- no persistent `worldID` field is emitted by new Object Graph scene writer.
- picking uses a transient registry instead of persistent object identity.

Suggested validation:

```powershell
cmake --build build --config Debug --target NullusUnitTests ReflectionTest -- /m:1
.\build\bin\Debug\ReflectionTest.exe
```

## Third Checkpoint: Object Graph Documents

Focused tests:

- empty scene document round-trips.
- scene with game objects and components round-trips.
- parent references resolve after all objects are created.
- component owner/ownership is restored.
- duplicate object IDs and invalid GUIDs produce diagnostics.
- deterministic save output matches golden fixture.

Suggested validation:

```powershell
cmake --build build --config Debug --target NullusUnitTests -- /m:1
ctest --test-dir build -C Debug --output-on-failure -R NullusUnitTests
```

## Fourth Checkpoint: Prefabs

Focused tests:

- prefab document saves a root game object graph.
- prefab instance creates new ObjectIds.
- source-to-instance ObjectId mapping is preserved.
- property override survives save/load.
- insert/remove/move owned-object overrides are validated.
- nested/variant prefab composition reports invalid source targets.

## Final Evidence

Validated on Windows Debug in this workspace:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022"
cmake --build build --config Debug --target NullusUnitTests ReflectionTest Editor -- /m:1 /v:minimal
.\Build\bin\Debug\NullusUnitTests.exe
ctest --test-dir build -C Debug --output-on-failure -R NullusUnitTests
.\Build\bin\Debug\ReflectionTest.exe
```

Observed result:

- `NullusUnitTests`: 518 tests, 517 passed, 1 skipped.
- `ctest -R NullusUnitTests`: 1/1 test passed.
- `ReflectionTest`: all reflection registration checks passed.
- `Editor` built in Debug.
- Deterministic scene and prefab golden-file tests passed as part of `NullusUnitTests`.
- Platform coverage is Windows Debug only unless separately validated.

## Non-Goals For This Feature

- Runtime support for old `GameobjectSerialize.cpp` scene format.
- Automatic old-scene migration on load.
- Hand-editing generated reflection output.
- Treating cooked/binary format as a separate semantic model.
